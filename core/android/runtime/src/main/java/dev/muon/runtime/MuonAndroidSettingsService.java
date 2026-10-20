/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

package dev.muon.runtime;

import android.app.Activity;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Looper;
import android.provider.Settings;
import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;
import org.json.JSONTokener;
import java.util.Iterator;
import java.util.function.Predicate;

/** Opens explicitly selected Android settings screens for this application. */
final class MuonAndroidSettingsService implements AutoCloseable {
    @Nullable private Activity activity;
    private boolean resumed;
    private final Predicate<String> channelExists;

    MuonAndroidSettingsService(@NonNull Activity activity, @NonNull Predicate<String> channelExists) {
        this.activity = activity;
        this.channelExists = channelExists;
    }

    void setResumed(boolean value) { resumed = value; }

    void invoke(@NonNull String path, @NonNull JSONArray arguments,
            @NonNull byte[][] attachments, @NonNull MuonAndroidPlatformService.Completion completion) {
        try {
            Activity current = requireActivity();
            if (arguments.length() != 1 || !(arguments.get(0) instanceof String) || attachments.length != 0) {
                throw new IllegalArgumentException(path + " requires one JSON string");
            }
            JSONTokener parser = new JSONTokener(arguments.getString(0));
            Object parsed = parser.nextValue();
            if (!(parsed instanceof JSONObject) || parser.nextClean() != 0) {
                throw new IllegalArgumentException("Settings arguments must be a JSON object");
            }
            JSONObject options = (JSONObject) parsed;
            boolean open = path.equals("muon.android.settings.open");
            if (!open && !path.equals("muon.android.settings.isAvailable")) {
                throw new IllegalArgumentException("Unknown Android settings function: " + path);
            }
            for (Iterator<String> keys = options.keys(); keys.hasNext();) {
                String key = keys.next();
                if (!key.equals("target") && !key.equals("channelId") && !(open && key.equals("fallback"))) {
                    throw new IllegalArgumentException("Unknown settings option: " + key);
                }
            }
            Object targetValue = options.opt("target");
            if (!"app-details".equals(targetValue) && !"notifications".equals(targetValue)
                    && !"notification-channel".equals(targetValue)) {
                throw new IllegalArgumentException("Settings target must be app-details, notifications or notification-channel");
            }
            String requested = (String) targetValue;
            String channelId = null;
            Object requestedTarget = requested;
            if (requested.equals("notification-channel")) {
                Object value = options.opt("channelId");
                if (!(value instanceof String) || ((String) value).matches("[\\s\\p{Z}\\uFEFF]*")
                        || ((String) value).length() > 128 || ((String) value).indexOf('\0') >= 0) {
                    throw new IllegalArgumentException("A valid notification channelId is required");
                }
                channelId = (String) value;
                requestedTarget = new JSONObject().put("type", "notification-channel").put("channelId", channelId);
            } else if (options.has("channelId")) {
                throw new IllegalArgumentException("channelId is only valid for notification-channel settings");
            }
            boolean fallback = options.has("fallback");
            if (fallback && !"app-details".equals(options.get("fallback"))) {
                throw new IllegalArgumentException("Settings fallback must be app-details");
            }
            Intent intent = intent(current, requested, channelId);
            if (!open) {
                completion.completeBoolean(available(current, intent));
                return;
            }
            if (!resumed) {
                throw new IllegalStateException("Opening Android settings requires a foreground Activity");
            }
            String selected = requested;
            if (!available(current, intent)) {
                if (!fallback || requested.equals("app-details")) {
                    throw new ActivityNotFoundException("Android settings target is unavailable: " + requested);
                }
                selected = "app-details";
                intent = intent(current, selected, null);
            }
            if (!available(current, intent)) {
                throw new ActivityNotFoundException("Android settings target is unavailable: " + selected);
            }
            try {
                current.startActivity(intent);
            } catch (ActivityNotFoundException error) {
                // The handler can disappear after the availability check.
                if (!fallback || selected.equals("app-details")) { throw error; }
                selected = "app-details";
                Intent replacement = intent(current, selected, null);
                if (!available(current, replacement)) { throw error; }
                current.startActivity(replacement);
            }
            completion.completeString(new JSONObject().put("requestedTarget", requestedTarget)
                    .put("target", selected.equals(requested) ? requestedTarget : selected)
                    .put("fallbackUsed", !requested.equals(selected)).toString());
        } catch (JSONException | RuntimeException error) {
            String diagnostic = error.getMessage();
            completion.fail(diagnostic == null ? error.getClass().getSimpleName() : diagnostic);
        }
    }

    @Nullable private Intent intent(Activity activity, String target, @Nullable String channelId) {
        if (target.equals("app-details")) {
            return new Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
                    Uri.fromParts("package", activity.getPackageName(), null));
        }
        if (target.equals("notification-channel")) {
            return Build.VERSION.SDK_INT < 26 || !channelExists.test(channelId) ? null
                    : new Intent(Settings.ACTION_CHANNEL_NOTIFICATION_SETTINGS)
                            .putExtra(Settings.EXTRA_APP_PACKAGE, activity.getPackageName())
                            .putExtra(Settings.EXTRA_CHANNEL_ID, channelId);
        }
        // The public per-application notification settings action starts at API 26.
        return Build.VERSION.SDK_INT < 26 ? null
                : new Intent(Settings.ACTION_APP_NOTIFICATION_SETTINGS)
                        .putExtra(Settings.EXTRA_APP_PACKAGE, activity.getPackageName());
    }

    private static boolean available(Activity activity, @Nullable Intent intent) {
        return intent != null && intent.resolveActivity(activity.getPackageManager()) != null;
    }

    private Activity requireActivity() {
        if (Looper.myLooper() != Looper.getMainLooper()) {
            throw new IllegalStateException("Android settings operations require the main thread");
        }
        if (activity == null || activity.isFinishing() || activity.isDestroyed()) {
            throw new IllegalStateException("Android settings Activity is unavailable");
        }
        return activity;
    }

    @Override public void close() { resumed = false; activity = null; }
}

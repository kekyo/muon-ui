/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

package dev.muon.runtime;

import android.Manifest;
import android.app.Activity;
import android.app.NotificationManager;
import android.content.Context;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Looper;
import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

/** Owns one Activity's notification permission request and reads current OS state. */
final class MuonAndroidNotificationPermissionService implements AutoCloseable {
    private static int nextRequestCode = 0x4d00;
    private static final String REQUESTED = "permission-requested";
    private final boolean enabled;
    private final SharedPreferences preferences;
    @Nullable private Activity activity;
    private boolean resumed;
    private int requestCode;
    private int pendingCallId;
    @Nullable private MuonAndroidPlatformService.Completion pending;

    MuonAndroidNotificationPermissionService(@NonNull Activity activity, boolean enabled) {
        this.activity = activity;
        this.enabled = enabled;
        preferences = activity.getSharedPreferences("muon.notifications", Context.MODE_PRIVATE);
    }

    void setResumed(boolean value) { resumed = value; }

    void invoke(int callId, @NonNull String path, @NonNull JSONArray arguments,
            @NonNull byte[][] attachments, @NonNull MuonAndroidPlatformService.Completion completion) {
        try {
            Activity current = requireActivity();
            if (!enabled) { throw new IllegalStateException("Android notifications are disabled"); }
            if (arguments.length() != 0 || attachments.length != 0) {
                throw new IllegalArgumentException(path + " does not accept arguments");
            }
            if (path.equals("muon.android.notifications.getPermissionState")) {
                completion.completeString(state(current).toString());
                return;
            }
            if (!path.equals("muon.android.notifications.requestPermission")) {
                throw new IllegalArgumentException("Unknown Android notification function: " + path);
            }
            if (!resumed) {
                throw new IllegalStateException("Notification permission requires a foreground Activity");
            }
            if (requestCode != 0) {
                throw new IllegalStateException("A notification permission request is already active");
            }
            if (Build.VERSION.SDK_INT < 33) {
                completion.completeString(outcome(current, "not-required"));
                return;
            }
            if (current.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS)
                    == PackageManager.PERMISSION_GRANTED) {
                completion.completeString(outcome(current, "granted"));
                return;
            }
            if (nextRequestCode == Integer.MAX_VALUE) {
                throw new IllegalStateException("Notification request identifiers are exhausted");
            }
            requestCode = nextRequestCode++;
            pendingCallId = callId;
            pending = completion;
            try {
                current.requestPermissions(new String[] { Manifest.permission.POST_NOTIFICATIONS }, requestCode);
                // This records an API invocation, not proof that Android displayed a dialog.
                preferences.edit().putBoolean(REQUESTED, true).apply();
            } catch (RuntimeException error) {
                requestCode = 0;
                pending = null;
                pendingCallId = 0;
                throw error;
            }
        } catch (JSONException | RuntimeException error) {
            String diagnostic = error.getMessage();
            completion.fail(diagnostic == null ? error.getClass().getSimpleName() : diagnostic);
        }
    }

    boolean onRequestPermissionsResult(int code, @NonNull String[] permissions, @NonNull int[] results) {
        if (requestCode == 0 || code != requestCode) { return false; }
        requestCode = 0;
        MuonAndroidPlatformService.Completion completion = pending;
        pending = null;
        pendingCallId = 0;
        if (completion == null || activity == null) { return true; }
        // Empty arrays mean interruption. A denied result does not identify how
        // the dialog was closed, or establish permanent denial.
        String result = "interrupted";
        for (int i = 0; i < permissions.length && i < results.length; i++) {
            if (Manifest.permission.POST_NOTIFICATIONS.equals(permissions[i])) {
                result = results[i] == PackageManager.PERMISSION_GRANTED ? "granted" : "not-granted";
                break;
            }
        }
        try {
            completion.completeString(outcome(activity, result));
        } catch (JSONException | RuntimeException error) {
            String diagnostic = error.getMessage();
            completion.fail(diagnostic == null ? error.getClass().getSimpleName() : diagnostic);
        }
        return true;
    }

    private JSONObject state(Activity current) throws JSONException {
        String permission = Build.VERSION.SDK_INT < 33 ? "not-required"
                : current.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS)
                        == PackageManager.PERMISSION_GRANTED ? "granted" : "denied";
        boolean notificationsEnabled = current.getSystemService(NotificationManager.class).areNotificationsEnabled();
        boolean requested = preferences.getBoolean(REQUESTED, false);
        boolean rationale = Build.VERSION.SDK_INT >= 33
                && current.shouldShowRequestPermissionRationale(Manifest.permission.POST_NOTIFICATIONS);
        // This is a suggested recovery path, never a permanent-denial classification.
        boolean settingsSuggested = !notificationsEnabled
                && (!permission.equals("denied") || (requested && !rationale));
        return new JSONObject().put("runtimePermission", permission)
                .put("notificationsEnabled", notificationsEnabled).put("requestedBefore", requested)
                .put("shouldShowRationale", rationale).put("settingsSuggested", settingsSuggested);
    }

    private String outcome(Activity current, String result) throws JSONException {
        return new JSONObject().put("state", state(current)).put("result", result).toString();
    }

    void cancel(int callId) {
        if (pendingCallId == callId) { cancelAll(); }
    }

    void cancelAll() {
        pending = null;
        pendingCallId = 0;
        // The OS dialog cannot be cancelled here. Keep its request code reserved
        // until its callback arrives, so page reload cannot start a competing request.
    }

    private Activity requireActivity() {
        if (Looper.myLooper() != Looper.getMainLooper()) {
            throw new IllegalStateException("Notification permission operations require the main thread");
        }
        if (activity == null || activity.isFinishing() || activity.isDestroyed()) {
            throw new IllegalStateException("Notification permission Activity is unavailable");
        }
        return activity;
    }

    @Override public void close() {
        cancelAll();
        requestCode = 0;
        resumed = false;
        activity = null;
    }
}

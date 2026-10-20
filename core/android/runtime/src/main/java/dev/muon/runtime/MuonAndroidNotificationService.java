/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

package dev.muon.runtime;

import android.Manifest;
import android.app.Activity;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Looper;
import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.core.app.NotificationCompat;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;
import org.json.JSONTokener;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;
import java.util.Iterator;
import java.util.Map;

/** Posts application-owned local notifications independently of document lifetime. */
final class MuonAndroidNotificationService implements AutoCloseable {
    private static final int NOTIFICATION_ID = 1;
    private static final String CHANNELS_UNSUPPORTED = "Notification channels require Android API 26 or later";
    @Nullable private Activity activity;
    private final NotificationManager manager;
    private final boolean enabled;
    private final Map<String, JSONObject> staticChannels = new HashMap<>();
    @Nullable private String defaultCategory;
    private int smallIcon;

    MuonAndroidNotificationService(@NonNull Activity activity, @NonNull MuonAppConfig config) {
        this.activity = activity;
        manager = activity.getSystemService(NotificationManager.class);
        enabled = config.notificationsEnabled;
        if (!enabled) { return; }
        try {
            if (config.notifications.has("defaultCategory")) {
                defaultCategory = identifier(config.notifications.get("defaultCategory"), "defaultCategory");
            }
            String iconName = text(config.androidNotifications.get("smallIcon"), "smallIcon", 128, true);
            if (!iconName.equals("muon_notification_small")) {
                throw new IllegalArgumentException("Unsupported notification small icon resource name");
            }
            var application = activity.getPackageManager().getApplicationInfo(
                    activity.getPackageName(), PackageManager.GET_META_DATA);
            smallIcon = application.metaData == null ? 0
                    : application.metaData.getInt("dev.muon.notifications.smallIcon", 0);
            if (smallIcon == 0 || !activity.getResources().getResourceTypeName(smallIcon).equals("drawable")
                    || !activity.getResources().getResourceEntryName(smallIcon).equals(iconName)) {
                throw new IllegalStateException("The notification small icon resource is unavailable");
            }
            JSONArray channels = config.androidNotifications.has("channels")
                    ? config.androidNotifications.getJSONArray("channels") : new JSONArray();
            for (int i = 0; i < channels.length(); i++) {
                JSONObject options = channelOptions(channels.getJSONObject(i));
                String id = options.getString("id");
                if (staticChannels.put(id, options) != null) {
                    throw new IllegalArgumentException("Duplicate notification channel: " + id);
                }
            }
            if (Build.VERSION.SDK_INT >= 26) {
                for (JSONObject options : staticChannels.values()) { ensureChannel(options); }
            }
        } catch (JSONException | PackageManager.NameNotFoundException error) {
            throw new IllegalStateException("Invalid Android notification configuration: " + error.getMessage(), error);
        }
    }

    void invoke(@NonNull String path, @NonNull JSONArray arguments, @NonNull byte[][] attachments,
            @NonNull MuonAndroidPlatformService.Completion completion) {
        try {
            Activity current = requireActivity();
            if (!enabled) { throw new IllegalStateException("Android notifications are disabled"); }
            if (attachments.length != 0) { throw new IllegalArgumentException("Notifications do not accept attachments"); }
            switch (path) {
                case "muon.notifications.post":
                    post(current, objectArgument(arguments));
                    completion.completeVoid();
                    return;
                case "muon.notifications.cancel":
                    manager.cancel(stringArgument(arguments, "id"), NOTIFICATION_ID);
                    completion.completeVoid();
                    return;
                case "muon.notifications.getStatus": {
                    JSONObject options = objectArgument(arguments);
                    keys(options, "category");
                    JSONObject category = null;
                    if (options.has("category")) {
                        String id = identifier(options.get("category"), "category");
                        requireCategory(id);
                        category = Build.VERSION.SDK_INT < 26
                                ? status("unknown", CHANNELS_UNSUPPORTED)
                                : channelEnabled(id) ? status("enabled", null)
                                        : status("disabled", "The notification channel is disabled");
                        category.put("id", id);
                    }
                    completion.completeString(new JSONObject().put("app", appStatus(current))
                            .put("category", category == null ? JSONObject.NULL : category).toString());
                    return;
                }
                case "muon.notifications.getCapabilities":
                    noArguments(arguments);
                    completion.completeString(new JSONObject()
                            .put("interaction", capability(false, "Notification events are not implemented"))
                            .put("launch", capability(false, "Notification launch events are not implemented"))
                            .put("persistence", capability(true, "Regular process exit is supported; force-stop is excluded"))
                            .put("actions", capability(false, "Notification actions are not implemented"))
                            .put("appStatus", capability(true, null))
                            .put("categoryStatus", capability(Build.VERSION.SDK_INT >= 26, CHANNELS_UNSUPPORTED)).toString());
                    return;
                case "muon.android.notifications.getCapabilities":
                    noArguments(arguments);
                    completion.completeString(new JSONObject().put("channels",
                            capability(Build.VERSION.SDK_INT >= 26, CHANNELS_UNSUPPORTED)).toString());
                    return;
                case "muon.android.notifications.ensureChannel":
                    requireChannels();
                    completion.completeString(channelState(ensureChannel(channelOptions(objectArgument(arguments)))).toString());
                    return;
                case "muon.android.notifications.getChannel": {
                    requireChannels();
                    NotificationChannel channel = manager.getNotificationChannel(stringArgument(arguments, "id"));
                    completion.completeString(channel == null ? "null" : channelState(channel).toString());
                    return;
                }
                default:
                    throw new IllegalArgumentException("Unknown notification function: " + path);
            }
        } catch (JSONException | RuntimeException error) {
            completion.fail(error.getMessage() == null ? error.getClass().getSimpleName() : error.getMessage());
        }
    }

    private void post(Activity current, JSONObject options) throws JSONException {
        keys(options, "id", "title", "body", "category", "data");
        String id = identifier(options.get("id"), "id");
        String title = text(options.get("title"), "title", 1024, true);
        String body = text(options.get("body"), "body", 4096, false);
        String category = options.has("category") ? identifier(options.get("category"), "category") : defaultCategory;
        if (category == null) { throw new IllegalArgumentException("A notification category or defaultCategory is required"); }
        requireCategory(category);
        Object data = options.has("data") ? options.get("data") : JSONObject.NULL;
        validateData(data, 0);
        String wrapped = new JSONArray().put(data).toString();
        if (wrapped.substring(1, wrapped.length() - 1).getBytes(StandardCharsets.UTF_8).length > 16384) {
            throw new IllegalArgumentException("Notification data exceeds 16384 UTF-8 bytes");
        }
        JSONObject app = appStatus(current);
        if (app.getString("state").equals("disabled")) {
            throw new IllegalStateException(app.getString("reason"));
        }
        if (Build.VERSION.SDK_INT >= 26 && !channelEnabled(category)) {
            throw new IllegalStateException("The notification channel is disabled: " + category);
        }
        Intent intent = new Intent(current, current.getClass())
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TOP | Intent.FLAG_ACTIVITY_SINGLE_TOP);
        PendingIntent content = PendingIntent.getActivity(current, 0, intent,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
        var builder = new NotificationCompat.Builder(current, category)
                .setSmallIcon(smallIcon).setContentTitle(title).setContentText(body)
                .setContentIntent(content).setAutoCancel(true);
        if (Build.VERSION.SDK_INT < 26) {
            String importance = staticChannels.get(category).getString("importance");
            builder.setPriority(importance.equals("min") ? NotificationCompat.PRIORITY_MIN
                    : importance.equals("low") ? NotificationCompat.PRIORITY_LOW
                    : importance.equals("high") ? NotificationCompat.PRIORITY_HIGH : NotificationCompat.PRIORITY_DEFAULT);
        }
        // Returning means the platform submission call completed, not that the user saw it.
        manager.notify(id, NOTIFICATION_ID, builder.build());
    }

    private JSONObject appStatus(Activity current) throws JSONException {
        if (Build.VERSION.SDK_INT >= 33
                && current.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
            return status("disabled", "Notification runtime permission is not granted");
        }
        return manager.areNotificationsEnabled() ? status("enabled", null)
                : status("disabled", "Application notifications are disabled");
    }

    boolean hasChannel(String id) {
        // Settings remain available even when notification APIs are disabled.
        return Build.VERSION.SDK_INT >= 26 && manager.getNotificationChannel(id) != null;
    }

    private void requireCategory(String id) {
        if (Build.VERSION.SDK_INT >= 26 ? manager.getNotificationChannel(id) == null : !staticChannels.containsKey(id)) {
            throw new IllegalArgumentException("Unregistered notification category: " + id);
        }
    }

    private boolean channelEnabled(String id) {
        NotificationChannel channel = manager.getNotificationChannel(id);
        return channel != null && channel.getImportance() != NotificationManager.IMPORTANCE_NONE;
    }

    private NotificationChannel ensureChannel(JSONObject options) throws JSONException {
        String id = options.getString("id");
        NotificationChannel existing = manager.getNotificationChannel(id);
        // Re-registration can lower importance on Android. Leave every existing setting intact.
        if (existing != null) { return existing; }
        String importance = options.getString("importance");
        NotificationChannel channel = new NotificationChannel(id, options.getString("name"),
                importance.equals("min") ? NotificationManager.IMPORTANCE_MIN
                        : importance.equals("low") ? NotificationManager.IMPORTANCE_LOW
                        : importance.equals("high") ? NotificationManager.IMPORTANCE_HIGH : NotificationManager.IMPORTANCE_DEFAULT);
        channel.setDescription(options.getString("description"));
        manager.createNotificationChannel(channel);
        NotificationChannel created = manager.getNotificationChannel(id);
        if (created == null) { throw new IllegalStateException("Android did not register notification channel: " + id); }
        return created;
    }

    private static JSONObject channelState(NotificationChannel channel) throws JSONException {
        int importance = channel.getImportance();
        return new JSONObject().put("id", channel.getId()).put("name", channel.getName().toString())
                .put("description", channel.getDescription() == null ? "" : channel.getDescription())
                .put("importance", importance == NotificationManager.IMPORTANCE_NONE ? "none"
                        : importance == NotificationManager.IMPORTANCE_MIN ? "min"
                        : importance == NotificationManager.IMPORTANCE_LOW ? "low"
                        : importance >= NotificationManager.IMPORTANCE_HIGH ? "high" : "default")
                .put("enabled", importance != NotificationManager.IMPORTANCE_NONE);
    }

    private static JSONObject channelOptions(JSONObject options) throws JSONException {
        keys(options, "id", "name", "description", "importance");
        String importance = text(options.get("importance"), "importance", 16, true);
        if (!importance.equals("min") && !importance.equals("low")
                && !importance.equals("default") && !importance.equals("high")) {
            throw new IllegalArgumentException("Channel importance must be min, low, default or high");
        }
        return new JSONObject().put("id", identifier(options.get("id"), "id"))
                .put("name", text(options.get("name"), "name", 128, true))
                .put("description", options.has("description")
                        ? text(options.get("description"), "description", 1024, false) : "")
                .put("importance", importance);
    }

    private static void requireChannels() {
        if (Build.VERSION.SDK_INT < 26) { throw new UnsupportedOperationException(CHANNELS_UNSUPPORTED); }
    }

    private static JSONObject status(String state, @Nullable String reason) throws JSONException {
        return new JSONObject().put("state", state).put("reason", reason == null ? JSONObject.NULL : reason);
    }

    private static JSONObject capability(boolean supported, @Nullable String reason) throws JSONException {
        return status(supported ? "supported" : "unsupported", supported ? null : reason);
    }

    private static void noArguments(JSONArray arguments) {
        if (arguments.length() != 0) { throw new IllegalArgumentException("This notification function takes no arguments"); }
    }

    private static String stringArgument(JSONArray arguments, String name) throws JSONException {
        if (arguments.length() != 1) { throw new IllegalArgumentException("This notification function requires one string"); }
        return identifier(arguments.get(0), name);
    }

    private static JSONObject objectArgument(JSONArray arguments) throws JSONException {
        if (arguments.length() != 1 || !(arguments.get(0) instanceof String)) {
            throw new IllegalArgumentException("This notification function requires one JSON string");
        }
        JSONTokener parser = new JSONTokener(arguments.getString(0));
        Object value = parser.nextValue();
        if (!(value instanceof JSONObject) || parser.nextClean() != 0) {
            throw new IllegalArgumentException("Notification arguments must be a JSON object");
        }
        return (JSONObject) value;
    }

    private static String identifier(Object value, String name) { return text(value, name, 128, true); }

    private static String text(Object value, String name, int maximum, boolean required) {
        if (!(value instanceof String)) { throw new IllegalArgumentException(name + " must be a string"); }
        String string = (String) value;
        // Match JavaScript trim's whitespace set while preserving the supplied value.
        if (string.length() > maximum || string.indexOf('\0') >= 0
                || (required && string.matches("[\\s\\p{Z}\\uFEFF]*"))) {
            throw new IllegalArgumentException("Invalid notification " + name);
        }
        return string;
    }

    private static void keys(JSONObject object, String... allowed) {
        for (Iterator<String> keys = object.keys(); keys.hasNext();) {
            String key = keys.next();
            boolean found = false;
            for (String candidate : allowed) { if (candidate.equals(key)) { found = true; break; } }
            if (!found) { throw new IllegalArgumentException("Unknown notification option: " + key); }
        }
    }

    private static void validateData(Object value, int depth) throws JSONException {
        if (value instanceof JSONObject || value instanceof JSONArray) {
            if (depth >= 64) { throw new IllegalArgumentException("Notification data exceeds 64 nested containers"); }
            if (value instanceof JSONObject) {
                JSONObject object = (JSONObject) value;
                for (Iterator<String> keys = object.keys(); keys.hasNext();) { validateData(object.get(keys.next()), depth + 1); }
            } else {
                JSONArray array = (JSONArray) value;
                for (int i = 0; i < array.length(); i++) { validateData(array.get(i), depth + 1); }
            }
        } else if (value instanceof Number && !Double.isFinite(((Number) value).doubleValue())) {
            throw new IllegalArgumentException("Notification data numbers must be finite");
        }
    }

    private Activity requireActivity() {
        if (Looper.myLooper() != Looper.getMainLooper()) { throw new IllegalStateException("Notifications require the main thread"); }
        if (activity == null || activity.isDestroyed() || activity.isFinishing()) {
            throw new IllegalStateException("Notification Activity is unavailable");
        }
        return activity;
    }

    @Override public void close() { activity = null; }
}

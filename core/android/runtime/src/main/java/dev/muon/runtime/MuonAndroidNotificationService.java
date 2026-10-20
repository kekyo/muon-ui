/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

package dev.muon.runtime;

import android.Manifest;
import android.app.Activity;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Intent;
import android.content.Context;
import android.net.Uri;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Looper;
import android.util.Log;
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
import java.util.ArrayList;
import java.util.HashSet;
import java.util.UUID;
import java.util.function.Consumer;

/** Posts application-owned local notifications independently of document lifetime. */
final class MuonAndroidNotificationService implements AutoCloseable {
    private static final int NOTIFICATION_ID = 1;
    private static final String CHANNELS_UNSUPPORTED = "Notification channels require Android API 26 or later";
    @Nullable private Activity activity;
    private final NotificationManager manager;
    private final boolean enabled;
    private final Context context;
    @Nullable private final MuonNotificationRuntime runtime;
    private final Map<Integer, MuonAndroidPlatformService.Completion> asynchronous = new HashMap<>();
    private final Map<Integer, String> acquisitions = new HashMap<>();
    private final Map<String, Long> leases = new HashMap<>();
    private final Map<Integer, PendingWatch> pendingWatches = new HashMap<>();
    private final Runnable eventsChanged = this::readPendingWatches;
    private final Map<String, JSONObject> staticChannels = new HashMap<>();
    @Nullable private String defaultCategory;
    private int smallIcon;

    private static final class PendingWatch {
        final String token;
        final long afterCursor;
        final MuonAndroidPlatformService.Completion completion;
        boolean reading;
        PendingWatch(String token, long afterCursor, MuonAndroidPlatformService.Completion completion) {
            this.token = token;
            this.afterCursor = afterCursor;
            this.completion = completion;
        }
    }

    MuonAndroidNotificationService(@NonNull Activity activity, @NonNull MuonAppConfig config) {
        this.activity = activity;
        manager = activity.getSystemService(NotificationManager.class);
        enabled = config.notificationsEnabled;
        context = activity.getApplicationContext();
        runtime = enabled ? MuonNotificationRuntime.get(context) : null;
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

    void invoke(int callId, @NonNull String path, @NonNull JSONArray arguments, @NonNull byte[][] attachments,
            @NonNull MuonAndroidPlatformService.Completion completion) {
        try {
            Activity current = requireActivity();
            if (!enabled) { throw new IllegalStateException("Android notifications are disabled"); }
            if (attachments.length != 0) { throw new IllegalArgumentException("Notifications do not accept attachments"); }
            switch (path) {
                case "muon.notifications.post":
                    post(callId, current, objectArgument(arguments), completion);
                    return;
                case "muon.notifications.cancel": {
                    String id = stringArgument(arguments, "id");
                    execute(callId, store -> { store.cancel(id, () -> manager.cancel(id, NOTIFICATION_ID)); return null; },
                            completion, value -> completion.completeVoid());
                    return;
                }
                case "muon.notifications.getPendingEvents":
                    noArguments(arguments);
                    execute(callId, store -> store.snapshot(-1).getJSONArray("events"), completion,
                            value -> completion.completeString(value.toString()));
                    return;
                case "muon.notifications.acknowledgeEvent": {
                    String eventId = stringArgument(arguments, "eventId");
                    execute(callId, store -> { store.acknowledge(eventId); return null; }, completion,
                            value -> completion.completeVoid());
                    return;
                }
                case "muon.notifications.watchEvents":
                    watch(callId, objectArgument(arguments), completion);
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
                            .put("interaction", capability(true, null))
                            .put("launch", capability(true, null))
                            .put("persistence", capability(true, "Regular process exit is supported; force-stop is excluded"))
                            .put("actions", capability(true, null))
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

    private void post(int callId, Activity current, JSONObject options,
            MuonAndroidPlatformService.Completion completion) throws JSONException {
        keys(options, "id", "title", "body", "category", "data", "actions");
        String id = identifier(options.get("id"), "id");
        String title = text(options.get("title"), "title", 1024, true);
        String body = text(options.get("body"), "body", 4096, false);
        String category = options.has("category") ? identifier(options.get("category"), "category") : defaultCategory;
        if (category == null) { throw new IllegalArgumentException("A notification category or defaultCategory is required"); }
        requireCategory(category);
        Object data = options.has("data") ? options.get("data") : JSONObject.NULL;
        validateData(data, 0);
        String wrapped = new JSONArray().put(data).toString();
        String dataJson = wrapped.substring(1, wrapped.length() - 1);
        if (dataJson.getBytes(StandardCharsets.UTF_8).length > 16384) {
            throw new IllegalArgumentException("Notification data exceeds 16384 UTF-8 bytes");
        }
        JSONObject app = appStatus(current);
        if (app.getString("state").equals("disabled")) {
            throw new IllegalStateException(app.getString("reason"));
        }
        if (Build.VERSION.SDK_INT >= 26 && !channelEnabled(category)) {
            throw new IllegalStateException("The notification channel is disabled: " + category);
        }
        JSONArray actions = options.has("actions") ? options.getJSONArray("actions") : new JSONArray();
        if (actions.length() > 3) { throw new IllegalArgumentException("Notifications support at most 3 actions"); }
        HashSet<String> actionIds = new HashSet<>();
        for (int i = 0; i < actions.length(); i++) {
            JSONObject action = actions.getJSONObject(i);
            keys(action, "id", "title");
            String actionId = identifier(action.get("id"), "action id");
            text(action.get("title"), "action title", 128, true);
            if (!actionIds.add(actionId)) { throw new IllegalArgumentException("Notification action ids must be unique"); }
        }
        Class<? extends Activity> target = current.getClass();
        execute(callId, store -> {
            JSONObject status = appStatus(context);
            if (status.getString("state").equals("disabled")) { throw new IllegalStateException(status.getString("reason")); }
            if (Build.VERSION.SDK_INT >= 26 && !channelEnabled(category)) {
                throw new IllegalStateException("The notification channel is disabled: " + category);
            }
            store.post(id, dataJson, actions.toString(), token -> manager.notify(id, NOTIFICATION_ID,
                    createNotification(target, token, title, body, category, actions)));
            return null;
        }, completion, value -> completion.completeVoid());
    }

    private Notification createNotification(Class<? extends Activity> target, String token,
            String title, String body, String category, JSONArray actions) {
        var builder = new NotificationCompat.Builder(context, category)
                .setSmallIcon(smallIcon).setContentTitle(title).setContentText(body)
                // The operation is persisted before this runtime requests removal.
                .setContentIntent(operationIntent(target, token, null)).setAutoCancel(false);
        for (int i = 0; i < actions.length(); i++) {
            JSONObject action = actions.optJSONObject(i);
            builder.addAction(new NotificationCompat.Action.Builder(0, action.optString("title"),
                    operationIntent(target, token, action.optString("id"))).build());
        }
        if (Build.VERSION.SDK_INT < 26) {
            String importance = staticChannels.get(category).optString("importance");
            builder.setPriority(importance.equals("min") ? NotificationCompat.PRIORITY_MIN
                    : importance.equals("low") ? NotificationCompat.PRIORITY_LOW
                    : importance.equals("high") ? NotificationCompat.PRIORITY_HIGH : NotificationCompat.PRIORITY_DEFAULT);
        }
        return builder.build();
    }

    private PendingIntent operationIntent(Class<? extends Activity> target, String token, @Nullable String actionId) {
        Uri.Builder uri = new Uri.Builder().scheme("muon-notification").authority(context.getPackageName())
                .appendPath(token).appendPath(actionId == null ? "activate" : "action");
        if (actionId != null) { uri.appendPath(actionId); }
        // Extras do not distinguish PendingIntents. Revision and action belong in the data URI.
        Intent intent = new Intent(context, target).setData(uri.build())
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TOP | Intent.FLAG_ACTIVITY_SINGLE_TOP);
        return PendingIntent.getActivity(context, 0, intent, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
    }

    private JSONObject appStatus(Context current) throws JSONException {
        if (Build.VERSION.SDK_INT >= 33
                && current.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
            return status("disabled", "Notification runtime permission is not granted");
        }
        return manager.areNotificationsEnabled() ? status("enabled", null)
                : status("disabled", "Application notifications are disabled");
    }

    void onNotificationIntent(@Nullable Intent intent) {
        if (!enabled || intent == null || intent.getData() == null) { return; }
        Uri uri = intent.getData();
        if (!"muon-notification".equals(uri.getScheme()) || !context.getPackageName().equals(uri.getAuthority())
                || uri.getQuery() != null || uri.getFragment() != null) { return; }
        var parts = uri.getPathSegments();
        if (parts.size() != 2 && parts.size() != 3) { return; }
        String token = parts.get(0);
        String type = parts.get(1);
        String actionId = parts.size() == 3 ? parts.get(2) : null;
        try {
            if (!UUID.fromString(token).toString().equals(token)) { return; }
        } catch (IllegalArgumentException error) { return; }
        MuonNotificationRuntime applicationRuntime = runtime;
        applicationRuntime.activate(token, type, actionId,
                id -> manager.cancel(id, NOTIFICATION_ID), new MuonNotificationRuntime.Result() {
            @Override public void complete(Object result) {}
            @Override public void fail(String diagnostic) { Log.e("MuonNotifications", "Could not complete notification operation: " + diagnostic); }
        });
    }

    private void execute(int callId, MuonNotificationRuntime.Operation operation,
            MuonAndroidPlatformService.Completion completion, Consumer<Object> success) {
        asynchronous.put(callId, completion);
        runtime.execute(operation, new MuonNotificationRuntime.Result() {
            @Override public void complete(Object value) {
                if (asynchronous.get(callId) == completion) {
                    asynchronous.remove(callId);
                    success.accept(value);
                }
            }
            @Override public void fail(String diagnostic) {
                if (asynchronous.get(callId) == completion) {
                    asynchronous.remove(callId);
                    String token = acquisitions.remove(callId);
                    if (token != null) { releaseWatch(token); }
                    completion.fail(diagnostic);
                }
            }
        });
    }

    private void watch(int callId, JSONObject options, MuonAndroidPlatformService.Completion completion) throws JSONException {
        String operation = text(options.get("operation"), "watch operation", 16, true);
        if (operation.equals("acquire")) {
            keys(options, "operation");
            String token = UUID.randomUUID().toString();
            leases.put(token, 0L);
            acquisitions.put(callId, token);
            runtime.addListener(eventsChanged);
            execute(callId, store -> store.snapshot(-1).getLong("cursor"), completion, value -> {
                acquisitions.remove(callId);
                long cursor = (Long) value;
                leases.put(token, cursor);
                completion.completeString("{\"token\":\"" + token + "\",\"cursor\":" + cursor + "}");
            });
            return;
        }
        if (!operation.equals("next") && !operation.equals("release")) {
            throw new IllegalArgumentException("Unknown notification watch operation");
        }
        keys(options, operation.equals("next") ? new String[] { "operation", "token", "afterCursor" }
                : new String[] { "operation", "token" });
        String token = identifier(options.get("token"), "watch token");
        if (operation.equals("release")) {
            releaseWatch(token);
            completion.completeVoid();
            return;
        }
        if (!leases.containsKey(token)) { throw new IllegalArgumentException("Notification subscription is not active"); }
        Object after = options.opt("afterCursor");
        if (!(after instanceof Number) || !Double.isFinite(((Number) after).doubleValue())
                || ((Number) after).doubleValue() != ((Number) after).longValue()
                || ((Number) after).longValue() < 0 || ((Number) after).longValue() > 9007199254740991L) {
            throw new IllegalArgumentException("Notification cursor must be a nonnegative safe integer");
        }
        for (PendingWatch pending : pendingWatches.values()) {
            if (pending.token.equals(token)) { throw new IllegalArgumentException("Notification subscription already has a pending call"); }
        }
        pendingWatches.put(callId, new PendingWatch(token, ((Number) after).longValue(), completion));
        readPendingWatches();
    }

    private void readPendingWatches() {
        for (Map.Entry<Integer, PendingWatch> entry : new ArrayList<>(pendingWatches.entrySet())) {
            int callId = entry.getKey();
            PendingWatch pending = entry.getValue();
            if (pending.reading) { continue; }
            pending.reading = true;
            runtime.execute(store -> store.snapshot(pending.afterCursor), new MuonNotificationRuntime.Result() {
                @Override public void complete(Object value) {
                    if (pendingWatches.get(callId) != pending) { return; }
                    pending.reading = false;
                    JSONObject snapshot = (JSONObject) value;
                    long cursor = snapshot.optLong("cursor");
                    leases.put(pending.token, cursor);
                    if (cursor > pending.afterCursor) {
                        pendingWatches.remove(callId);
                        pending.completion.completeString(snapshot.toString());
                    }
                }
                @Override public void fail(String diagnostic) {
                    if (pendingWatches.get(callId) == pending) {
                        pendingWatches.remove(callId);
                        pending.completion.fail(diagnostic);
                    }
                }
            });
        }
    }

    private void releaseWatch(String token) {
        Long cursor = leases.remove(token);
        for (Map.Entry<Integer, PendingWatch> entry : new ArrayList<>(pendingWatches.entrySet())) {
            if (entry.getValue().token.equals(token)) {
                pendingWatches.remove(entry.getKey());
                entry.getValue().completion.completeString("{\"events\":null,\"cursor\":" + (cursor == null ? 0 : cursor) + "}");
            }
        }
        if (leases.isEmpty() && runtime != null) { runtime.removeListener(eventsChanged); }
    }

    void cancel(int callId) {
        asynchronous.remove(callId);
        pendingWatches.remove(callId);
        String token = acquisitions.remove(callId);
        if (token != null) { releaseWatch(token); }
    }

    void cancelAll() {
        asynchronous.clear();
        acquisitions.clear();
        pendingWatches.clear();
        leases.clear();
        if (runtime != null) { runtime.removeListener(eventsChanged); }
    }

    int getActiveWatchCount() { return leases.size(); }

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

    @Override public void close() { cancelAll(); activity = null; }
}

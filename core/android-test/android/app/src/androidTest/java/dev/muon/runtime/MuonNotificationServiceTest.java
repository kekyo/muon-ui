/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

package dev.muon.runtime;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;
import static org.junit.Assert.assertThrows;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.os.Build;
import androidx.annotation.NonNull;
import androidx.test.core.app.ActivityScenario;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.Test;
import org.junit.runner.RunWith;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

/** Exercises OS notification behavior without requiring a current WebView. */
@RunWith(AndroidJUnit4.class)
public final class MuonNotificationServiceTest {
    private static final class Result implements MuonAndroidPlatformService.Completion {
        final LinkedBlockingQueue<String> values = new LinkedBlockingQueue<>();
        @Override public void completeVoid() { values.add("void"); }
        @Override public void completeString(@NonNull String value) { values.add(value); }
        @Override public void fail(@NonNull String diagnostic) { values.add("error:" + diagnostic); }
        @Override public void completeUnsignedInteger(long value) { throw new AssertionError("Unexpected integer result"); }
        @Override public void completeBoolean(boolean value) { values.add(Boolean.toString(value)); }
        @Override public void completeBinary(@NonNull byte[] value) { throw new AssertionError("Unexpected binary result"); }
        String await() throws InterruptedException {
            String value = values.poll(30, TimeUnit.SECONDS);
            assertNotNull("Notification call must settle", value);
            return value;
        }
    }

    @Test
    public void postsUpdatesAndCancelsStaticCategoriesWithoutOsChannelsOnLegacyAndroid() throws Exception {
        if (Build.VERSION.SDK_INT >= 26) { return; }
        try (ActivityScenario<MuonWindowActivity> scenario = ActivityScenario.launch(MuonWindowActivity.class)) {
            AtomicReference<MuonAndroidNotificationService> reference = new AtomicReference<>();
            AtomicReference<NotificationManager> manager = new AtomicReference<>();
            scenario.onActivity(activity -> {
                reference.set(new MuonAndroidNotificationService(activity, MuonAppConfig.load(activity)));
                manager.set(activity.getSystemService(NotificationManager.class));
            });
            String[][] operations = {
                    { "post", "{\"id\":\"legacy-a\",\"title\":\"First\",\"body\":\"\"}" },
                    { "post", "{\"id\":\"legacy-b\",\"title\":\"Second\",\"body\":\"\",\"category\":\"quiet\"}" },
                    { "post", "{\"id\":\"legacy-a\",\"title\":\"Updated\",\"body\":\"\"}" },
                    { "cancel", "legacy-a" }, { "cancel", "legacy-b" }
            };
            for (int index = 0; index < operations.length; index++) {
                String[] operation = operations[index];
                Result result = new Result();
                scenario.onActivity(activity -> reference.get().invoke(0, "muon.notifications." + operation[0],
                        new JSONArray().put(operation[1]), new byte[0][], result));
                assertEquals("void", result.await());
                var active = manager.get().getActiveNotifications();
                assertEquals(new int[] { 1, 2, 2, 1, 0 }[index], active.length);
                for (var notification : active) {
                    assertNotNull(notification.getNotification().getSmallIcon());
                    assertNotNull(notification.getNotification().contentIntent);
                    // Notification removal must follow the durable operation commit.
                    assertEquals(0, notification.getNotification().flags & Notification.FLAG_AUTO_CANCEL);
                    if (notification.getTag().equals("legacy-a")) {
                        assertEquals(index == 2 ? "Updated" : "First",
                                notification.getNotification().extras.getString(Notification.EXTRA_TITLE));
                    } else {
                        assertEquals("legacy-b", notification.getTag());
                        assertEquals("Second", notification.getNotification().extras.getString(Notification.EXTRA_TITLE));
                    }
                }
            }
            for (String path : new String[] { "getChannel", "ensureChannel" }) {
                Result result = new Result();
                scenario.onActivity(activity -> reference.get().invoke(0, "muon.android.notifications." + path,
                        new JSONArray().put("general"), new byte[0][], result));
                assertTrue(result.await().contains("require Android API 26"));
            }
            Result status = new Result();
            scenario.onActivity(activity -> reference.get().invoke(0, "muon.notifications.getStatus",
                    new JSONArray().put("{\"category\":\"general\"}"), new byte[0][], status));
            assertEquals("unknown", new JSONObject(status.await()).getJSONObject("category").getString("state"));
            scenario.onActivity(activity -> reference.get().close());
        }
    }

    @Test
    public void rejectsAbsentDefaultUnknownCategoryAndDisabledNotifications() throws Exception {
        try (ActivityScenario<MuonWindowActivity> scenario = ActivityScenario.launch(MuonWindowActivity.class)) {
            AtomicReference<MuonAndroidNotificationService> reference = new AtomicReference<>();
            scenario.onActivity(activity -> {
                try {
                    JSONObject config = new JSONObject(MuonAppConfig.readAsset(activity, "muon/config.json"));
                    config.getJSONObject("notifications").remove("defaultCategory");
                    reference.set(new MuonAndroidNotificationService(activity, new MuonAppConfig(config)));
                } catch (Exception error) { throw new AssertionError(error); }
            });
            for (String category : new String[] { "", ",\"category\":\"missing-category\"" }) {
                Result result = new Result();
                scenario.onActivity(activity -> reference.get().invoke(0, "muon.notifications.post",
                        new JSONArray().put("{\"id\":\"invalid\",\"title\":\"Title\",\"body\":\"\"" + category + "}"),
                        new byte[0][], result));
                String error = result.await();
                assertTrue(error, error.contains(category.isEmpty() ? "defaultCategory is required" : "Unregistered"));
            }
            scenario.onActivity(activity -> {
                reference.get().close();
                try {
                    JSONObject config = new JSONObject(MuonAppConfig.readAsset(activity, "muon/config.json"));
                    config.getJSONObject("notifications").put("enabled", false);
                    config.put("androidNotifications", new JSONObject().put("channels", new JSONArray().put(
                            new JSONObject().put("id", "disabled-unregistered").put("name", "Disabled").put("importance", "low"))));
                    reference.set(new MuonAndroidNotificationService(activity, new MuonAppConfig(config)));
                } catch (Exception error) { throw new AssertionError(error); }
            });
            Result disabled = new Result();
            scenario.onActivity(activity -> reference.get().invoke(0, "muon.notifications.getCapabilities",
                    new JSONArray(), new byte[0][], disabled));
            assertTrue(disabled.await().contains("disabled"));
            Result settingsAvailable = new Result();
            scenario.onActivity(activity -> {
                MuonAndroidSettingsService settings = new MuonAndroidSettingsService(activity, reference.get()::hasChannel);
                settings.invoke("muon.android.settings.isAvailable", new JSONArray().put(
                        "{\"target\":\"notification-channel\",\"channelId\":\"general\"}"), new byte[0][], settingsAvailable);
                settings.close();
                if (Build.VERSION.SDK_INT >= 26) {
                    assertEquals(null, activity.getSystemService(NotificationManager.class).getNotificationChannel("disabled-unregistered"));
                }
            });
            assertEquals(Boolean.toString(Build.VERSION.SDK_INT >= 26), settingsAvailable.await());
            scenario.onActivity(activity -> reference.get().close());
        }
    }

    @Test
    public void reportsBlockedChannelsWithoutOverwritingTheirOsState() throws Exception {
        if (Build.VERSION.SDK_INT < 26) { return; }
        try (ActivityScenario<MuonWindowActivity> scenario = ActivityScenario.launch(MuonWindowActivity.class)) {
            String id = "blocked-" + System.nanoTime();
            AtomicReference<MuonAndroidNotificationService> reference = new AtomicReference<>();
            scenario.onActivity(activity -> {
                activity.getSystemService(NotificationManager.class).createNotificationChannel(
                        new NotificationChannel(id, "Blocked", NotificationManager.IMPORTANCE_NONE));
                reference.set(new MuonAndroidNotificationService(activity, MuonAppConfig.load(activity)));
            });
            Result ensured = new Result();
            scenario.onActivity(activity -> reference.get().invoke(0, "muon.android.notifications.ensureChannel",
                    new JSONArray().put("{\"id\":\"" + id + "\",\"name\":\"Replacement\",\"importance\":\"high\"}"),
                    new byte[0][], ensured));
            JSONObject state = new JSONObject(ensured.await());
            assertEquals("none", state.getString("importance"));
            assertEquals("Blocked", state.getString("name"));
            assertFalse(state.getBoolean("enabled"));
            Result status = new Result();
            scenario.onActivity(activity -> reference.get().invoke(0, "muon.notifications.getStatus",
                    new JSONArray().put("{\"category\":\"" + id + "\"}"), new byte[0][], status));
            assertEquals("disabled", new JSONObject(status.await()).getJSONObject("category").getString("state"));
            scenario.onActivity(activity -> reference.get().close());
        }
    }

    @Test
    public void rejectsOversizedConfiguredChannelNamesAndDescriptions() throws Exception {
        try (ActivityScenario<MuonWindowActivity> scenario = ActivityScenario.launch(MuonWindowActivity.class)) {
            scenario.onActivity(activity -> {
                for (String field : new String[] { "name", "description" }) {
                    try {
                        JSONObject config = new JSONObject(MuonAppConfig.readAsset(activity, "muon/config.json"));
                        config.getJSONObject("androidNotifications").getJSONArray("channels").getJSONObject(0)
                                .put(field, new String(new char[field.equals("name") ? 129 : 1025]).replace('\0', 'x'));
                        assertThrows(IllegalArgumentException.class,
                                () -> new MuonAndroidNotificationService(activity, new MuonAppConfig(config)));
                    } catch (org.json.JSONException error) { throw new AssertionError(error); }
                }
            });
        }
    }

    @Test
    public void diagnosesInvalidSmallIconConfigurationDuringStartup() throws Exception {
        try (ActivityScenario<MuonWindowActivity> scenario = ActivityScenario.launch(MuonWindowActivity.class)) {
            scenario.onActivity(activity -> {
                try {
                    JSONObject config = new JSONObject(MuonAppConfig.readAsset(activity, "muon/config.json"));
                    config.getJSONObject("androidNotifications").put("smallIcon", "missing_icon");
                    try {
                        new MuonAndroidNotificationService(activity, new MuonAppConfig(config));
                        throw new AssertionError("Invalid notification icons must reject startup");
                    } catch (IllegalArgumentException error) {
                        assertTrue(error.getMessage(), error.getMessage().contains("small icon"));
                    }
                } catch (org.json.JSONException error) { throw new AssertionError(error); }
            });
        }
    }
}

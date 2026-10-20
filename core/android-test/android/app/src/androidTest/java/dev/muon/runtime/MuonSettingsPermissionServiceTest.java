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

import android.accessibilityservice.AccessibilityService;
import android.os.Build;
import androidx.annotation.NonNull;
import androidx.test.core.app.ActivityScenario;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.Test;
import org.junit.runner.RunWith;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.Semaphore;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

/** Verifies settings and permission boundaries without requiring a recent WebView. */
@RunWith(AndroidJUnit4.class)
public final class MuonSettingsPermissionServiceTest {
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
            assertNotNull("Platform call must settle", value);
            return value;
        }
    }

    @Test
    public void refusesBackgroundSettingsLaunchesWhileAllowingAvailabilityChecks() throws Exception {
        try (ActivityScenario<MuonWindowActivity> scenario = ActivityScenario.launch(MuonWindowActivity.class)) {
            AtomicReference<MuonAndroidSettingsService> service = new AtomicReference<>();
            Result available = new Result();
            Result opened = new Result();
            scenario.onActivity(activity -> {
                service.set(new MuonAndroidSettingsService(activity, id -> false));
                service.get().invoke("muon.android.settings.isAvailable",
                        new JSONArray().put("{\"target\":\"app-details\"}"), new byte[0][], available);
                service.get().invoke("muon.android.settings.open",
                        new JSONArray().put("{\"target\":\"app-details\"}"), new byte[0][], opened);
                service.get().close();
            });
            assertEquals("true", available.await());
            String result = opened.await();
            assertTrue(result, result.contains("foreground Activity"));
        }
    }

    @Test
    public void usesOnlyAnExplicitFallbackForOlderNotificationSettings() throws Exception {
        try (ActivityScenario<MuonWindowActivity> scenario = ActivityScenario.launch(MuonWindowActivity.class)) {
            AtomicReference<MuonAndroidSettingsService> service = new AtomicReference<>();
            Result available = new Result();
            scenario.onActivity(activity -> {
                service.set(new MuonAndroidSettingsService(activity, id -> false));
                service.get().setResumed(true);
                service.get().invoke("muon.android.settings.isAvailable",
                        new JSONArray().put("{\"target\":\"notifications\"}"), new byte[0][], available);
            });
            assertEquals(Boolean.toString(Build.VERSION.SDK_INT >= 26), available.await());
            if (Build.VERSION.SDK_INT < 26) {
                Result unavailable = new Result();
                scenario.onActivity(activity -> service.get().invoke("muon.android.settings.open",
                        new JSONArray().put("{\"target\":\"notifications\"}"), new byte[0][], unavailable));
                assertTrue(unavailable.await().startsWith("error:"));
            }
            Result opened = new Result();
            var automation = InstrumentationRegistry.getInstrumentation().getUiAutomation();
            Semaphore changes = new Semaphore(0);
            // Accessibility callbacks run on the main thread; only signal from them.
            automation.setOnAccessibilityEventListener(event -> changes.release());
            try {
                scenario.onActivity(activity -> service.get().invoke("muon.android.settings.open",
                        new JSONArray().put("{\"target\":\"notifications\",\"fallback\":\"app-details\"}"),
                        new byte[0][], opened));
                long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(30);
                while (true) {
                    var root = automation.getRootInActiveWindow();
                    if (root != null && "com.android.settings".contentEquals(root.getPackageName())) { break; }
                    long remaining = deadline - System.nanoTime();
                    assertTrue("Settings must actually appear", remaining > 0
                            && changes.tryAcquire(remaining, TimeUnit.NANOSECONDS));
                }
            } finally {
                automation.setOnAccessibilityEventListener(null);
            }
            JSONObject result = new JSONObject(opened.await());
            assertEquals("notifications", result.getString("requestedTarget"));
            assertEquals(Build.VERSION.SDK_INT < 26 ? "app-details" : "notifications", result.getString("target"));
            assertEquals(Build.VERSION.SDK_INT < 26, result.getBoolean("fallbackUsed"));
            var instrumentation = InstrumentationRegistry.getInstrumentation();
            var returned = instrumentation.addMonitor(MuonWindowActivity.class.getName(), null, false);
            try {
                assertTrue(automation.performGlobalAction(AccessibilityService.GLOBAL_ACTION_BACK));
                assertNotNull("Back must return to the caller before the scenario closes",
                        instrumentation.waitForMonitorWithTimeout(returned, 30000));
            } finally { instrumentation.removeMonitor(returned); }
            scenario.onActivity(activity -> service.get().close());
            Result closed = new Result();
            scenario.onActivity(activity -> service.get().invoke("muon.android.settings.isAvailable",
                    new JSONArray().put("{\"target\":\"app-details\"}"), new byte[0][], closed));
            assertTrue(closed.await().startsWith("error:"));
        }
    }

    @Test
    public void opensKnownChannelSettingsAndRequiresExplicitFallbackOnOlderAndroid() throws Exception {
        try (ActivityScenario<MuonWindowActivity> scenario = ActivityScenario.launch(MuonWindowActivity.class)) {
            AtomicReference<MuonAndroidNotificationService> notifications = new AtomicReference<>();
            AtomicReference<MuonAndroidSettingsService> settings = new AtomicReference<>();
            Result available = new Result();
            Result unknown = new Result();
            scenario.onActivity(activity -> {
                notifications.set(new MuonAndroidNotificationService(activity, MuonAppConfig.load(activity)));
                settings.set(new MuonAndroidSettingsService(activity, notifications.get()::hasChannel));
                settings.get().setResumed(true);
                settings.get().invoke("muon.android.settings.isAvailable", new JSONArray().put(
                        "{\"target\":\"notification-channel\",\"channelId\":\"general\"}"), new byte[0][], available);
                settings.get().invoke("muon.android.settings.isAvailable", new JSONArray().put(
                        "{\"target\":\"notification-channel\",\"channelId\":\"missing-channel\"}"), new byte[0][], unknown);
            });
            assertEquals(Boolean.toString(Build.VERSION.SDK_INT >= 26), available.await());
            assertEquals("false", unknown.await());
            Result rejected = new Result();
            scenario.onActivity(activity -> settings.get().invoke("muon.android.settings.open", new JSONArray().put(
                    "{\"target\":\"notification-channel\",\"channelId\":\"missing-channel\"}"), new byte[0][], rejected));
            assertTrue(rejected.await().contains("unavailable"));
            var automation = InstrumentationRegistry.getInstrumentation().getUiAutomation();
            Semaphore changes = new Semaphore(0);
            automation.setOnAccessibilityEventListener(event -> changes.release());
            Result opened = new Result();
            try {
                scenario.onActivity(activity -> settings.get().invoke("muon.android.settings.open", new JSONArray().put(
                        "{\"target\":\"notification-channel\",\"channelId\":\"general\",\"fallback\":\"app-details\"}"), new byte[0][], opened));
                long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(30);
                while (true) {
                    var root = automation.getRootInActiveWindow();
                    if (root != null && "com.android.settings".contentEquals(root.getPackageName())) { break; }
                    long remaining = deadline - System.nanoTime();
                    assertTrue("Channel settings or the explicit fallback must appear", remaining > 0
                            && changes.tryAcquire(remaining, TimeUnit.NANOSECONDS));
                }
            } finally { automation.setOnAccessibilityEventListener(null); }
            JSONObject result = new JSONObject(opened.await());
            assertEquals("notification-channel", result.getJSONObject("requestedTarget").getString("type"));
            assertEquals("general", result.getJSONObject("requestedTarget").getString("channelId"));
            assertEquals(Build.VERSION.SDK_INT < 26, result.getBoolean("fallbackUsed"));
            if (Build.VERSION.SDK_INT >= 26) {
                assertEquals("general", result.getJSONObject("target").getString("channelId"));
            } else { assertEquals("app-details", result.getString("target")); }
            var instrumentation = InstrumentationRegistry.getInstrumentation();
            var returned = instrumentation.addMonitor(MuonWindowActivity.class.getName(), null, false);
            try {
                assertTrue(automation.performGlobalAction(AccessibilityService.GLOBAL_ACTION_BACK));
                assertNotNull("Back must return to the caller before the scenario closes",
                        instrumentation.waitForMonitorWithTimeout(returned, 30000));
            } finally { instrumentation.removeMonitor(returned); }
            scenario.onActivity(activity -> { settings.get().close(); notifications.get().close(); });
        }
    }

    @Test
    public void distinguishesUnsupportedRuntimePermissionAndUnavailableActivity() throws Exception {
        try (ActivityScenario<MuonWindowActivity> scenario = ActivityScenario.launch(MuonWindowActivity.class)) {
            AtomicReference<MuonAndroidNotificationPermissionService> service = new AtomicReference<>();
            Result background = new Result();
            scenario.onActivity(activity -> {
                service.set(new MuonAndroidNotificationPermissionService(activity, true));
                service.get().invoke(1, "muon.android.notifications.requestPermission", new JSONArray(), new byte[0][], background);
            });
            assertTrue(background.await().contains("foreground Activity"));
            if (Build.VERSION.SDK_INT < 33) {
                Result requested = new Result();
                scenario.onActivity(activity -> {
                    service.get().setResumed(true);
                    service.get().invoke(2, "muon.android.notifications.requestPermission", new JSONArray(), new byte[0][], requested);
                });
                JSONObject result = new JSONObject(requested.await());
                assertEquals("not-required", result.getString("result"));
                JSONObject state = result.getJSONObject("state");
                assertEquals("not-required", state.getString("runtimePermission"));
                assertFalse(state.getBoolean("shouldShowRationale"));
            }
            Result closed = new Result();
            scenario.onActivity(activity -> {
                service.get().close();
                service.get().invoke(3, "muon.android.notifications.getPermissionState", new JSONArray(), new byte[0][], closed);
            });
            assertTrue(closed.await().contains("Activity is unavailable"));
        }
    }
}

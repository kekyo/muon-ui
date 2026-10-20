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

import android.Manifest;
import android.app.NotificationManager;
import android.content.pm.PackageManager;
import android.content.Intent;
import android.os.Build;
import androidx.test.core.app.ActivityScenario;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.Test;
import org.junit.runner.RunWith;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

/** Exercises settings and notification permission state through the actual RPC boundary. */
@RunWith(AndroidJUnit4.class)
public final class MuonSettingsPermissionTest {
    @Test
    public void exposesOnlySupportedSettingsDestinations() throws Exception {
        try (ActivityScenario<MuonActivity> scenario = ActivityScenario.launch(MuonActivity.class)) {
            AtomicReference<MuonActivity> reference = new AtomicReference<>();
            scenario.onActivity(reference::set);
            MuonActivity activity = reference.get();
            assertTrue(activity.awaitPageReadyForTest(30, TimeUnit.SECONDS));
            JSONObject result = call(scenario, activity, "settings-capability",
                    "muon.android.settings.isAvailable", new JSONArray().put("{\"target\":\"app-details\"}"));
            assertTrue(result.toString(), result.getBoolean("ok"));
            assertTrue(result.getBoolean("value"));
            JSONObject notifications = call(scenario, activity, "settings-capability",
                    "muon.android.settings.isAvailable", new JSONArray().put("{\"target\":\"notifications\"}"));
            assertTrue(notifications.toString(), notifications.getBoolean("ok"));
            assertEquals(Build.VERSION.SDK_INT >= 26, notifications.getBoolean("value"));
        }
    }

    @Test
    public void reportsRuntimePermissionSeparatelyFromNotificationDelivery() throws Exception {
        try (ActivityScenario<MuonActivity> scenario = ActivityScenario.launch(MuonActivity.class)) {
            AtomicReference<MuonActivity> reference = new AtomicReference<>();
            scenario.onActivity(reference::set);
            MuonActivity activity = reference.get();
            assertTrue(activity.awaitPageReadyForTest(30, TimeUnit.SECONDS));
            JSONObject result = call(scenario, activity, "android-notifications-capability",
                    "muon.android.notifications.getPermissionState", new JSONArray());
            assertTrue(result.toString(), result.getBoolean("ok"));
            JSONObject state = new JSONObject(result.getString("value"));
            String permission = Build.VERSION.SDK_INT < 33 ? "not-required"
                    : activity.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS)
                            == PackageManager.PERMISSION_GRANTED ? "granted" : "denied";
            assertEquals(permission, state.getString("runtimePermission"));
            assertEquals(activity.getSystemService(NotificationManager.class).areNotificationsEnabled(),
                    state.getBoolean("notificationsEnabled"));
            assertTrue(state.has("requestedBefore"));
            assertTrue(state.has("shouldShowRationale"));
            assertTrue(state.has("settingsSuggested"));
        }
    }

    @Test
    public void keepsSettingsAvailableWhenNotificationsAreDisabled() throws Exception {
        Intent intent = new Intent(InstrumentationRegistry.getInstrumentation().getTargetContext(), MuonActivity.class)
                .putExtra("notifications-enabled", false);
        try (ActivityScenario<MuonActivity> scenario = ActivityScenario.launch(intent)) {
            AtomicReference<MuonActivity> reference = new AtomicReference<>();
            scenario.onActivity(reference::set);
            MuonActivity activity = reference.get();
            assertTrue(activity.awaitPageReadyForTest(30, TimeUnit.SECONDS));
            activity.clearTestMessages();
            scenario.onActivity(current -> current.getWebViewForTest().evaluateJavascript(
                    "muonAndroidTest.postMessage(JSON.stringify({"
                            + "publicApi:typeof muon.android.notifications,commonApi:typeof muon.notifications,"
                            + "registered:__muon_android_plugin_metadata.builtinFunctions.some("
                            + "path=>path.startsWith('muon.android.notifications.'))}));", null));
            String message = activity.awaitTestMessage(30, TimeUnit.SECONDS);
            assertNotNull(message);
            JSONObject exposed = new JSONObject(message);
            assertEquals("undefined", exposed.getString("publicApi"));
            assertEquals("undefined", exposed.getString("commonApi"));
            assertFalse(call(scenario, activity, "notifications-capability", "muon.notifications.getCapabilities",
                    new JSONArray()).getBoolean("ok"));
            assertFalse(exposed.getBoolean("registered"));
            for (String function : new String[] { "getPermissionState", "requestPermission" }) {
                assertFalse(call(scenario, activity, "android-notifications-capability",
                        "muon.android.notifications." + function, new JSONArray()).getBoolean("ok"));
            }
            JSONObject available = call(scenario, activity, "settings-capability", "muon.android.settings.isAvailable",
                    new JSONArray().put("{\"target\":\"app-details\"}"));
            assertTrue(available.toString(), available.getBoolean("ok"));
            assertTrue(available.getBoolean("value"));
            JSONObject config = new JSONObject(MuonAppConfig.readAsset(activity, "muon/config.json"));
            config.remove("notifications");
            assertFalse(new MuonAppConfig(config).notificationsEnabled);
        }
    }

    @Test
    public void rejectsMalformedSettingsAndOtherCapabilities() throws Exception {
        try (ActivityScenario<MuonActivity> scenario = ActivityScenario.launch(MuonActivity.class)) {
            AtomicReference<MuonActivity> reference = new AtomicReference<>();
            scenario.onActivity(reference::set);
            MuonActivity activity = reference.get();
            assertTrue(activity.awaitPageReadyForTest(30, TimeUnit.SECONDS));
            for (String options : new String[] { "[]", "{}", "{\"target\":\"wifi\"}",
                    "{\"target\":\"app-details\",\"fallback\":null}",
                    "{\"target\":\"notifications\",\"unknown\":true}" }) {
                assertFalse(options, call(scenario, activity, "settings-capability", "muon.android.settings.open",
                        new JSONArray().put(options)).getBoolean("ok"));
            }
            assertFalse(call(scenario, activity, "browser-capability", "muon.android.settings.isAvailable",
                    new JSONArray().put("{\"target\":\"app-details\"}")).getBoolean("ok"));
            assertFalse(call(scenario, activity, "settings-capability", "muon.android.notifications.getPermissionState",
                    new JSONArray()).getBoolean("ok"));
            assertFalse(call(scenario, activity, "android-notifications-capability",
                    "muon.android.notifications.requestPermission", new JSONArray().put(true)).getBoolean("ok"));
        }
    }

    private static JSONObject call(ActivityScenario<MuonActivity> scenario, MuonActivity activity,
            String capability, String path, JSONArray arguments) throws Exception {
        activity.clearTestMessages();
        scenario.onActivity(current -> current.getWebViewForTest().evaluateJavascript(
                "void (async () => { try { const value = await globalThis.__muon_plugin_call("
                        + JSONObject.quote(capability) + "," + JSONObject.quote(path) + ","
                        + arguments + "); muonAndroidTest.postMessage(JSON.stringify({ok:true,value}));"
                        + " } catch(error) { muonAndroidTest.postMessage(JSON.stringify({ok:false,"
                        + "error:error instanceof Error ? error.message : String(error)})); } })();", null));
        String message = activity.awaitTestMessage(30, TimeUnit.SECONDS);
        assertNotNull(path + " must settle", message);
        return new JSONObject(message);
    }
}

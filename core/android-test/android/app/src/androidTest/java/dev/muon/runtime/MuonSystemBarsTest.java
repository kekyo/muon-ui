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

import androidx.core.view.ViewCompat;
import androidx.core.view.WindowCompat;
import androidx.core.view.WindowInsetsCompat;
import androidx.test.core.app.ActivityScenario;
import androidx.test.ext.junit.runners.AndroidJUnit4;

import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.Test;
import org.junit.runner.RunWith;

import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

/** Exercises native system-bar contracts through the actual WebView RPC boundary. */
@RunWith(AndroidJUnit4.class)
public final class MuonSystemBarsTest {
    @Test
    public void sharesIndividualBarRequestsWithFullscreenAndObservesActualVisibility() throws Exception {
        try (ActivityScenario<MuonActivity> scenario = ActivityScenario.launch(MuonActivity.class)) {
            AtomicReference<MuonActivity> reference = new AtomicReference<>();
            scenario.onActivity(reference::set);
            MuonActivity activity = reference.get();
            assertTrue(activity.awaitPageReadyForTest(30, TimeUnit.SECONDS));

            JSONObject configured = call(scenario, activity, "system-bars-capability",
                    "muon.android.systemBars.configure",
                    new JSONArray().put("{\"statusBar\":{\"visible\":false}}"));
            assertTrue(configured.toString(), configured.getBoolean("ok"));
            awaitVisibleBars(scenario, false, true);
            assertState(scenario, activity, false, true, false, true);
            assertFalse(activity.isFullscreenForTest());

            assertTrue(call(scenario, activity, "system-bars-capability",
                    "muon.android.systemBars.configure",
                    new JSONArray().put("{\"navigationBar\":{\"visible\":false}}"))
                    .getBoolean("ok"));
            awaitVisibleBars(scenario, false, false);
            assertState(scenario, activity, false, false, false, false);
            assertTrue(activity.isFullscreenForTest());

            assertTrue(call(scenario, activity, "system-bars-capability",
                    "muon.android.systemBars.configure",
                    new JSONArray().put("{\"statusBar\":{\"visible\":true}}"))
                    .getBoolean("ok"));
            awaitVisibleBars(scenario, true, false);
            assertState(scenario, activity, true, false, true, false);
            assertFalse(activity.isFullscreenForTest());

            assertTrue(call(scenario, activity, "browser-capability",
                    "muon.browser.toggleFullscreen", new JSONArray()).getBoolean("ok"));
            awaitVisibleBars(scenario, false, false);
            assertState(scenario, activity, false, false, false, false);

            // A visibility change outside muon's request must not redefine fullscreen.
            scenario.onActivity(current -> WindowCompat.getInsetsController(
                    current.getWindow(), current.getWindow().getDecorView())
                    .show(WindowInsetsCompat.Type.systemBars()));
            awaitVisibleBars(scenario, true, true);
            assertState(scenario, activity, false, false, true, true);
            assertTrue(activity.isFullscreenForTest());

            assertTrue(call(scenario, activity, "browser-capability",
                    "muon.browser.toggleFullscreen", new JSONArray()).getBoolean("ok"));
            assertState(scenario, activity, true, true, true, true);
            assertFalse(activity.isFullscreenForTest());

            assertTrue(call(scenario, activity, "browser-capability",
                    "muon.browser.enterFullscreen", new JSONArray()).getBoolean("ok"));
            awaitVisibleBars(scenario, false, false);
            assertState(scenario, activity, false, false, false, false);
            assertTrue(call(scenario, activity, "browser-capability",
                    "muon.browser.exitFullscreen", new JSONArray()).getBoolean("ok"));
            awaitVisibleBars(scenario, true, true);
            assertState(scenario, activity, true, true, true, true);
        }
    }

    @Test
    public void rejectsMalformedPartialUpdatesWithoutChangingEitherRequest() throws Exception {
        try (ActivityScenario<MuonActivity> scenario = ActivityScenario.launch(MuonActivity.class)) {
            AtomicReference<MuonActivity> reference = new AtomicReference<>();
            scenario.onActivity(reference::set);
            MuonActivity activity = reference.get();
            assertTrue(activity.awaitPageReadyForTest(30, TimeUnit.SECONDS));
            assertTrue(call(scenario, activity, "system-bars-capability",
                    "muon.android.systemBars.configure",
                    new JSONArray().put("{\"statusBar\":{\"visible\":false}}"))
                    .getBoolean("ok"));
            awaitVisibleBars(scenario, false, true);

            String[] invalidOptions = {
                    "null", "[]", "true", "{", "{} trailing",
                    "{\"statusBar\":null}", "{\"statusBar\":[]}",
                    "{\"statusBar\":{\"visible\":null}}",
                    "{\"statusBar\":{\"visible\":1}}",
                    "{\"statusBar\":{\"visible\":\"false\"}}",
                    "{\"statusBars\":{\"visible\":true}}",
                    "{\"contentLayout\":\"safe-area\"}",
                    "{\"statusBar\":{\"icons\":\"light\"}}",
                    "{\"statusBar\":{\"visible\":true},\"navigationBar\":{\"visible\":\"false\"}}"
            };
            for (String options : invalidOptions) {
                JSONObject result = call(scenario, activity, "system-bars-capability",
                        "muon.android.systemBars.configure", new JSONArray().put(options));
                assertFalse(options + ": " + result, result.getBoolean("ok"));
                assertFalse(result.getString("error").isEmpty());
                assertState(scenario, activity, false, true, false, true);
            }

            assertFalse(call(scenario, activity, "system-bars-capability",
                    "muon.android.systemBars.configure", new JSONArray()).getBoolean("ok"));
            assertFalse(call(scenario, activity, "system-bars-capability",
                    "muon.android.systemBars.configure", new JSONArray().put(false)).getBoolean("ok"));
            assertFalse(call(scenario, activity, "system-bars-capability",
                    "muon.android.systemBars.configure", new JSONArray().put("{}").put("{}"))
                    .getBoolean("ok"));
            assertFalse(call(scenario, activity, "system-bars-capability",
                    "muon.android.systemBars.getState", new JSONArray().put("{}"))
                    .getBoolean("ok"));
            assertTrue(call(scenario, activity, "system-bars-capability",
                    "muon.android.systemBars.configure", new JSONArray().put("{\"statusBar\":{}}"))
                    .getBoolean("ok"));
            assertState(scenario, activity, false, true, false, true);
        }
    }

    @Test
    public void rejectsOtherCapabilitiesBeforeChangingSystemBars() throws Exception {
        try (ActivityScenario<MuonActivity> scenario = ActivityScenario.launch(MuonActivity.class)) {
            AtomicReference<MuonActivity> reference = new AtomicReference<>();
            scenario.onActivity(reference::set);
            MuonActivity activity = reference.get();
            assertTrue(activity.awaitPageReadyForTest(30, TimeUnit.SECONDS));
            assertTrue(call(scenario, activity, "browser-capability",
                    "muon.browser.exitFullscreen", new JSONArray()).getBoolean("ok"));
            awaitVisibleBars(scenario, true, true);

            assertFalse(call(scenario, activity, "browser-capability",
                    "muon.android.systemBars.configure",
                    new JSONArray().put("{\"statusBar\":{\"visible\":false}}"))
                    .getBoolean("ok"));
            assertFalse(call(scenario, activity, "system-bars-capability",
                    "muon.browser.enterFullscreen", new JSONArray()).getBoolean("ok"));
            assertState(scenario, activity, true, true, true, true);
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

    private static void assertState(ActivityScenario<MuonActivity> scenario, MuonActivity activity,
            boolean requestedStatus, boolean requestedNavigation,
            boolean actualStatus, boolean actualNavigation) throws Exception {
        JSONObject result = call(scenario, activity, "system-bars-capability",
                "muon.android.systemBars.getState", new JSONArray());
        assertTrue(result.toString(), result.getBoolean("ok"));
        JSONObject state = new JSONObject(result.getString("value"));
        assertEquals(requestedStatus, state.getJSONObject("requested")
                .getJSONObject("statusBar").getBoolean("visible"));
        assertEquals(requestedNavigation, state.getJSONObject("requested")
                .getJSONObject("navigationBar").getBoolean("visible"));
        assertEquals(actualStatus, state.getJSONObject("actual")
                .getJSONObject("statusBar").getBoolean("visible"));
        assertEquals(actualNavigation, state.getJSONObject("actual")
                .getJSONObject("navigationBar").getBoolean("visible"));
    }

    private static void awaitVisibleBars(ActivityScenario<MuonActivity> scenario,
            boolean status, boolean navigation) throws Exception {
        CountDownLatch observed = new CountDownLatch(1);
        scenario.onActivity(current -> {
            var decor = current.getWindow().getDecorView();
            ViewCompat.setOnApplyWindowInsetsListener(decor, (view, insets) -> {
                if (insets.isVisible(WindowInsetsCompat.Type.statusBars()) == status
                        && insets.isVisible(WindowInsetsCompat.Type.navigationBars()) == navigation) {
                    observed.countDown();
                }
                return insets;
            });
            WindowInsetsCompat insets = ViewCompat.getRootWindowInsets(decor);
            if (insets != null && insets.isVisible(WindowInsetsCompat.Type.statusBars()) == status
                    && insets.isVisible(WindowInsetsCompat.Type.navigationBars()) == navigation) {
                observed.countDown();
            }
            ViewCompat.requestApplyInsets(decor);
        });
        try {
            assertTrue("System bars must reach status=" + status + ", navigation=" + navigation,
                    observed.await(30, TimeUnit.SECONDS));
        } finally {
            scenario.onActivity(current -> ViewCompat.setOnApplyWindowInsetsListener(
                    current.getWindow().getDecorView(), null));
        }
    }
}

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

import android.os.Build;
import android.content.pm.ActivityInfo;
import android.content.res.Configuration;
import android.view.Choreographer;
import android.view.WindowInsets;
import android.view.WindowInsetsAnimation;

import androidx.core.view.ViewCompat;
import androidx.core.view.WindowCompat;
import androidx.core.view.WindowInsetsCompat;
import androidx.test.core.app.ActivityScenario;
import androidx.test.ext.junit.runners.AndroidJUnit4;

import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.Test;
import org.junit.runner.RunWith;

import java.util.concurrent.TimeUnit;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicReference;
import java.util.List;

/** Checks layout, subscription and Activity restoration through real platform RPC calls. */
@RunWith(AndroidJUnit4.class)
public final class MuonInsetsTest {
    @Test
    public void exposesSafeAreaWithoutApplyingTheSameMarginTwice() throws Exception {
        try (ActivityScenario<MuonActivity> scenario = ActivityScenario.launch(MuonActivity.class)) {
            AtomicReference<MuonActivity> reference = new AtomicReference<>();
            scenario.onActivity(reference::set);
            MuonActivity activity = reference.get();
            assertTrue(activity.awaitPageReadyForTest(30, TimeUnit.SECONDS));

            JSONObject initial = call(scenario, activity, "getInsets", new JSONArray());
            assertTrue(initial.toString(), initial.getBoolean("ok"));
            JSONObject snapshot = new JSONObject(initial.getString("value"));
            assertEquals("css-px", snapshot.getString("unit"));
            assertEquals("safe-area", snapshot.getString("contentLayout"));
            for (String edge : new String[] {"left", "top", "right", "bottom"}) {
                assertEquals(0, snapshot.getJSONObject("contentSafeArea").getDouble(edge), 0.001);
                assertEquals(Math.max(snapshot.getJSONObject("systemBars").getDouble(edge),
                                snapshot.getJSONObject("displayCutout").getDouble(edge)),
                        snapshot.getJSONObject("safeArea").getDouble(edge), 0.001);
            }
            assertWebContentClearsSystemBars(scenario, activity);

            assertTrue(call(scenario, activity, "configure",
                    new JSONArray().put("{\"contentLayout\":\"edge-to-edge\"}"))
                    .getBoolean("ok"));
            JSONObject edge = new JSONObject(call(scenario, activity, "getInsets", new JSONArray())
                    .getString("value"));
            assertEquals("edge-to-edge", edge.getString("contentLayout"));
            assertTrue(edge.getLong("revision") > snapshot.getLong("revision"));
            assertEquals(edge.getJSONObject("safeArea").toString(),
                    edge.getJSONObject("contentSafeArea").toString());
            assertWebContentClearsSystemBars(scenario, activity);
        }
    }

    @Test
    public void waitsForInsetsChangesAndReleasesPendingSubscriptions() throws Exception {
        try (ActivityScenario<MuonActivity> scenario = ActivityScenario.launch(MuonActivity.class)) {
            AtomicReference<MuonActivity> reference = new AtomicReference<>();
            scenario.onActivity(reference::set);
            MuonActivity activity = reference.get();
            assertTrue(activity.awaitPageReadyForTest(30, TimeUnit.SECONDS));
            JSONObject acquired = call(scenario, activity, "watchInsets",
                    new JSONArray().put("{\"operation\":\"acquire\"}"));
            assertTrue(acquired.toString(), acquired.getBoolean("ok"));
            JSONObject lease = new JSONObject(acquired.getString("value"));
            String token = lease.getString("token");
            long revision = lease.getJSONObject("insets").getLong("revision");

            JSONObject next = new JSONObject().put("operation", "next").put("token", token)
                    .put("afterRevision", revision);
            activity.clearTestMessages();
            scenario.onActivity(current -> current.getWebViewForTest().evaluateJavascript(
                    "void (async () => { try { const result = await globalThis.__muon_plugin_call("
                            + "'system-bars-capability','muon.android.systemBars.watchInsets',["
                            + JSONObject.quote(next.toString()) + "]);"
                            + "muonAndroidTest.postMessage('next:' + result);"
                            + "} catch(error) { muonAndroidTest.postMessage('failed:' + error); } })();"
                            + "void (async () => { await globalThis.__muon_plugin_call("
                            + "'system-bars-capability','muon.android.systemBars.configure',"
                            + "['{\"contentLayout\":\"edge-to-edge\"}']); })();", null));
            String event = activity.awaitTestMessage(30, TimeUnit.SECONDS);
            assertNotNull(event);
            assertTrue(event, event.startsWith("next:"));
            JSONObject changed = new JSONObject(event.substring(5)).getJSONObject("insets");
            assertTrue(changed.getLong("revision") > revision);
            assertEquals("edge-to-edge", changed.getString("contentLayout"));

            JSONObject latest = call(scenario, activity, "watchInsets", new JSONArray().put(next.toString()));
            assertTrue(latest.toString(), latest.getBoolean("ok"));
            JSONObject latestInsets = new JSONObject(latest.getString("value")).getJSONObject("insets");
            assertTrue(latestInsets.getLong("revision") > revision);
            next.put("afterRevision", latestInsets.getLong("revision"));
            JSONObject release = new JSONObject().put("operation", "release").put("token", token);
            activity.clearTestMessages();
            scenario.onActivity(current -> current.getWebViewForTest().evaluateJavascript(
                    "void (async () => { try { const pending = globalThis.__muon_plugin_call("
                            + "'system-bars-capability','muon.android.systemBars.watchInsets',["
                            + JSONObject.quote(next.toString()) + "]);"
                            + "await globalThis.__muon_plugin_call('system-bars-capability',"
                            + "'muon.android.systemBars.watchInsets',[" + JSONObject.quote(release.toString()) + "]);"
                            + "muonAndroidTest.postMessage(await pending);"
                            + "} catch(error) { muonAndroidTest.postMessage('failed:' + error); } })();", null));
            String ended = activity.awaitTestMessage(30, TimeUnit.SECONDS);
            assertNotNull(ended);
            assertTrue(ended, new JSONObject(ended).isNull("insets"));
            assertEquals(0, activity.getNativePendingCallCountForTest());
            assertTrue(call(scenario, activity, "watchInsets", new JSONArray().put(release.toString()))
                    .getBoolean("ok"));
            assertFalse(call(scenario, activity, "watchInsets", new JSONArray().put(next.toString()))
                    .getBoolean("ok"));
        }
    }

    @Test
    public void convertsInsetsUsingTheObservedWebViewZoom() throws Exception {
        try (ActivityScenario<MuonActivity> scenario = ActivityScenario.launch(MuonActivity.class)) {
            AtomicReference<MuonActivity> reference = new AtomicReference<>();
            scenario.onActivity(reference::set);
            MuonActivity activity = reference.get();
            assertTrue(activity.awaitPageReadyForTest(30, TimeUnit.SECONDS));
            assertTrue(call(scenario, activity, "configure", new JSONArray()
                    .put("{\"contentLayout\":\"edge-to-edge\"}")).getBoolean("ok"));
            activity.clearTestMessages();
            scenario.onActivity(current -> current.getWebViewForTest().evaluateJavascript("""
                    void (async () => {
                      try {
                        const before = await muon.android.systemBars.getInsets();
                        let complete;
                        const changed = new Promise(resolve => { complete = resolve; });
                        const watch = await muon.android.systemBars.watchInsets(insets => {
                          if (insets.safeArea.top < before.safeArea.top) complete(insets);
                        });
                        await muon.browser.zoomIn();
                        const snapshot = await changed;
                        await watch.close();
                        await new Promise(resolve => requestAnimationFrame(resolve));
                        muonAndroidTest.postMessage(JSON.stringify({snapshot,
                          viewportWidth:visualViewport.width,
                          cssTop:parseFloat(getComputedStyle(document.documentElement)
                            .getPropertyValue('--muon-safe-area-top'))}));
                      } catch(error) { muonAndroidTest.postMessage(JSON.stringify({error:String(error)})); }
                    })();
                    """, null));
            String message = activity.awaitTestMessage(30, TimeUnit.SECONDS);
            assertNotNull("Zoom must notify the inset subscription", message);
            JSONObject result = new JSONObject(message);
            assertFalse(result.toString(), result.has("error"));
            AtomicReference<double[]> measured = new AtomicReference<>();
            scenario.onActivity(current -> measured.set(new double[] {
                    current.getWebViewForTest().getWidth(),
                    ViewCompat.getRootWindowInsets(current.getWindow().getDecorView())
                            .getInsets(WindowInsetsCompat.Type.systemBars() | WindowInsetsCompat.Type.displayCutout()).top }));
            double scale = measured.get()[0] / result.getDouble("viewportWidth");
            double inset = result.getJSONObject("snapshot").getJSONObject("safeArea").getDouble("top");
            assertEquals(measured.get()[1], inset * scale, 2.0);
            assertEquals(inset, result.getDouble("cssTop"), 0.01);
        }
    }

    @Test
    @androidx.test.filters.SdkSuppress(minSdkVersion = 30)
    public void reportsImeSeparatelyFromTheSafeArea() throws Exception {
        try (ActivityScenario<MuonActivity> scenario = ActivityScenario.launch(MuonActivity.class)) {
            AtomicReference<MuonActivity> reference = new AtomicReference<>();
            scenario.onActivity(reference::set);
            MuonActivity activity = reference.get();
            assertTrue(activity.awaitPageReadyForTest(30, TimeUnit.SECONDS));
            activity.clearTestMessages();
            scenario.onActivity(current -> current.getWebViewForTest().evaluateJavascript("""
                    void (async () => {
                      await muon.android.systemBars.configure({contentLayout:'edge-to-edge'});
                      const marker = document.createElement('div');
                      marker.id = 'ime-state-marker';
                      marker.style.cssText = 'position:fixed;left:calc(50% - 40px);top:120px;width:80px;height:80px;z-index:999999;background:#00ff00';
                      document.body.append(marker);
                      globalThis.imeSnapshots = [];
                      globalThis.imeWatch = await muon.android.systemBars.watchInsets(insets => {
                        imeSnapshots.push(insets);
                        marker.style.backgroundColor = insets.ime.bottom > insets.safeArea.bottom ? '#ff00ff' : '#00ff00';
                      });
                      const input = document.createElement('input');
                      input.id = 'insets-ime-test';
                      input.style.cssText = 'position:fixed;left:20px;top:60px;width:180px;z-index:999998';
                      document.body.append(input);
                      muonAndroidTest.postMessage('watching');
                    })();
                    """, null));
            assertEquals("watching", activity.awaitTestMessage(30, TimeUnit.SECONDS));
            long previousRevision = 0;
            for (boolean visible : new boolean[] {true, false}) {
                CountDownLatch stable = new CountDownLatch(1);
                AtomicReference<Choreographer.FrameCallback> callback = new AtomicReference<>();
                scenario.onActivity(current -> {
                    var decor = current.getWindow().getDecorView();
                    boolean[] ended = {false};
                    int[] stableFrames = {0};
                    decor.setWindowInsetsAnimationCallback(new WindowInsetsAnimation.Callback(
                            WindowInsetsAnimation.Callback.DISPATCH_MODE_CONTINUE_ON_SUBTREE) {
                        @Override public WindowInsets onProgress(WindowInsets insets,
                                List<WindowInsetsAnimation> animations) { return insets; }
                        @Override public void onEnd(WindowInsetsAnimation animation) {
                            if ((animation.getTypeMask() & WindowInsets.Type.ime()) != 0) { ended[0] = true; }
                        }
                    });
                    Choreographer.FrameCallback frames = new Choreographer.FrameCallback() {
                        @Override public void doFrame(long frameTimeNanos) {
                            WindowInsets insets = decor.getRootWindowInsets();
                            boolean matches = ended[0] && insets != null
                                    && insets.isVisible(WindowInsets.Type.ime()) == visible;
                            stableFrames[0] = matches ? stableFrames[0] + 1 : 0;
                            if (stableFrames[0] == 60) {
                                android.util.Log.i("MuonImeTest", "imeVisible=" + visible
                                        + ", stableFrames=60, frameTimeNanos=" + frameTimeNanos);
                                stable.countDown();
                            } else { Choreographer.getInstance().postFrameCallback(this); }
                        }
                    };
                    callback.set(frames);
                    Choreographer.getInstance().postFrameCallback(frames);
                });
                try {
                    if (visible) {
                        activity.clearTestMessages();
                        scenario.onActivity(current -> current.getWebViewForTest().evaluateJavascript(
                                "document.querySelector('#insets-ime-test').focus();muonAndroidTest.postMessage('focused');", null));
                        assertEquals("focused", activity.awaitTestMessage(30, TimeUnit.SECONDS));
                    }
                    scenario.onActivity(current -> {
                        var controller = WindowCompat.getInsetsController(current.getWindow(), current.getWebViewForTest());
                        if (visible) { controller.show(WindowInsetsCompat.Type.ime()); }
                        else { controller.hide(WindowInsetsCompat.Type.ime()); }
                    });
                    assertTrue("IME visibility must finish animating and remain stable: " + visible,
                            stable.await(60, TimeUnit.SECONDS));
                } finally {
                    scenario.onActivity(current -> {
                        Choreographer.getInstance().removeFrameCallback(callback.get());
                        current.getWindow().getDecorView().setWindowInsetsAnimationCallback(null);
                    });
                }
                activity.clearTestMessages();
                scenario.onActivity(current -> current.getWebViewForTest().evaluateJavascript("""
                        void (async () => {
                          const snapshot = await muon.android.systemBars.getInsets();
                          const css = getComputedStyle(document.documentElement);
                          muonAndroidTest.postMessage(JSON.stringify({snapshot,
                            lastRevision:imeSnapshots.at(-1).revision,
                            cssBottom:parseFloat(css.getPropertyValue('--muon-safe-area-bottom')),
                            marker:getComputedStyle(document.querySelector('#ime-state-marker')).backgroundColor}));
                        })();
                        """, null));
                String message = activity.awaitTestMessage(30, TimeUnit.SECONDS);
                assertNotNull(message);
                JSONObject result = new JSONObject(message);
                JSONObject snapshot = result.getJSONObject("snapshot");
                assertTrue(snapshot.getLong("revision") > previousRevision);
                assertEquals(snapshot.getLong("revision"), result.getLong("lastRevision"));
                assertEquals(visible, snapshot.getJSONObject("ime").getDouble("bottom") > 0);
                assertEquals(visible ? "rgb(255, 0, 255)" : "rgb(0, 255, 0)", result.getString("marker"));
                for (String edge : new String[] {"left", "top", "right", "bottom"}) {
                    assertEquals(Math.max(snapshot.getJSONObject("systemBars").getDouble(edge),
                                    snapshot.getJSONObject("displayCutout").getDouble(edge)),
                            snapshot.getJSONObject("safeArea").getDouble(edge), 0.001);
                }
                assertEquals(snapshot.getJSONObject("safeArea").getDouble("bottom"), result.getDouble("cssBottom"), 0.01);
                if (visible) {
                    assertTrue(snapshot.getJSONObject("ime").getDouble("bottom") > result.getDouble("cssBottom"));
                }
                previousRevision = snapshot.getLong("revision");
            }
            scenario.onActivity(current -> {
                current.getWebViewForTest().evaluateJavascript("void imeWatch.close();", null);
                WindowCompat.getInsetsController(current.getWindow(), current.getWebViewForTest())
                        .hide(WindowInsetsCompat.Type.ime());
            });
        }
    }

    @Test
    public void preservesTheLayoutAndUpdatesSafeEdgesAfterRotation() throws Exception {
        try (ActivityScenario<MuonActivity> scenario = ActivityScenario.launch(MuonActivity.class)) {
            AtomicReference<MuonActivity> reference = new AtomicReference<>();
            scenario.onActivity(reference::set);
            MuonActivity activity = reference.get();
            assertTrue(activity.awaitPageReadyForTest(30, TimeUnit.SECONDS));
            assertTrue(call(scenario, activity, "configure", new JSONArray()
                    .put("{\"contentLayout\":\"edge-to-edge\"}")).getBoolean("ok"));
            int original = activity.getResources().getConfiguration().orientation;
            int target = original == Configuration.ORIENTATION_PORTRAIT
                    ? ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE : ActivityInfo.SCREEN_ORIENTATION_PORTRAIT;
            scenario.onActivity(current -> current.setRequestedOrientation(target));
            assertTrue(activity.awaitDestroyedForTest(30, TimeUnit.SECONDS));
            scenario.onActivity(reference::set);
            MuonActivity rotated = reference.get();
            assertTrue(rotated.awaitPageReadyForTest(30, TimeUnit.SECONDS));
            assertTrue(original != rotated.getResources().getConfiguration().orientation);
            JSONObject snapshot = new JSONObject(call(scenario, rotated, "getInsets", new JSONArray()).getString("value"));
            assertEquals("edge-to-edge", snapshot.getString("contentLayout"));
            assertWebContentClearsSystemBars(scenario, rotated);
        }
    }

    @Test
    public void releasesDocumentSubscriptionsWhenReloaded() throws Exception {
        try (ActivityScenario<MuonActivity> scenario = ActivityScenario.launch(MuonActivity.class)) {
            AtomicReference<MuonActivity> reference = new AtomicReference<>();
            scenario.onActivity(reference::set);
            MuonActivity activity = reference.get();
            assertTrue(activity.awaitPageReadyForTest(30, TimeUnit.SECONDS));
            JSONObject acquired = call(scenario, activity, "watchInsets",
                    new JSONArray().put("{\"operation\":\"acquire\"}"));
            assertTrue(acquired.toString(), acquired.getBoolean("ok"));
            scenario.onActivity(current -> assertEquals(1,
                    current.getRpcBridgeForTest().getSystemBarsService().getActiveWatchCount()));
            activity.clearFinishedPageUrlsForTest();
            scenario.onActivity(current -> current.getWebViewForTest().reload());
            assertEquals(MuonActivity.TRUSTED_ORIGIN + "/index.html",
                    activity.awaitFinishedPageUrlForTest(30, TimeUnit.SECONDS));
            scenario.onActivity(current -> assertEquals(0,
                    current.getRpcBridgeForTest().getSystemBarsService().getActiveWatchCount()));
            assertTrue(call(scenario, activity, "getInsets", new JSONArray()).getBoolean("ok"));
        }
    }

    @Test
    public void restoresRequestedAppearanceOnlyAcrossActivityRecreation() throws Exception {
        try (ActivityScenario<MuonActivity> scenario = ActivityScenario.launch(MuonActivity.class)) {
            AtomicReference<MuonActivity> reference = new AtomicReference<>();
            scenario.onActivity(reference::set);
            MuonActivity activity = reference.get();
            assertTrue(activity.awaitPageReadyForTest(30, TimeUnit.SECONDS));
            String navigationIcons = Build.VERSION.SDK_INT >= 26 ? "dark" : "light";
            JSONObject options = new JSONObject().put("contentLayout", "edge-to-edge")
                    .put("statusBar", new JSONObject().put("visible", false).put("icons", "dark"))
                    .put("navigationBar", new JSONObject().put("visible", true).put("icons", navigationIcons));
            JSONObject configured = call(scenario, activity, "configure", new JSONArray().put(options.toString()));
            assertTrue(configured.toString(), configured.getBoolean("ok"));
            scenario.recreate();
            scenario.onActivity(reference::set);
            MuonActivity recreated = reference.get();
            assertTrue(recreated.awaitPageReadyForTest(30, TimeUnit.SECONDS));
            JSONObject state = new JSONObject(call(scenario, recreated, "getState", new JSONArray()).getString("value"));
            JSONObject requested = state.getJSONObject("requested");
            assertEquals("edge-to-edge", requested.getString("contentLayout"));
            assertFalse(requested.getJSONObject("statusBar").getBoolean("visible"));
            assertEquals("dark", requested.getJSONObject("statusBar").getString("icons"));
            assertTrue(requested.getJSONObject("navigationBar").getBoolean("visible"));
            assertEquals(navigationIcons, requested.getJSONObject("navigationBar").getString("icons"));
            assertEquals("dark", state.getJSONObject("actual").getJSONObject("statusBar").getString("icons"));
        }
        try (ActivityScenario<MuonActivity> fresh = ActivityScenario.launch(MuonActivity.class)) {
            AtomicReference<MuonActivity> reference = new AtomicReference<>();
            fresh.onActivity(reference::set);
            MuonActivity activity = reference.get();
            assertTrue(activity.awaitPageReadyForTest(30, TimeUnit.SECONDS));
            JSONObject requested = new JSONObject(call(fresh, activity, "getState", new JSONArray())
                    .getString("value")).getJSONObject("requested");
            assertEquals("safe-area", requested.getString("contentLayout"));
            assertTrue(requested.getJSONObject("statusBar").getBoolean("visible"));
            assertTrue(requested.getJSONObject("statusBar").isNull("icons"));
        }
    }

    private static JSONObject call(ActivityScenario<MuonActivity> scenario, MuonActivity activity,
            String name, JSONArray arguments) throws Exception {
        activity.clearTestMessages();
        scenario.onActivity(current -> current.getWebViewForTest().evaluateJavascript(
                "void (async () => { try { const value = await globalThis.__muon_plugin_call("
                        + "'system-bars-capability'," + JSONObject.quote("muon.android.systemBars." + name)
                        + "," + arguments + "); muonAndroidTest.postMessage(JSON.stringify({ok:true,value}));"
                        + "} catch(error) { muonAndroidTest.postMessage(JSON.stringify({ok:false,error:String(error)})); } })();", null));
        String message = activity.awaitTestMessage(30, TimeUnit.SECONDS);
        assertNotNull(name + " must settle", message);
        return new JSONObject(message);
    }

    private static void assertWebContentClearsSystemBars(ActivityScenario<MuonActivity> scenario,
            MuonActivity activity) throws Exception {
        activity.clearTestMessages();
        scenario.onActivity(current -> current.getWebViewForTest().evaluateJavascript("""
                void (async () => {
                  let marker = document.querySelector('#muon-insets-marker');
                  if (!marker) {
                    marker = document.createElement('div');
                    marker.id = 'muon-insets-marker';
                    marker.style.cssText = 'position:fixed;left:var(--muon-safe-area-left);top:var(--muon-safe-area-top);right:var(--muon-safe-area-right);bottom:var(--muon-safe-area-bottom);background:#008080';
                    document.body.append(marker);
                  }
                  await new Promise(resolve => requestAnimationFrame(resolve));
                  const rect = marker.getBoundingClientRect();
                  muonAndroidTest.postMessage(JSON.stringify({ top:rect.top, left:rect.left,
                    right:rect.right, bottom:rect.bottom, width:visualViewport.width,
                    height:visualViewport.height, offsetLeft:visualViewport.offsetLeft,
                    offsetTop:visualViewport.offsetTop }));
                })();
                """, null));
        String message = activity.awaitTestMessage(30, TimeUnit.SECONDS);
        assertNotNull(message);
        JSONObject dom = new JSONObject(message);
        AtomicReference<double[]> geometry = new AtomicReference<>();
        scenario.onActivity(current -> {
            var webView = current.getWebViewForTest();
            var decor = current.getWindow().getDecorView();
            int[] webPosition = new int[2];
            int[] rootPosition = new int[2];
            webView.getLocationInWindow(webPosition);
            decor.getLocationInWindow(rootPosition);
            WindowInsetsCompat insets = ViewCompat.getRootWindowInsets(decor);
            assertNotNull(insets);
            var safe = insets.getInsets(WindowInsetsCompat.Type.systemBars()
                    | WindowInsetsCompat.Type.displayCutout());
            geometry.set(new double[] { webView.getWidth(), webPosition[0] - rootPosition[0],
                    webPosition[1] - rootPosition[1], decor.getWidth(), decor.getHeight(),
                    safe.left, safe.top, safe.right, safe.bottom });
        });
        double[] nativeGeometry = geometry.get();
        double scale = nativeGeometry[0] / dom.getDouble("width");
        assertEquals(nativeGeometry[5], nativeGeometry[1]
                + (dom.getDouble("left") - dom.getDouble("offsetLeft")) * scale, 2.0);
        assertEquals(nativeGeometry[6], nativeGeometry[2]
                + (dom.getDouble("top") - dom.getDouble("offsetTop")) * scale, 2.0);
        assertEquals(nativeGeometry[3] - nativeGeometry[7], nativeGeometry[1]
                + (dom.getDouble("right") - dom.getDouble("offsetLeft")) * scale, 2.0);
        assertEquals(nativeGeometry[4] - nativeGeometry[8], nativeGeometry[2]
                + (dom.getDouble("bottom") - dom.getDouble("offsetTop")) * scale, 2.0);
    }
}

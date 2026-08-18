/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

package dev.muon.prototype;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNotSame;
import static org.junit.Assert.assertTrue;

import android.webkit.WebView;

import androidx.test.core.app.ActivityScenario;
import androidx.test.ext.junit.runners.AndroidJUnit4;

import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.Test;
import org.junit.runner.RunWith;

import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

@RunWith(AndroidJUnit4.class)
public final class MuonActivityTest {
    @Test
    public void servesViteAssetsFromTrustedHttpsOrigin() throws Exception {
        try (ActivityScenario<MuonActivity> scenario = ActivityScenario.launch(MuonActivity.class)) {
            AtomicReference<MuonActivity> activityReference = new AtomicReference<>();
            scenario.onActivity(activityReference::set);
            MuonActivity activity = activityReference.get();
            assertNotNull(activity);
            assertTrue(activity.awaitPageReadyForTest(30, TimeUnit.SECONDS));

            AtomicReference<String> result = new AtomicReference<>();
            CountDownLatch evaluated = new CountDownLatch(1);
            scenario.onActivity(current -> {
                WebView webView = current.getWebViewForTest();
                webView.evaluateJavascript(
                        "document.querySelector('h1')?.textContent + '|' + location.origin",
                        value -> {
                            result.set(value);
                            evaluated.countDown();
                        });
            });
            assertTrue(evaluated.await(30, TimeUnit.SECONDS));
            assertEquals(
                    "\"muon Android prototype|https://main.asset.muon.invalid\"",
                    result.get());
        }
    }

    @Test
    public void simpleModePublishesOnlyImplementedAndroidNamespaces() throws Exception {
        try (ActivityScenario<MuonActivity> scenario = ActivityScenario.launch(MuonActivity.class)) {
            AtomicReference<MuonActivity> activityReference = new AtomicReference<>();
            scenario.onActivity(activityReference::set);
            MuonActivity activity = activityReference.get();
            assertNotNull(activity);
            assertTrue(activity.awaitPageReadyForTest(30, TimeUnit.SECONDS));

            AtomicReference<String> result = new AtomicReference<>();
            CountDownLatch evaluated = new CountDownLatch(1);
            scenario.onActivity(current -> current.getWebViewForTest().evaluateJavascript(
                    "JSON.stringify({" +
                            "namespaces: Object.keys(globalThis.muon)," +
                            "browser: Object.keys(globalThis.muon.browser)," +
                            "hasHardReload: 'hardReload' in globalThis.muon.browser" +
                            "})",
                    value -> {
                        result.set(value);
                        evaluated.countDown();
                    }));

            assertTrue(evaluated.await(30, TimeUnit.SECONDS));
            assertNotNull(result.get());
            String decoded = new JSONArray("[" + result.get() + "]").getString(0);
            JSONObject api = new JSONObject(decoded);
            assertEquals("[\"browser\",\"environments\"]",
                    api.getJSONArray("namespaces").toString());
            assertEquals(8, api.getJSONArray("browser").length());
            assertFalse(api.getBoolean("hasHardReload"));
        }
    }

    @Test
    public void roundTripsGetConfigValuesAsAPromise() throws Exception {
        try (ActivityScenario<MuonActivity> scenario = ActivityScenario.launch(MuonActivity.class)) {
            AtomicReference<MuonActivity> activityReference = new AtomicReference<>();
            scenario.onActivity(activityReference::set);
            MuonActivity activity = activityReference.get();
            assertNotNull(activity);
            assertTrue(activity.awaitPageReadyForTest(30, TimeUnit.SECONDS));
            activity.clearTestMessages();

            scenario.onActivity(current -> current.getWebViewForTest().evaluateJavascript("""
                    void (async () => {
                      try {
                        const value = await globalThis.__muon_plugin_call(
                          'environment-capability',
                          'muon.environments.getConfigValues',
                          []
                        );
                        muonAndroidTest.postMessage(JSON.stringify({ status: 'resolved', value }));
                      } catch (error) {
                        muonAndroidTest.postMessage(JSON.stringify({
                          status: 'rejected',
                          error: error instanceof Error ? error.message : String(error)
                        }));
                      }
                    })();
                    """, null));

            String message = activity.awaitTestMessage(30, TimeUnit.SECONDS);
            assertNotNull(message);
            JSONObject result = new JSONObject(message);
            assertEquals("resolved", result.getString("status"));
            JSONObject config = new JSONObject(result.getString("value"));
            assertEquals("android", config.getString("channel"));
            assertEquals("webview", config.getString("backend"));
        }
    }

    @Test
    public void returnsAndroidEnvironmentInformation() throws Exception {
        try (ActivityScenario<MuonActivity> scenario = ActivityScenario.launch(MuonActivity.class)) {
            AtomicReference<MuonActivity> activityReference = new AtomicReference<>();
            scenario.onActivity(activityReference::set);
            MuonActivity activity = activityReference.get();
            assertNotNull(activity);
            assertTrue(activity.awaitPageReadyForTest(30, TimeUnit.SECONDS));
            activity.clearTestMessages();

            scenario.onActivity(current -> current.getWebViewForTest().evaluateJavascript("""
                    void (async () => {
                      try {
                        const call = (path) => globalThis.__muon_plugin_call(
                          'environment-capability', path, []
                        );
                        const variables = JSON.parse(await call(
                          'muon.environments.getVariables'
                        ));
                        const config = JSON.parse(await call(
                          'muon.environments.getConfigValues'
                        ));
                        const processId = await call(
                          'muon.environments.getProcessId'
                        );
                        const runtime = JSON.parse(await call(
                          'muon.environments.getRuntimeInfo'
                        ));
                        muonAndroidTest.postMessage(JSON.stringify({
                          status: 'resolved',
                          variables,
                          config,
                          processId,
                          runtime
                        }));
                      } catch (error) {
                        muonAndroidTest.postMessage(JSON.stringify({
                          status: 'rejected',
                          error: error instanceof Error ? error.message : String(error)
                        }));
                      }
                    })();
                    """, null));

            String message = activity.awaitTestMessage(30, TimeUnit.SECONDS);
            assertNotNull(message);
            JSONObject result = new JSONObject(message);
            assertEquals(message, "resolved", result.getString("status"));
            assertTrue(result.getJSONObject("variables").length() >= 0);
            assertEquals("android", result.getJSONObject("config").getString("channel"));
            assertTrue(result.getInt("processId") > 0);
            JSONObject runtime = result.getJSONObject("runtime");
            assertEquals("android-webview", runtime.getString("backend"));
            assertEquals("android", runtime.getString("os"));
            assertTrue(runtime.getInt("apiLevel") >= 24);
            assertFalse(runtime.getString("abi").isEmpty());
            assertFalse(runtime.getString("webViewPackage").isEmpty());
            assertFalse(runtime.getString("webViewVersion").isEmpty());
        }
    }

    @Test
    public void controlsFullscreenAndManagedZoom() throws Exception {
        try (ActivityScenario<MuonActivity> scenario = ActivityScenario.launch(MuonActivity.class)) {
            AtomicReference<MuonActivity> activityReference = new AtomicReference<>();
            scenario.onActivity(activityReference::set);
            MuonActivity activity = activityReference.get();
            assertNotNull(activity);
            assertTrue(activity.awaitPageReadyForTest(30, TimeUnit.SECONDS));
            activity.clearTestMessages();

            scenario.onActivity(current -> current.getWebViewForTest().evaluateJavascript("""
                    void (async () => {
                      try {
                        const call = (path) => globalThis.__muon_plugin_call(
                          'browser-capability', path, []
                        );
                        await call('muon.browser.enterFullscreen');
                        await call('muon.browser.zoomIn');
                        muonAndroidTest.postMessage('entered');
                      } catch (error) {
                        muonAndroidTest.postMessage(
                          error instanceof Error ? error.message : String(error)
                        );
                      }
                    })();
                    """, null));

            assertEquals("entered", activity.awaitTestMessage(30, TimeUnit.SECONDS));
            assertTrue(activity.isFullscreenForTest());
            assertTrue(activity.getManagedZoomFactorForTest() > 1.0f);

            activity.clearTestMessages();
            scenario.onActivity(current -> current.getWebViewForTest().evaluateJavascript("""
                    void (async () => {
                      const call = (path) => globalThis.__muon_plugin_call(
                        'browser-capability', path, []
                      );
                      await call('muon.browser.toggleFullscreen');
                      await call('muon.browser.zoomOut');
                      await call('muon.browser.zoomIn');
                      await call('muon.browser.resetZoom');
                      muonAndroidTest.postMessage('reset');
                    })();
                    """, null));

            assertEquals("reset", activity.awaitTestMessage(30, TimeUnit.SECONDS));
            assertFalse(activity.isFullscreenForTest());
            assertEquals(1.0f, activity.getManagedZoomFactorForTest(), 0.001f);
        }
    }

    @Test
    public void reloadsTheCurrentWebView() throws Exception {
        try (ActivityScenario<MuonActivity> scenario = ActivityScenario.launch(MuonActivity.class)) {
            AtomicReference<MuonActivity> activityReference = new AtomicReference<>();
            scenario.onActivity(activityReference::set);
            MuonActivity activity = activityReference.get();
            assertNotNull(activity);
            assertTrue(activity.awaitPageReadyForTest(30, TimeUnit.SECONDS));
            activity.clearFinishedPageUrlsForTest();

            scenario.onActivity(current -> current.getWebViewForTest().evaluateJavascript("""
                    void globalThis.__muon_plugin_call(
                      'browser-capability',
                      'muon.browser.reload',
                      []
                    );
                    """, null));

            assertEquals(
                    MuonActivity.TRUSTED_ORIGIN + "/index.html",
                    activity.awaitFinishedPageUrlForTest(30, TimeUnit.SECONDS));
        }
    }

    @Test
    public void closesOnlyTheOwnedActivity() throws Exception {
        try (ActivityScenario<MuonActivity> scenario = ActivityScenario.launch(MuonActivity.class)) {
            AtomicReference<MuonActivity> activityReference = new AtomicReference<>();
            scenario.onActivity(activityReference::set);
            MuonActivity activity = activityReference.get();
            assertNotNull(activity);
            assertTrue(activity.awaitPageReadyForTest(30, TimeUnit.SECONDS));

            scenario.onActivity(current -> current.getWebViewForTest().evaluateJavascript("""
                    void globalThis.__muon_plugin_call(
                      'browser-capability',
                      'muon.browser.close',
                      []
                    );
                    """, null));

            assertTrue(activity.awaitDestroyedForTest(30, TimeUnit.SECONDS));
        }
    }

    @Test
    public void rejectsNativeFailuresWithTheirDiagnostic() throws Exception {
        try (ActivityScenario<MuonActivity> scenario = ActivityScenario.launch(MuonActivity.class)) {
            AtomicReference<MuonActivity> activityReference = new AtomicReference<>();
            scenario.onActivity(activityReference::set);
            MuonActivity activity = activityReference.get();
            assertNotNull(activity);
            assertTrue(activity.awaitPageReadyForTest(30, TimeUnit.SECONDS));
            activity.clearTestMessages();

            scenario.onActivity(current -> current.getWebViewForTest().evaluateJavascript("""
                    void (async () => {
                      try {
                        await globalThis.__muon_plugin_call(
                          'prototype-capability',
                          'prototype.fail',
                          []
                        );
                        muonAndroidTest.postMessage(JSON.stringify({ status: 'resolved' }));
                      } catch (error) {
                        muonAndroidTest.postMessage(JSON.stringify({
                          status: 'rejected',
                          error: error instanceof Error ? error.message : String(error)
                        }));
                      }
                    })();
                    """, null));

            String message = activity.awaitTestMessage(30, TimeUnit.SECONDS);
            assertNotNull(message);
            JSONObject result = new JSONObject(message);
            assertEquals("rejected", result.getString("status"));
            assertEquals("prototype failure", result.getString("error"));
        }
    }

    @Test
    public void rejectsFunctionsNotRegisteredForAndroid() throws Exception {
        try (ActivityScenario<MuonActivity> scenario = ActivityScenario.launch(MuonActivity.class)) {
            AtomicReference<MuonActivity> activityReference = new AtomicReference<>();
            scenario.onActivity(activityReference::set);
            MuonActivity activity = activityReference.get();
            assertNotNull(activity);
            assertTrue(activity.awaitPageReadyForTest(30, TimeUnit.SECONDS));
            activity.clearTestMessages();

            scenario.onActivity(current -> current.getWebViewForTest().evaluateJavascript("""
                    void (async () => {
                      try {
                        await globalThis.__muon_plugin_call(
                          'environment-capability',
                          'muon.environments.getCommandLine',
                          []
                        );
                        muonAndroidTest.postMessage('resolved');
                      } catch (error) {
                        muonAndroidTest.postMessage(
                          error instanceof Error ? error.message : String(error)
                        );
                      }
                    })();
                    """, null));

            String diagnostic = activity.awaitTestMessage(30, TimeUnit.SECONDS);
            assertNotNull(diagnostic);
            assertEquals("Unknown muon plugin function", diagnostic);
        }
    }

    @Test
    public void abortsDelayedCallsAndCancelsNativePendingState() throws Exception {
        try (ActivityScenario<MuonActivity> scenario = ActivityScenario.launch(MuonActivity.class)) {
            AtomicReference<MuonActivity> activityReference = new AtomicReference<>();
            scenario.onActivity(activityReference::set);
            MuonActivity activity = activityReference.get();
            assertNotNull(activity);
            assertTrue(activity.awaitPageReadyForTest(30, TimeUnit.SECONDS));
            activity.clearTestMessages();

            scenario.onActivity(current -> current.getWebViewForTest().evaluateJavascript("""
                    void (async () => {
                      try {
                        await globalThis.__muon_android_prototype.cancelDelayed();
                        muonAndroidTest.postMessage(JSON.stringify({ status: 'resolved' }));
                      } catch (error) {
                        muonAndroidTest.postMessage(JSON.stringify({
                          status: 'rejected',
                          name: error !== null && typeof error === 'object' && 'name' in error
                            ? String(error.name)
                            : '',
                          error: error instanceof Error ? error.message : String(error)
                        }));
                      }
                    })();
                    """, null));

            String message = activity.awaitTestMessage(30, TimeUnit.SECONDS);
            assertNotNull(message);
            JSONObject result = new JSONObject(message);
            assertEquals("rejected", result.getString("status"));
            assertEquals("AbortError", result.getString("name"));
            assertEquals(0, activity.getNativePendingCallCountForTest());
        }
    }

    @Test
    public void roundTripsArrayBufferWithoutJsonEncoding() throws Exception {
        try (ActivityScenario<MuonActivity> scenario = ActivityScenario.launch(MuonActivity.class)) {
            AtomicReference<MuonActivity> activityReference = new AtomicReference<>();
            scenario.onActivity(activityReference::set);
            MuonActivity activity = activityReference.get();
            assertNotNull(activity);
            assertTrue(activity.awaitPageReadyForTest(30, TimeUnit.SECONDS));
            activity.clearTestMessages();

            scenario.onActivity(current -> current.getWebViewForTest().evaluateJavascript("""
                    void (async () => {
                      try {
                        const source = Uint8Array.from([99, 3, 1, 4, 88]);
                        const value = await globalThis.__muon_plugin_call(
                          'prototype-capability',
                          'prototype.echoBinary',
                          [source.subarray(1, 4)]
                        );
                        muonAndroidTest.postMessage(JSON.stringify({
                          status: 'resolved',
                          isArrayBuffer: value instanceof ArrayBuffer,
                          bytes: Array.from(new Uint8Array(value))
                        }));
                      } catch (error) {
                        muonAndroidTest.postMessage(JSON.stringify({
                          status: 'rejected',
                          error: error instanceof Error ? error.message : String(error)
                        }));
                      }
                    })();
                    """, null));

            String message = activity.awaitTestMessage(30, TimeUnit.SECONDS);
            assertNotNull(message);
            JSONObject result = new JSONObject(message);
            assertEquals("resolved", result.getString("status"));
            assertTrue(result.getBoolean("isArrayBuffer"));
            assertEquals("[3,1,4]", result.getJSONArray("bytes").toString());
        }
    }

    @Test
    public void recreatesActivityWithAFreshWorkingRpcContext() throws Exception {
        try (ActivityScenario<MuonActivity> scenario = ActivityScenario.launch(MuonActivity.class)) {
            AtomicReference<MuonActivity> firstReference = new AtomicReference<>();
            scenario.onActivity(firstReference::set);
            MuonActivity first = firstReference.get();
            assertNotNull(first);
            assertTrue(first.awaitPageReadyForTest(30, TimeUnit.SECONDS));

            scenario.recreate();

            AtomicReference<MuonActivity> secondReference = new AtomicReference<>();
            scenario.onActivity(secondReference::set);
            MuonActivity second = secondReference.get();
            assertNotNull(second);
            assertNotSame(first, second);
            assertTrue(second.awaitPageReadyForTest(30, TimeUnit.SECONDS));
            second.clearTestMessages();

            scenario.onActivity(current -> current.getWebViewForTest().evaluateJavascript("""
                    void (async () => {
                      try {
                        const value = await globalThis.__muon_plugin_call(
                          'environment-capability',
                          'muon.environments.getConfigValues',
                          []
                        );
                        muonAndroidTest.postMessage(JSON.stringify({
                          status: 'resolved',
                          heading: document.querySelector('h1')?.textContent,
                          value
                        }));
                      } catch (error) {
                        muonAndroidTest.postMessage(JSON.stringify({
                          status: 'rejected',
                          error: error instanceof Error ? error.message : String(error)
                        }));
                      }
                    })();
                    """, null));

            String message = second.awaitTestMessage(30, TimeUnit.SECONDS);
            assertNotNull(message);
            JSONObject result = new JSONObject(message);
            assertEquals("resolved", result.getString("status"));
            assertEquals("muon Android prototype", result.getString("heading"));
            JSONObject config = new JSONObject(result.getString("value"));
            assertEquals("android", config.getString("channel"));
        }
    }
}

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
                            "fs: Object.keys(globalThis.muon.fs)," +
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
            assertEquals("[\"browser\",\"environments\",\"fs\",\"node\",\"test\"]",
                    api.getJSONArray("namespaces").toString());
            assertEquals(8, api.getJSONArray("browser").length());
            assertEquals(22, api.getJSONArray("fs").length());
            assertFalse(api.getBoolean("hasHardReload"));
        }
    }

    @Test
    public void exposesMultipleQuickJsRuntimesThroughMuonNode() throws Exception {
        try (ActivityScenario<MuonActivity> scenario = ActivityScenario.launch(MuonActivity.class)) {
            AtomicReference<MuonActivity> activityReference = new AtomicReference<>();
            scenario.onActivity(activityReference::set);
            MuonActivity activity = activityReference.get();
            assertNotNull(activity);
            assertTrue(activity.awaitPageReadyForTest(30, TimeUnit.SECONDS));
            activity.clearTestMessages();

            scenario.onActivity(current -> current.getWebViewForTest().evaluateJavascript("""
                    void (async () => {
                      let first;
                      let second;
                      try {
                        first = await globalThis.muon.node.createNode();
                        second = await globalThis.muon.node.createNode();
                        const firstRoot = await first.importModule('.');
                        const secondRoot = await second.importModule('.');
                        const firstCount = await firstRoot.increment();
                        const secondCount = await secondRoot.increment();
                        const integer = await firstRoot.echo(18446744073709551615n);
                        const buffer = await firstRoot.echo(
                          Uint8Array.from([3, 1, 4])
                        );
                        const callback = await firstRoot.invokeCallback(
                          'runtime-value',
                          async (value) => `${value}:renderer`
                        );

                        const promises = await first.importModule('node:fs/promises');
                        await promises.mkdir('activity-test', { recursive: true });
                        await promises.writeFile(
                          'activity-test/value.txt',
                          'from-quickjs'
                        );
                        const text = await promises.readFile(
                          'activity-test/value.txt',
                          'utf8'
                        );
                        const callbackFs = await first.importModule('node:fs');
                        const callbackText = await new Promise((resolve, reject) => {
                          void (async () => {
                            try {
                              await callbackFs.readFile(
                                'activity-test/value.txt',
                                'utf8',
                                (error, value) => {
                                  if (error) {
                                    reject(error);
                                  } else {
                                    resolve(value);
                                  }
                                }
                              );
                            } catch (error) {
                              reject(error);
                            }
                          })();
                        });
                        const path = await first.importModule('node:path');
                        const joined = await path.join('alpha', 'beta', '..', 'gamma');
                        const timers = await first.importModule('node:timers/promises');
                        const timer = await timers.setTimeout(10, 'awake');

                        await first.release();
                        const survivingCount = await secondRoot.increment();
                        await second.release();
                        muonAndroidTest.postMessage(JSON.stringify({
                          status: 'resolved',
                          firstCount,
                          secondCount,
                          integer: integer.toString(),
                          buffer: Array.from(buffer),
                          callback,
                          text,
                          callbackText,
                          joined,
                          timer,
                          survivingCount
                        }));
                      } catch (error) {
                        if (first) {
                          try { await first.release(); } catch {}
                        }
                        if (second) {
                          try { await second.release(); } catch {}
                        }
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
            assertEquals(1, result.getInt("firstCount"));
            assertEquals(1, result.getInt("secondCount"));
            assertEquals("18446744073709551615", result.getString("integer"));
            assertEquals("[3,1,4]", result.getJSONArray("buffer").toString());
            assertEquals("runtime-value:renderer", result.getString("callback"));
            assertEquals("from-quickjs", result.getString("text"));
            assertEquals("from-quickjs", result.getString("callbackText"));
            assertEquals("alpha/gamma", result.getString("joined"));
            assertEquals("awake", result.getString("timer"));
            assertEquals(2, result.getInt("survivingCount"));
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
    public void invokesPackagedPluginsThroughThePublicJavaScriptApi() throws Exception {
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
                        const types = globalThis.muon.test.types;
                        const source = Uint8Array.from([3, 1, 4, 1, 5, 9, 2, 6]);
                        const originalBeforeMutation = Array.from(source);
                        const mutated = Array.from(new Uint8Array(
                          await types.mutateBufferCopy(source.subarray(0))
                        ));
                        const values = {
                          pluginKeys: Object.keys(globalThis.muon.test).sort(),
                          typeKeys: Object.keys(types).sort(),
                          alphaName: await globalThis.muon.test.alpha.alphaName(),
                          alphaAdd: await globalThis.muon.test.alpha.alphaAdd(12, 30),
                          alphaConfig: await globalThis.muon.test.alpha.alphaConfig(),
                          cardioInit: await globalThis.muon.test.cardio
                            .dispatcherAvailableAtInit(),
                          cardioCall: await globalThis.muon.test.cardio
                            .dispatcherAvailable(),
                          bool: await types.echoBool(false),
                          i8: await types.echoI8(-128),
                          u8: await types.echoU8(255),
                          i16: await types.echoI16(-32768),
                          u16: await types.echoU16(65535),
                          i32: await types.echoI32(-2147483648),
                          u32: await types.echoU32(4294967295),
                          i64Safe: await types.echoI64(9007199254740991),
                          i64Truncated: await types.echoI64(1.9),
                          i64NonFinite: await types.echoI64(Infinity),
                          u64SignedMax: await types.echoU64(-1),
                          f32: await types.echoF32(1.25),
                          f64: await types.echoF64(1.25),
                          pointerBits: await types.pointerBitSize(),
                          pointer: await types.echoPointer(4294967296),
                          nullPointer: await types.echoPointer(null),
                          returnedNullPointer: await types.returnNullPointer(),
                          string: await types.echoString('hello'),
                          nullString: await types.echoString(null),
                          returnedNullString: await types.returnNullString(),
                          checksum: await types.bufferChecksum(source.subarray(1, 5)),
                          transformed: Array.from(new Uint8Array(
                            await types.transformBuffer(source.buffer)
                          )),
                          normal: Array.from(new Uint8Array(
                            await types.returnNormalBuffer()
                          )),
                          shared: Array.from(new Uint8Array(
                            await types.returnSharedBuffer()
                          )),
                          originalBeforeMutation,
                          mutated,
                          originalAfterMutation: Array.from(source),
                          asyncValue: await types.resolveAsync(41),
                          resolvedTwice: await types.resolveTwice(),
                          voidType: typeof (await types.returnVoid()),
                        };
                        try {
                          await globalThis.__muon_plugin_call(
                            'muon_test_plugin_alpha',
                            'muon.test.cardio.dispatcherAvailable',
                            []
                          );
                          values.deniedCapability = 'resolved';
                        } catch (error) {
                          values.deniedCapability = error instanceof Error
                            ? error.message
                            : String(error);
                        }
                        muonAndroidTest.postMessage(JSON.stringify({
                          status: 'resolved',
                          values,
                        }));
                      } catch (error) {
                        muonAndroidTest.postMessage(JSON.stringify({
                          status: 'rejected',
                          error: error instanceof Error ? error.message : String(error),
                        }));
                      }
                    })();
                    """, null));

            String message = activity.awaitTestMessage(30, TimeUnit.SECONDS);
            assertNotNull(message);
            JSONObject result = new JSONObject(message);
            assertEquals(message, "resolved", result.getString("status"));
            JSONObject values = result.getJSONObject("values");
            assertEquals("[\"alpha\",\"cardio\",\"functionLifetime\","
                            + "\"recursiveFunctions\",\"types\"]",
                    values.getJSONArray("pluginKeys").toString());
            assertTrue(values.getJSONArray("typeKeys").length() >= 31);
            assertEquals("alpha", values.getString("alphaName"));
            assertEquals(42, values.getInt("alphaAdd"));
            assertEquals("android-registry", values.getString("alphaConfig"));
            assertTrue(values.getBoolean("cardioInit"));
            assertTrue(values.getBoolean("cardioCall"));
            assertFalse(values.getBoolean("bool"));
            assertEquals(-128, values.getInt("i8"));
            assertEquals(255, values.getInt("u8"));
            assertEquals(-32768, values.getInt("i16"));
            assertEquals(65535, values.getInt("u16"));
            assertEquals(-2147483648, values.getInt("i32"));
            assertEquals(4294967295L, values.getLong("u32"));
            assertEquals(9007199254740991L, values.getLong("i64Safe"));
            assertEquals(1, values.getInt("i64Truncated"));
            assertEquals(0, values.getInt("i64NonFinite"));
            assertEquals(-1, values.getInt("u64SignedMax"));
            assertEquals(1.25, values.getDouble("f32"), 0.0);
            assertEquals(1.25, values.getDouble("f64"), 0.0);
            assertEquals(64, values.getInt("pointerBits"));
            assertEquals(4294967296L, values.getLong("pointer"));
            assertEquals(0, values.getInt("nullPointer"));
            assertEquals(0, values.getInt("returnedNullPointer"));
            assertEquals("hello", values.getString("string"));
            assertTrue(values.isNull("nullString"));
            assertTrue(values.isNull("returnedNullString"));
            assertEquals(11, values.getInt("checksum"));
            assertEquals("[163,167,172,160,164,161,164,166]",
                    values.getJSONArray("transformed").toString());
            assertEquals("[31,34,37,40,43,46,49,52]",
                    values.getJSONArray("normal").toString());
            assertEquals("[91,92,93,94,95,96,97,98]",
                    values.getJSONArray("shared").toString());
            assertEquals("[3,1,4,1,5,9,2,6]",
                    values.getJSONArray("originalBeforeMutation").toString());
            assertEquals("[94,88,95,88,92,80,89,93]",
                    values.getJSONArray("mutated").toString());
            assertEquals("[3,1,4,1,5,9,2,6]",
                    values.getJSONArray("originalAfterMutation").toString());
            assertEquals(42, values.getInt("asyncValue"));
            assertEquals("first", values.getString("resolvedTwice"));
            assertEquals("undefined", values.getString("voidType"));
            assertTrue(values.getString("deniedCapability").contains("not allowed"));
        }
    }

    @Test
    public void bridgesRendererFunctionsAndPluginProxyLifetimes() throws Exception {
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
                        const lifetime = globalThis.muon.test.functionLifetime;
                        const recursive = globalThis.muon.test.recursiveFunctions;
                        const callback = () => undefined;
                        const otherCallback = () => undefined;
                        const overlap = await Promise.all([
                          lifetime.lifetimeOverlapSamePointer(callback, callback),
                          lifetime.lifetimeOverlapSamePointer(callback, callback),
                        ]);
                        let invalidArgument = '';
                        try {
                          await lifetime.lifetimeSamePointer(callback, 1);
                        } catch (error) {
                          invalidArgument = error instanceof Error
                            ? error.message
                            : String(error);
                        }

                        let retainedInvocations = 0;
                        const retainedCallback = () => {
                          retainedInvocations += 1;
                        };
                        const retained = await lifetime.lifetimeRetain(retainedCallback);
                        const retainedMatches = await lifetime
                          .lifetimeRetainedMatches(retainedCallback);
                        await lifetime.lifetimeInvokeRetained();
                        await lifetime.lifetimeFinalizeRetained();

                        let proxy;
                        const recursiveProxy = await recursive
                          .recursiveFunctionArgRoundtrip((value) => {
                            proxy = value;
                            return value;
                          });
                        const proxyReleaseDescriptor = Object
                          .getOwnPropertyDescriptor(proxy, 'release');
                        const proxyDisposeDescriptor = Object
                          .getOwnPropertyDescriptor(proxy, Symbol.dispose);
                        const proxyCall = await proxy(41);
                        proxy.release();
                        proxy[Symbol.dispose]();
                        const releasedCall = proxy(41);
                        let releasedCallError = '';
                        try {
                          await releasedCall;
                        } catch (error) {
                          releasedCallError = error instanceof Error
                            ? error.message
                            : String(error);
                        }
                        let releasedArgumentError = '';
                        try {
                          await recursive.recursiveInvoke(proxy);
                        } catch (error) {
                          releasedArgumentError = error instanceof Error
                            ? error.message
                            : String(error);
                        }

                        const recursiveBuffer = Array.from(new Uint8Array(
                          await recursive.recursiveBufferReturnFunction((buffer) => {
                            if (Array.from(new Uint8Array(buffer)).join(',')
                                !== '12,13,14,15') {
                              throw new Error('unexpected outer buffer');
                            }
                            return (innerBuffer) => {
                              if (Array.from(new Uint8Array(innerBuffer)).join(',')
                                  !== '21,22,23,24') {
                                throw new Error('unexpected inner buffer');
                              }
                              return Uint8Array.from([201, 202, 203, 204]).buffer;
                            };
                          })
                        ));

                        const values = {
                          samePointer: await lifetime
                            .lifetimeSamePointer(callback, callback),
                          differentPointer: await lifetime
                            .lifetimeDifferentPointer(callback, otherCallback),
                          asyncSamePointer: await lifetime
                            .lifetimeAsyncSamePointer(callback, callback),
                          overlap,
                          invalidArgument,
                          nullInput: await lifetime.lifetimeNullPointer(null),
                          undefinedInput: await lifetime
                            .lifetimeNullPointer(undefined),
                          nullResult: await lifetime.lifetimeReturnNullFunction(),
                          nullCallback: await lifetime
                            .lifetimeNullCallbackRoundtrip((value) => {
                              if (value !== null) {
                                throw new Error('expected null function argument');
                              }
                              return null;
                            }),
                          undefinedCallback: await lifetime
                            .lifetimeNullCallbackRoundtrip((value) => {
                              if (value !== null) {
                                throw new Error('expected null function argument');
                              }
                              return undefined;
                            }),
                          retained,
                          retainedMatches,
                          retainedInvocations,
                          asyncRetainFinalize: await lifetime
                            .lifetimeAsyncRetainFinalize(callback),
                          recursiveInvoke: await recursive
                            .recursiveInvoke((value) => value + 1),
                          recursiveReturn: await recursive
                            .recursiveReturnFunction((base) =>
                              (value) => base + value),
                          recursiveProxy,
                          proxyType: typeof proxy,
                          proxyCall,
                          releaseType: typeof proxy.release,
                          releaseEnumerable: Object.keys(proxy).includes('release'),
                          releaseFunctionsMatch:
                            proxy.release === proxy[Symbol.dispose],
                          releaseDescriptor: {
                            configurable: proxyReleaseDescriptor?.configurable,
                            enumerable: proxyReleaseDescriptor?.enumerable,
                            writable: proxyReleaseDescriptor?.writable,
                          },
                          disposeDescriptor: {
                            configurable: proxyDisposeDescriptor?.configurable,
                            enumerable: proxyDisposeDescriptor?.enumerable,
                            writable: proxyDisposeDescriptor?.writable,
                          },
                          releasedCallIsPromise: releasedCall instanceof Promise,
                          releasedCallError,
                          releasedArgumentError,
                          recursiveBuffer,
                        };
                        muonAndroidTest.postMessage(JSON.stringify({
                          status: 'resolved',
                          values,
                        }));
                      } catch (error) {
                        muonAndroidTest.postMessage(JSON.stringify({
                          status: 'rejected',
                          error: error instanceof Error ? error.message : String(error),
                        }));
                      }
                    })();
                    """, null));

            String message = activity.awaitTestMessage(30, TimeUnit.SECONDS);
            assertNotNull(message);
            JSONObject result = new JSONObject(message);
            assertEquals(message, "resolved", result.getString("status"));
            JSONObject values = result.getJSONObject("values");
            assertTrue(values.getBoolean("samePointer"));
            assertTrue(values.getBoolean("differentPointer"));
            assertTrue(values.getBoolean("asyncSamePointer"));
            assertEquals("[true,true]", values.getJSONArray("overlap").toString());
            assertEquals("Invalid argument 1: expected function",
                    values.getString("invalidArgument"));
            assertTrue(values.getBoolean("nullInput"));
            assertTrue(values.getBoolean("undefinedInput"));
            assertTrue(values.isNull("nullResult"));
            assertTrue(values.isNull("nullCallback"));
            assertTrue(values.isNull("undefinedCallback"));
            assertTrue(values.getBoolean("retained"));
            assertTrue(values.getBoolean("retainedMatches"));
            assertEquals(1, values.getInt("retainedInvocations"));
            assertTrue(values.getBoolean("asyncRetainFinalize"));
            assertEquals(42, values.getInt("recursiveInvoke"));
            assertEquals(42, values.getInt("recursiveReturn"));
            assertEquals(42, values.getInt("recursiveProxy"));
            assertEquals("function", values.getString("proxyType"));
            assertEquals(42, values.getInt("proxyCall"));
            assertEquals("function", values.getString("releaseType"));
            assertFalse(values.getBoolean("releaseEnumerable"));
            assertTrue(values.getBoolean("releaseFunctionsMatch"));
            assertEquals("{\"configurable\":false,\"enumerable\":false,"
                            + "\"writable\":false}",
                    values.getJSONObject("releaseDescriptor").toString());
            assertEquals("{\"configurable\":false,\"enumerable\":false,"
                            + "\"writable\":false}",
                    values.getJSONObject("disposeDescriptor").toString());
            assertTrue(values.getBoolean("releasedCallIsPromise"));
            assertTrue(values.getString("releasedCallError").toLowerCase()
                    .contains("released"));
            assertTrue(values.getString("releasedArgumentError").toLowerCase()
                    .contains("released"));
            assertEquals("[31,32,33,34]",
                    values.getJSONArray("recursiveBuffer").toString());
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

    @Test
    public void performsFilesystemOperationsThroughThePublicApi() throws Exception {
        try (ActivityScenario<MuonActivity> scenario = ActivityScenario.launch(MuonActivity.class)) {
            AtomicReference<MuonActivity> activityReference = new AtomicReference<>();
            scenario.onActivity(activityReference::set);
            MuonActivity activity = activityReference.get();
            assertNotNull(activity);
            assertTrue(activity.awaitPageReadyForTest(30, TimeUnit.SECONDS));
            activity.clearTestMessages();
            String basePath = activity.getFilesDir().getAbsolutePath()
                    + "/fs-api-" + System.nanoTime();
            String script = """
                    void (async () => {
                      const base = %s;
                      const nested = `${base}/nested`;
                      const textPath = `${nested}/value.txt`;
                      const binaryPath = `${nested}/binary.bin`;
                      const copiedPath = `${nested}/copied.bin`;
                      const renamedPath = `${nested}/renamed.bin`;
                      const linkPath = `${nested}/link`;
                      try {
                        await muon.fs.mkdir(nested, { recursive: true });
                        await muon.fs.writeTextFile(textPath, 'hello', 'utf8');
                        await muon.fs.appendTextFile(textPath, '世界', 'utf-8');
                        const text = await muon.fs.readTextFile(textPath, 'utf8');

                        const source = Uint8Array.from([0, 1, 2, 3, 4]);
                        await muon.fs.writeFile(binaryPath, source.subarray(1, 4));
                        await muon.fs.writeFile(binaryPath, Uint8Array.from([9, 8]), {
                          position: 1
                        });
                        await muon.fs.appendFile(binaryPath, Uint8Array.from([7]));
                        const binary = Array.from(new Uint8Array(
                          await muon.fs.readFile(binaryPath)
                        ));

                        await muon.fs.copyFile(binaryPath, copiedPath, {
                          overwrite: false
                        });
                        await muon.fs.rename(copiedPath, renamedPath);
                        await muon.fs.truncate(renamedPath, 2);
                        const truncated = Array.from(new Uint8Array(
                          await muon.fs.readFile(renamedPath)
                        ));

                        await muon.fs.symlink('value.txt', linkPath, 'file');
                        const followed = await muon.fs.stat(linkPath);
                        const link = await muon.fs.lstat(linkPath);
                        const target = await muon.fs.readlink(linkPath);
                        const textStats = await muon.fs.stat(textPath);
                        const names = await muon.fs.readdir(nested);
                        const dirents = await muon.fs.readdir(nested, {
                          withFileTypes: true
                        });
                        const linkDirent = dirents.find(({ name }) => name === 'link');
                        const accessible = await muon.fs.access(textPath, {
                          mode: ['read', 'write']
                        });
                        const canonical = await muon.fs.realpath(textPath);
                        const missing = await muon.fs.exists(`${nested}/missing`);

                        await muon.fs.mkdir(`${nested}/empty`);
                        await muon.fs.rmdir(`${nested}/empty`);
                        await muon.fs.unlink(linkPath);
                        await muon.fs.rm(base, { recursive: true });
                        const existsAfterRemove = await muon.fs.exists(base);

                        muonAndroidTest.postMessage(JSON.stringify({
                          status: 'resolved',
                          text,
                          binary,
                          truncated,
                          target,
                          followedIsFile: followed.isFile(),
                          linkIsSymbolicLink: link.isSymbolicLink(),
                          direntIsSymbolicLink: linkDirent?.isSymbolicLink() === true,
                          textSize: textStats.size,
                          mtimeIsValid: textStats.mtimeMs >= 0,
                          names,
                          accessible,
                          canonical,
                          missing,
                          existsAfterRemove
                        }));
                      } catch (error) {
                        muonAndroidTest.postMessage(JSON.stringify({
                          status: 'rejected',
                          error: error instanceof Error ? error.message : String(error)
                        }));
                      }
                    })();
                    """.formatted(JSONObject.quote(basePath));

            scenario.onActivity(current -> current.getWebViewForTest()
                    .evaluateJavascript(script, null));

            String message = activity.awaitTestMessage(30, TimeUnit.SECONDS);
            assertNotNull(message);
            JSONObject result = new JSONObject(message);
            assertEquals(message, "resolved", result.getString("status"));
            assertEquals("hello世界", result.getString("text"));
            assertEquals("[1,9,8,7]", result.getJSONArray("binary").toString());
            assertEquals("[1,9]", result.getJSONArray("truncated").toString());
            assertEquals("value.txt", result.getString("target"));
            assertTrue(result.getBoolean("followedIsFile"));
            assertTrue(result.getBoolean("linkIsSymbolicLink"));
            assertTrue(result.getBoolean("direntIsSymbolicLink"));
            assertEquals(11, result.getLong("textSize"));
            assertTrue(result.getBoolean("mtimeIsValid"));
            assertEquals(
                    "[\"binary.bin\",\"link\",\"renamed.bin\",\"value.txt\"]",
                    result.getJSONArray("names").toString());
            assertTrue(result.getBoolean("accessible"));
            assertTrue(result.getString("canonical").endsWith("/nested/value.txt"));
            assertFalse(result.getBoolean("missing"));
            assertFalse(result.getBoolean("existsAfterRemove"));
        }
    }

    @Test
    public void enforcesFilesystemInputAndTextBoundaries() throws Exception {
        try (ActivityScenario<MuonActivity> scenario = ActivityScenario.launch(MuonActivity.class)) {
            AtomicReference<MuonActivity> activityReference = new AtomicReference<>();
            scenario.onActivity(activityReference::set);
            MuonActivity activity = activityReference.get();
            assertNotNull(activity);
            assertTrue(activity.awaitPageReadyForTest(30, TimeUnit.SECONDS));
            activity.clearTestMessages();
            String basePath = activity.getFilesDir().getAbsolutePath()
                    + "/fs-boundary-" + System.nanoTime();
            String script = """
                    void (async () => {
                      const base = %s;
                      const capture = async (operation) => {
                        try {
                          await operation();
                          return { resolved: true };
                        } catch (error) {
                          return {
                            resolved: false,
                            name: error !== null && typeof error === 'object' && 'name' in error
                              ? String(error.name)
                              : '',
                            message: error instanceof Error ? error.message : String(error)
                          };
                        }
                      };
                      try {
                        await muon.fs.mkdir(base);
                        const invalidPath = `${base}/invalid.txt`;
                        await muon.fs.writeFile(
                          invalidPath,
                          Uint8Array.from([0xc3, 0x28])
                        );
                        const invalidUtf8 = await capture(() =>
                          muon.fs.readTextFile(invalidPath, 'utf8')
                        );
                        const nulText = await capture(() =>
                          muon.fs.writeTextFile(`${base}/nul.txt`, 'a\\0b', 'utf8')
                        );
                        const contentUri = await capture(() =>
                          muon.fs.exists('content://dev.muon/document/1')
                        );
                        const junction = await capture(() =>
                          muon.fs.symlink('target', `${base}/junction`, 'junction')
                        );
                        const oversized = await capture(() =>
                          muon.fs.readFile(`${base}/missing`, { length: 67108865 })
                        );
                        const controller = new AbortController();
                        controller.abort();
                        const aborted = await capture(() =>
                          muon.fs.readFile(invalidPath, { signal: controller.signal })
                        );
                        await muon.fs.rm(base, { recursive: true });
                        muonAndroidTest.postMessage(JSON.stringify({
                          status: 'resolved',
                          invalidUtf8,
                          nulText,
                          contentUri,
                          junction,
                          oversized,
                          aborted
                        }));
                      } catch (error) {
                        muonAndroidTest.postMessage(JSON.stringify({
                          status: 'rejected',
                          error: error instanceof Error ? error.message : String(error)
                        }));
                      }
                    })();
                    """.formatted(JSONObject.quote(basePath));

            scenario.onActivity(current -> current.getWebViewForTest()
                    .evaluateJavascript(script, null));

            String message = activity.awaitTestMessage(30, TimeUnit.SECONDS);
            assertNotNull(message);
            JSONObject result = new JSONObject(message);
            assertEquals(message, "resolved", result.getString("status"));
            assertTrue(result.getJSONObject("invalidUtf8").getString("message")
                    .contains("valid UTF-8"));
            assertTrue(result.getJSONObject("nulText").getString("message")
                    .contains("NUL"));
            assertTrue(result.getJSONObject("contentUri").getString("message")
                    .contains("content://"));
            assertTrue(result.getJSONObject("junction").getString("message")
                    .contains("junction symbolic links are unavailable on Android"));
            assertTrue(result.getJSONObject("oversized").getString("message")
                    .contains("67108864"));
            assertEquals("AbortError",
                    result.getJSONObject("aborted").getString("name"));
        }
    }

    @Test
    public void watchesFilesystemChangesAndReleasesTheLease() throws Exception {
        try (ActivityScenario<MuonActivity> scenario = ActivityScenario.launch(MuonActivity.class)) {
            AtomicReference<MuonActivity> activityReference = new AtomicReference<>();
            scenario.onActivity(activityReference::set);
            MuonActivity activity = activityReference.get();
            assertNotNull(activity);
            assertTrue(activity.awaitPageReadyForTest(30, TimeUnit.SECONDS));
            activity.clearTestMessages();
            String basePath = activity.getFilesDir().getAbsolutePath()
                    + "/fs-watch-" + System.nanoTime();
            String script = """
                    void (async () => {
                      const base = %s;
                      try {
                        await muon.fs.mkdir(base);
                        let reported = false;
                        const watcher = await muon.fs.watch(base, async (event) => {
                          if (reported || event.filename !== 'watched.txt') {
                            return;
                          }
                          reported = true;
                          await watcher.close();
                          await muon.fs.rm(base, { recursive: true });
                          muonAndroidTest.postMessage(JSON.stringify({
                            status: 'resolved',
                            event
                          }));
                        });
                        await muon.fs.writeTextFile(
                          `${base}/watched.txt`,
                          'changed',
                          'utf8'
                        );
                      } catch (error) {
                        muonAndroidTest.postMessage(JSON.stringify({
                          status: 'rejected',
                          error: error instanceof Error ? error.message : String(error)
                        }));
                      }
                    })();
                    """.formatted(JSONObject.quote(basePath));

            scenario.onActivity(current -> current.getWebViewForTest()
                    .evaluateJavascript(script, null));

            String message = activity.awaitTestMessage(30, TimeUnit.SECONDS);
            assertNotNull(message);
            JSONObject result = new JSONObject(message);
            assertEquals(message, "resolved", result.getString("status"));
            JSONObject event = result.getJSONObject("event");
            assertEquals("watched.txt", event.getString("filename"));
            assertTrue("rename".equals(event.getString("eventType"))
                    || "change".equals(event.getString("eventType")));
            assertEquals(0, activity.getActiveFilesystemWatchCountForTest());
        }
    }
}

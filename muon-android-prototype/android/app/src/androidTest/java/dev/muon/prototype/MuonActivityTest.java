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
            assertEquals("[\"browser\",\"environments\",\"fs\"]",
                    api.getJSONArray("namespaces").toString());
            assertEquals(8, api.getJSONArray("browser").length());
            assertEquals(22, api.getJSONArray("fs").length());
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

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


import androidx.test.core.app.ActivityScenario;
import androidx.test.ext.junit.runners.AndroidJUnit4;

import org.json.JSONObject;
import org.junit.Test;
import org.junit.runner.RunWith;

import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

@RunWith(AndroidJUnit4.class)
public final class MuonActivityTest {
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
                        const importedPathBasename = await firstRoot.importedPathBasename(
                          'alpha/value.txt'
                        );
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
                        const runtimePrimitives = await firstRoot.exerciseRuntimePrimitives();
                        const streamAndUrl = await firstRoot.exerciseStreamAndUrl();

                        await first.release();
                        const survivingCount = await secondRoot.increment();
                        await second.release();
                        muonAndroidTest.postMessage(JSON.stringify({
                          status: 'resolved',
                          importedPathBasename,
                          firstCount,
                          secondCount,
                          integer: integer.toString(),
                          buffer: Array.from(buffer),
                          callback,
                          text,
                          callbackText,
                          joined,
                          timer,
                          runtimePrimitives,
                          streamAndUrl,
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
            assertEquals("value.txt", result.getString("importedPathBasename"));
            assertEquals(1, result.getInt("firstCount"));
            assertEquals(1, result.getInt("secondCount"));
            assertEquals("18446744073709551615", result.getString("integer"));
            assertEquals("[3,1,4]", result.getJSONArray("buffer").toString());
            assertEquals("runtime-value:renderer", result.getString("callback"));
            assertEquals("from-quickjs", result.getString("text"));
            assertEquals("from-quickjs", result.getString("callbackText"));
            assertEquals("alpha/gamma", result.getString("joined"));
            assertEquals("awake", result.getString("timer"));
            JSONObject runtimePrimitives = result.getJSONObject("runtimePrimitives");
            assertEquals("muon✓!",
                    runtimePrimitives.getJSONObject("buffer").getString("text"));
            assertEquals("AbortError",
                    runtimePrimitives.getJSONObject("abort").getString("abortName"));
            assertFalse(runtimePrimitives
                    .getJSONObject("timers")
                    .getBoolean("cancelledTimeoutCalled"));
            JSONObject streamAndUrl = result.getJSONObject("streamAndUrl");
            assertEquals("MUON-STREAM",
                    streamAndUrl.getJSONObject("stream").getString("output"));
            assertEquals("https://user:pass@example.com:8443/root/child"
                            + "?alpha=3&space=a+b#section",
                    streamAndUrl.getJSONObject("url").getString("href"));
            assertEquals(2, result.getInt("survivingCount"));
        }
    }

}

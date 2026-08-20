/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

package dev.muon.prototype;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;

import androidx.test.core.app.ActivityScenario;
import androidx.test.ext.junit.runners.AndroidJUnit4;

import org.json.JSONObject;
import org.json.JSONArray;
import org.junit.Test;
import org.junit.runner.RunWith;

import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

@RunWith(AndroidJUnit4.class)
public final class MuonNativeRuntimeTest {
    private static JSONObject getRuntimeDiagnostics(
            ActivityScenario<MuonActivity> scenario) throws Exception {
        AtomicReference<String> diagnosticsJson = new AtomicReference<>();
        scenario.onActivity(current -> diagnosticsJson.set(
                current.getNativeRuntimeDiagnosticsForTest()));
        return new JSONObject(diagnosticsJson.get());
    }

    @Test
    public void dispatchesCardioPluginWorkOnTheJavaMainLooper() throws Exception {
        try (ActivityScenario<MuonActivity> scenario = ActivityScenario.launch(MuonActivity.class)) {
            AtomicReference<MuonActivity> activityReference = new AtomicReference<>();
            scenario.onActivity(activityReference::set);
            MuonActivity activity = activityReference.get();
            assertNotNull(activity);
            assertTrue(activity.awaitPageReadyForTest(30, TimeUnit.SECONDS));
            activity.clearNativeRuntimeProbeEventsForTest();

            scenario.onActivity(MuonActivity::startNativeRuntimeProbeForTest);

            assertEquals(
                    "main:127",
                    activity.awaitNativeRuntimeProbeResultForTest(30, TimeUnit.SECONDS));
            assertEquals(
                    "main:true",
                    activity.awaitNativeRuntimeProbeSettlementForTest(30, TimeUnit.SECONDS));
            AtomicReference<String> diagnosticsJson = new AtomicReference<>();
            scenario.onActivity(current -> diagnosticsJson.set(
                    current.getNativeRuntimeDiagnosticsForTest()));
            JSONObject diagnostics = new JSONObject(diagnosticsJson.get());
            assertEquals(1, diagnostics.getInt("liveDispatcherHosts"));
            assertEquals(1, diagnostics.getInt("activeSessions"));
            assertTrue(diagnostics.getInt("runtimeFileDescriptors") >= 2);
            assertEquals(0, diagnostics.getInt("outstandingProbes"));
            assertEquals(0, diagnostics.getInt("leakedDispatcherFileDescriptors"));
            assertTrue(diagnostics.getBoolean("ownerThread"));
        }
    }

    @Test
    public void suppressesPluginCompletionAfterContextRelease() throws Exception {
        try (ActivityScenario<MuonActivity> scenario = ActivityScenario.launch(MuonActivity.class)) {
            AtomicReference<MuonActivity> activityReference = new AtomicReference<>();
            scenario.onActivity(activityReference::set);
            MuonActivity activity = activityReference.get();
            assertNotNull(activity);
            assertTrue(activity.awaitPageReadyForTest(30, TimeUnit.SECONDS));
            activity.clearNativeRuntimeProbeEventsForTest();

            scenario.onActivity(current -> {
                current.startNativeRuntimeProbeForTest();
                current.releaseNativeContextForTest();
            });

            assertEquals(
                    "main:false",
                    activity.awaitNativeRuntimeProbeSettlementForTest(30, TimeUnit.SECONDS));
            assertEquals(0, activity.getNativeRuntimeProbeResultCountForTest());
            AtomicReference<String> diagnosticsJson = new AtomicReference<>();
            scenario.onActivity(current -> diagnosticsJson.set(
                    current.getNativeRuntimeDiagnosticsForTest()));
            JSONObject diagnostics = new JSONObject(diagnosticsJson.get());
            assertEquals(0, diagnostics.getInt("outstandingProbes"));
            assertTrue(diagnostics.getInt("suppressedProbeResults") >= 1);
        }
    }

    @Test
    public void keepsOneRuntimeAcrossRecreationAndReplacesItAfterExit()
            throws Exception {
        JSONObject initial;
        AtomicReference<MuonActivity> firstReference = new AtomicReference<>();
        try (ActivityScenario<MuonActivity> scenario = ActivityScenario.launch(MuonActivity.class)) {
            scenario.onActivity(firstReference::set);
            MuonActivity first = firstReference.get();
            assertNotNull(first);
            assertTrue(first.awaitPageReadyForTest(30, TimeUnit.SECONDS));
            first.clearNativeRuntimeProbeEventsForTest();
            scenario.onActivity(MuonActivity::startNativeRuntimeProbeForTest);
            assertEquals(
                    "main:true",
                    first.awaitNativeRuntimeProbeSettlementForTest(30, TimeUnit.SECONDS));
            AtomicReference<String> initialDiagnosticsJson = new AtomicReference<>();
            scenario.onActivity(current -> initialDiagnosticsJson.set(
                    current.getNativeRuntimeDiagnosticsForTest()));
            initial = new JSONObject(initialDiagnosticsJson.get());

            for (int iteration = 0; iteration < 3; iteration += 1) {
                scenario.recreate();
                AtomicReference<MuonActivity> recreatedReference = new AtomicReference<>();
                scenario.onActivity(recreatedReference::set);
                MuonActivity recreated = recreatedReference.get();
                assertNotNull(recreated);
                assertTrue(recreated.awaitPageReadyForTest(30, TimeUnit.SECONDS));
                recreated.clearNativeRuntimeProbeEventsForTest();
                scenario.onActivity(MuonActivity::startNativeRuntimeProbeForTest);
                assertEquals(
                        "main:true",
                        recreated.awaitNativeRuntimeProbeSettlementForTest(
                                30, TimeUnit.SECONDS));
                AtomicReference<String> currentDiagnosticsJson = new AtomicReference<>();
                scenario.onActivity(current -> currentDiagnosticsJson.set(
                        current.getNativeRuntimeDiagnosticsForTest()));
                JSONObject current = new JSONObject(currentDiagnosticsJson.get());
                assertEquals(initial.getLong("generation"), current.getLong("generation"));
                assertEquals(
                        initial.getLong("createdDispatcherHosts"),
                        current.getLong("createdDispatcherHosts"));
                assertEquals(
                        initial.getInt("runtimeFileDescriptors"),
                        current.getInt("runtimeFileDescriptors"));
                assertEquals(1, current.getInt("liveDispatcherHosts"));
                assertEquals(1, current.getInt("activeSessions"));
                assertEquals(0, current.getInt("leakedDispatcherFileDescriptors"));
            }
        }
        assertTrue(firstReference.get().awaitDestroyedForTest(30, TimeUnit.SECONDS));

        try (ActivityScenario<MuonActivity> replacementScenario =
                     ActivityScenario.launch(MuonActivity.class)) {
            AtomicReference<MuonActivity> replacementReference = new AtomicReference<>();
            replacementScenario.onActivity(replacementReference::set);
            MuonActivity replacement = replacementReference.get();
            assertNotNull(replacement);
            assertTrue(replacement.awaitPageReadyForTest(30, TimeUnit.SECONDS));
            replacement.clearNativeRuntimeProbeEventsForTest();
            replacementScenario.onActivity(MuonActivity::startNativeRuntimeProbeForTest);
            assertEquals(
                    "main:true",
                    replacement.awaitNativeRuntimeProbeSettlementForTest(
                            30, TimeUnit.SECONDS));
            AtomicReference<String> restartedDiagnosticsJson = new AtomicReference<>();
            replacementScenario.onActivity(current -> restartedDiagnosticsJson.set(
                    current.getNativeRuntimeDiagnosticsForTest()));
            JSONObject restarted = new JSONObject(restartedDiagnosticsJson.get());
            assertTrue(restarted.getLong("generation") > initial.getLong("generation"));
            assertEquals(
                    initial.getLong("createdDispatcherHosts") + 1,
                    restarted.getLong("createdDispatcherHosts"));
            assertTrue(
                    restarted.getLong("destroyedDispatcherHosts")
                            >= initial.getLong("destroyedDispatcherHosts") + 1);
            assertEquals(
                    initial.getInt("runtimeFileDescriptors"),
                    restarted.getInt("runtimeFileDescriptors"));
            assertEquals(1, restarted.getInt("liveDispatcherHosts"));
            assertEquals(0, restarted.getInt("leakedDispatcherFileDescriptors"));
        }
    }

    @Test
    public void balancesFunctionClosuresAndUsesExecutableNonWritableMappings()
            throws Exception {
        MuonRpcBridge.clearNativeRuntimeStopEventsForTest();
        long baselineLiveClosures;
        long baselineAllocations;
        long baselineReleases;
        try (ActivityScenario<MuonActivity> scenario = ActivityScenario.launch(MuonActivity.class)) {
            AtomicReference<MuonActivity> activityReference = new AtomicReference<>();
            scenario.onActivity(activityReference::set);
            MuonActivity activity = activityReference.get();
            assertNotNull(activity);
            assertTrue(activity.awaitPageReadyForTest(30, TimeUnit.SECONDS));
            JSONObject baseline = getRuntimeDiagnostics(scenario);
            assertTrue(baseline.getBoolean("ffiClosuresEnabled"));
            baselineLiveClosures = baseline.getLong("ffiClosureLive");
            baselineAllocations = baseline.getLong("ffiClosureAlloc");
            baselineReleases = baseline.getLong("ffiClosureFree");
            activity.clearTestMessages();

            scenario.onActivity(current -> current.getWebViewForTest().evaluateJavascript("""
                    void (async () => {
                      try {
                        const lifetime = globalThis.muon.test.functionLifetime;
                        const callback = () => undefined;
                        if (!await lifetime.lifetimeRetain(callback)) {
                          throw new Error('renderer function was not retained');
                        }
                        await lifetime.lifetimeInvokeRetained();
                        muonAndroidTest.postMessage('retained');
                      } catch (error) {
                        muonAndroidTest.postMessage(
                          error instanceof Error ? error.message : String(error)
                        );
                      }
                    })();
                    """, null));
            assertEquals("retained", activity.awaitTestMessage(30, TimeUnit.SECONDS));

            JSONObject active = getRuntimeDiagnostics(scenario);
            assertEquals(1, active.getInt("functionOwnerSources"));
            assertEquals(1, active.getInt("functionGlobalSources"));
            assertEquals(0, active.getInt("functionGlobalBorrows"));
            assertEquals(0, active.getInt("functionGlobalProxies"));
            assertEquals(0, active.getInt("functionGlobalProxyLeases"));
            assertTrue(active.getLong("ffiClosureLive") > baselineLiveClosures);
            assertTrue(active.getBoolean("closureExecutable"));
            assertFalse(active.getBoolean("closureWritable"));
            assertTrue(active.getString("closureMappingPermissions").contains("x"));

            activity.clearTestMessages();
            scenario.onActivity(current -> current.getWebViewForTest().evaluateJavascript("""
                    void (async () => {
                      try {
                        await globalThis.muon.test.functionLifetime
                          .lifetimeFinalizeRetained();
                        muonAndroidTest.postMessage('finalized');
                      } catch (error) {
                        muonAndroidTest.postMessage(
                          error instanceof Error ? error.message : String(error)
                        );
                      }
                    })();
                    """, null));
            assertEquals("finalized", activity.awaitTestMessage(30, TimeUnit.SECONDS));

            JSONObject finalized = getRuntimeDiagnostics(scenario);
            assertEquals(0, finalized.getInt("functionOwnerSources"));
            assertEquals(0, finalized.getInt("functionGlobalSources"));
            assertEquals(0, finalized.getInt("functionGlobalBorrows"));
            assertEquals(0, finalized.getInt("functionGlobalProxies"));
            assertEquals(0, finalized.getInt("functionGlobalProxyLeases"));
            assertEquals(0, finalized.getInt("pendingRendererFunctionCalls"));
            assertFalse(finalized.getBoolean("trafficTasksPending"));
            assertEquals(0, activity.getNativePendingCallCountForTest());
            assertEquals(baselineLiveClosures, finalized.getLong("ffiClosureLive"));
            long allocationDelta = finalized.getLong("ffiClosureAlloc")
                    - baselineAllocations;
            long releaseDelta = finalized.getLong("ffiClosureFree")
                    - baselineReleases;
            assertTrue(allocationDelta > 0);
            assertEquals(allocationDelta, releaseDelta);
        }

        String stoppedJson = MuonRpcBridge.awaitNativeRuntimeStopForTest(
                30, TimeUnit.SECONDS);
        assertNotNull(stoppedJson);
        JSONObject stopped = new JSONObject(stoppedJson);
        assertEquals("idle", stopped.getString("runtimeState"));
        assertEquals(0, stopped.getInt("liveDispatcherHosts"));
        assertEquals(0, stopped.getInt("liveLibraryHandles"));
        assertEquals(0, stopped.getInt("deferredLibraryHandles"));
        assertEquals(0, stopped.getInt("leakedDispatcherFileDescriptors"));
        assertEquals(
                stopped.getLong("openedLibraryHandles"),
                stopped.getLong("closedLibraryHandles"));
    }

    @Test
    public void rejectsPluginStartupFailuresBeforePageLoadAndRecovers()
            throws Exception {
        String[][] faults = {
                {"missing-library", "Failed to load plugin"},
                {"missing-entry", "muon_init_plugin"},
                {"init-failure", "declined loading"},
                {"invalid-metadata", "namespace"},
                {"duplicate-path", "Duplicate plugin"},
                {"allow-mismatch", "no allowed functions"},
        };

        for (String[] fault : faults) {
            assertNotNull(MuonRpcBridge.awaitNativeRuntimeIdleForTest(
                    30, TimeUnit.SECONDS));
            MuonRpcBridge.clearNativeRuntimeStopEventsForTest();
            assertTrue(MuonRpcBridge.setNativeRuntimeStartupFaultForTest(
                    fault[0], 30, TimeUnit.SECONDS));
            try (ActivityScenario<MuonActivity> scenario =
                         ActivityScenario.launch(MuonActivity.class)) {
                AtomicReference<MuonActivity> activityReference = new AtomicReference<>();
                scenario.onActivity(activityReference::set);
                MuonActivity activity = activityReference.get();
                assertNotNull(activity);
                assertTrue(activity.awaitStartupFailureForTest(30, TimeUnit.SECONDS));
                assertTrue(activity.getStartupFailureForTest().contains(fault[1]));
                assertFalse(activity.wasPageLoadStartedForTest());
            }

            String stoppedJson = MuonRpcBridge.awaitNativeRuntimeStopForTest(
                    30, TimeUnit.SECONDS);
            assertNotNull(stoppedJson);
            JSONObject stopped = new JSONObject(stoppedJson);
            assertEquals("idle", stopped.getString("runtimeState"));
            assertEquals(0, stopped.getInt("liveDispatcherHosts"));
            assertEquals(0, stopped.getInt("liveLibraryHandles"));
            assertEquals(0, stopped.getInt("deferredLibraryHandles"));
            assertEquals(0, stopped.getInt("leakedDispatcherFileDescriptors"));
            assertEquals(
                    stopped.getLong("openedLibraryHandles"),
                    stopped.getLong("closedLibraryHandles"));
            JSONArray closedLibraries = stopped.getJSONArray("lastClosedLibraries");
            assertTrue(closedLibraries.length() >= 2);
            assertEquals(
                    "libmuon_test_plugin_alpha.so",
                    closedLibraries.getString(closedLibraries.length() - 1));
            assertEquals(
                    "libmuon_test_plugin_cardio.so",
                    closedLibraries.getString(closedLibraries.length() - 2));
        }

        MuonRpcBridge.clearNativeRuntimeStopEventsForTest();
        try (ActivityScenario<MuonActivity> scenario = ActivityScenario.launch(MuonActivity.class)) {
            AtomicReference<MuonActivity> activityReference = new AtomicReference<>();
            scenario.onActivity(activityReference::set);
            MuonActivity activity = activityReference.get();
            assertNotNull(activity);
            assertTrue(activity.awaitPageReadyForTest(30, TimeUnit.SECONDS));
        }
        assertNotNull(MuonRpcBridge.awaitNativeRuntimeStopForTest(
                30, TimeUnit.SECONDS));
    }
}

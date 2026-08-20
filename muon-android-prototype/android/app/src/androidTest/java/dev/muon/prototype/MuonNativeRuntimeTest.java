/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

package dev.muon.prototype;

import static org.junit.Assert.assertEquals;
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
public final class MuonNativeRuntimeTest {
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
}

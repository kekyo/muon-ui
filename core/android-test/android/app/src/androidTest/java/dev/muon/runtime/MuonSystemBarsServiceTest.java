/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

package dev.muon.runtime;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import android.os.Build;

import androidx.annotation.NonNull;
import androidx.core.view.ViewCompat;
import androidx.test.core.app.ActivityScenario;
import androidx.test.ext.junit.runners.AndroidJUnit4;

import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.Test;
import org.junit.runner.RunWith;

import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

/** Verifies Window contracts independently of the device's installed WebView version. */
@RunWith(AndroidJUnit4.class)
public final class MuonSystemBarsServiceTest {
    private static final class Result implements MuonAndroidPlatformService.Completion {
        final LinkedBlockingQueue<String> values = new LinkedBlockingQueue<>();
        @Override public void completeVoid() { values.add("void"); }
        @Override public void completeString(@NonNull String value) { values.add(value); }
        @Override public void fail(@NonNull String diagnostic) { values.add("error:" + diagnostic); }
        @Override public void completeUnsignedInteger(long value) { throw new AssertionError("Unexpected integer result"); }
        @Override public void completeBoolean(boolean value) { throw new AssertionError("Unexpected boolean result"); }
        @Override public void completeBinary(@NonNull byte[] value) { throw new AssertionError("Unexpected binary result"); }
        String await() throws InterruptedException {
            String value = values.poll(30, TimeUnit.SECONDS);
            assertNotNull("Platform call must settle", value);
            return value;
        }
    }

    @Test
    public void appliesInitialSettingsAndValidatesIconSupportAtomically() throws Exception {
        try (ActivityScenario<MuonWindowActivity> scenario = ActivityScenario.launch(MuonWindowActivity.class)) {
            AtomicReference<MuonAndroidSystemBarsService> service = new AtomicReference<>();
            JSONObject config = new JSONObject("{\"contentLayout\":\"edge-to-edge\","
                    + "\"statusBar\":{\"visible\":false,\"icons\":\"dark\"},"
                    + "\"navigationBar\":{\"visible\":true,\"icons\":\"light\"}}");
            Result state = new Result();
            scenario.onActivity(activity -> {
                service.set(new MuonAndroidSystemBarsService(activity.getWindow(),
                        activity.getResources().getDisplayMetrics().density, config, null));
                service.get().invoke(1, "muon.android.systemBars.getState", new JSONArray(), new byte[0][], state);
            });
            JSONObject initial = new JSONObject(state.await()).getJSONObject("requested");
            assertEquals("edge-to-edge", initial.getString("contentLayout"));
            assertFalse(initial.getJSONObject("statusBar").getBoolean("visible"));
            assertEquals("dark", initial.getJSONObject("statusBar").getString("icons"));
            assertEquals("light", initial.getJSONObject("navigationBar").getString("icons"));
            Result configured = new Result();
            scenario.onActivity(activity -> service.get().invoke(2, "muon.android.systemBars.configure",
                    new JSONArray().put("{\"contentLayout\":\"safe-area\","
                            + "\"statusBar\":{\"visible\":true,\"icons\":\"light\"},"
                            + "\"navigationBar\":{\"icons\":\"dark\"}}"), new byte[0][], configured));
            String outcome = configured.await();
            scenario.onActivity(activity -> service.get().invoke(3, "muon.android.systemBars.getState",
                    new JSONArray(), new byte[0][], state));
            JSONObject after = new JSONObject(state.await());
            if (Build.VERSION.SDK_INT < 26) {
                assertTrue(outcome, outcome.startsWith("error:"));
                assertTrue(outcome, outcome.contains("API 26"));
                assertEquals(initial.toString(), after.getJSONObject("requested").toString());
            } else {
                assertEquals("void", outcome);
                assertEquals("safe-area", after.getJSONObject("requested").getString("contentLayout"));
                assertEquals("dark", after.getJSONObject("actual").getJSONObject("navigationBar").getString("icons"));
            }
            scenario.onActivity(activity -> service.get().close());
        }
    }

    @Test
    public void waitsForAnObservedInsetSnapshotAndCancelsAnUnattachedAcquisition() throws Exception {
        try (ActivityScenario<MuonWindowActivity> scenario = ActivityScenario.launch(MuonWindowActivity.class)) {
            AtomicReference<MuonAndroidSystemBarsService> service = new AtomicReference<>();
            Result insets = new Result();
            Result acquired = new Result();
            scenario.onActivity(activity -> {
                service.set(new MuonAndroidSystemBarsService(activity.getWindow(),
                        activity.getResources().getDisplayMetrics().density, new JSONObject(), null));
                service.get().invoke(1, "muon.android.systemBars.getInsets", new JSONArray(), new byte[0][], insets);
                service.get().invoke(2, "muon.android.systemBars.watchInsets",
                        new JSONArray().put("{\"operation\":\"acquire\"}"), new byte[0][], acquired);
                assertNull(insets.values.peek());
                assertNull(acquired.values.peek());
                assertEquals(1, service.get().getActiveWatchCount());
                service.get().cancel(2);
                assertEquals(0, service.get().getActiveWatchCount());
                service.get().attach(activity.content, () -> {});
                ViewCompat.requestApplyInsets(activity.content);
            });
            JSONObject observed = new JSONObject(insets.await());
            assertTrue(observed.getLong("revision") > 0);
            assertEquals("css-px", observed.getString("unit"));
            assertTrue(observed.getJSONObject("safeArea").getDouble("top") > 0);
            assertNull(acquired.values.peek());
            scenario.onActivity(activity -> service.get().close());
        }
    }
}

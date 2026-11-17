/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
package dev.muon.runtime;

import static org.junit.Assert.*;
import android.content.Intent;
import androidx.test.core.app.ActivityScenario;
import androidx.test.platform.app.InstrumentationRegistry;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import org.json.JSONObject;
import org.junit.Test;
import org.junit.runner.RunWith;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

/** Real FD and plugin work must share a dispatcher without sharing ownership. */
@RunWith(AndroidJUnit4.class)
public final class MuonServiceDispatcherTest {
    private static JSONObject request(MuonServiceClient client, String operation, JSONObject payload) throws Exception {
        CountDownLatch done = new CountDownLatch(1);
        AtomicReference<JSONObject> value = new AtomicReference<>();
        AtomicReference<String> failure = new AtomicReference<>();
        InstrumentationRegistry.getInstrumentation().runOnMainSync(() -> client.invoke(operation, payload,
                new MuonServiceClient.Completion() {
                    @Override public void complete(JSONObject result) { value.set(result); done.countDown(); }
                    @Override public void fail(String error) { failure.set(error); done.countDown(); }
                }));
        assertTrue("RPC completion", done.await(30, TimeUnit.SECONDS));
        assertNull(failure.get());
        return value.get();
    }

    private static JSONObject rpc(MuonServiceClient client, JSONObject reference, String action, JSONObject payload) throws Exception {
        return request(client, "rpc", new JSONObject(reference.toString()).put("action", action).put("payload", payload));
    }

    private static JSONObject idle() throws Exception {
        String result = MuonRpcBridge.awaitNativeRuntimeIdleForTest(30, TimeUnit.SECONDS);
        assertNotNull("Native plugin stop completed", result);
        return new JSONObject(result);
    }

    @Test public void closesFdClientBeforePluginRuntime() throws Exception { exercise(false); }
    @Test public void retainsPluginCodeUntilTheLastFdClientDrains() throws Exception { exercise(true); }

    @Test public void closesBeforeTheBinderDescriptorArrives() throws Exception {
        var context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        Intent intent = new Intent(context, MuonServiceRpcActivity.class)
                .putExtra("owner", "close-before-bind").putExtra("deferHost", true);
        AtomicReference<MuonServiceClient> client = new AtomicReference<>();
        java.util.concurrent.atomic.AtomicInteger results = new java.util.concurrent.atomic.AtomicInteger();
        try (ActivityScenario<MuonServiceRpcActivity> scenario = ActivityScenario.launch(intent)) {
            JSONObject payload = new JSONObject().put("serviceId", "alpha.v1-test").put("refId", "1")
                    .put("generation", "1").put("action", "describe").put("payload", new JSONObject());
            scenario.onActivity(current -> {
                client.set(new MuonServiceClient(current));
                client.get().invoke("rpc", payload, new MuonServiceClient.Completion() {
                    @Override public void complete(JSONObject value) { results.addAndGet(1000); }
                    @Override public void fail(String error) { results.incrementAndGet(); }
                });
                // No Looper turn has yet delivered the Binder connection or FD.
                client.get().close();
                client.get().close();
            });
            assertTrue("Pending connect and queued call drain", client.get().awaitRpcClosedForTest(30, TimeUnit.SECONDS));
            assertEquals(1, results.get());
        }
    }

    private static void exercise(boolean serviceFirst) throws Exception {
        var context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        Intent intent = new Intent(context, MuonServiceRpcActivity.class)
                .putExtra("owner", "dispatcher-test").putExtra("deferHost", serviceFirst);
        AtomicReference<MuonServiceClient> client = new AtomicReference<>();
        try (ActivityScenario<MuonServiceRpcActivity> scenario = ActivityScenario.launch(intent)) {
            AtomicReference<MuonServiceRpcActivity> activity = new AtomicReference<>();
            scenario.onActivity(activity::set);
            if (!serviceFirst) assertTrue(activity.get().awaitPageForTest());
            scenario.onActivity(current -> client.set(new MuonServiceClient(current)));
            JSONObject reference = request(client.get(), "acquire", new JSONObject().put("serviceId", "alpha.v1-test"));
            assertTrue(rpc(client.get(), reference, "describe", new JSONObject()).has("methods"));
            if (serviceFirst) { scenario.onActivity(MuonServiceRpcActivity::startHostForTest); assertTrue(activity.get().awaitPageForTest()); }
            AtomicReference<String> initial = new AtomicReference<>();
            scenario.onActivity(current -> initial.set(current.runtimeDiagnosticsForTest()));
            assertEquals(1, new JSONObject(initial.get()).getInt("liveLibraryHandles"));
            if (serviceFirst) {
                MuonRpcBridge.clearNativeRuntimeStopEventsForTest();
                scenario.onActivity(MuonServiceRpcActivity::closeHostForTest);
                JSONObject stopped = idle();
                assertEquals(1, stopped.getInt("liveLibraryHandles"));
                assertEquals(1, stopped.getInt("deferredLibraryHandles"));
                assertEquals(0, stopped.getInt("activeSessions"));
                JSONObject call = rpc(client.get(), reference, "begin", new JSONObject()
                        .put("method", "echo").put("arguments", new org.json.JSONArray().put("after-plugin-stop")));
                assertEquals("after-plugin-stop", rpc(client.get(), reference, "result", call).getString("value"));
            }
            scenario.onActivity(current -> client.get().close());
            assertTrue("FD and cardio tasks drained", client.get().awaitRpcClosedForTest(30, TimeUnit.SECONDS));
            if (!serviceFirst) {
                MuonRpcBridge.clearNativeRuntimeStopEventsForTest();
                scenario.onActivity(MuonServiceRpcActivity::closeHostForTest);
            }
            JSONObject stopped = idle();
            assertEquals(0, stopped.getInt("liveLibraryHandles"));
            assertEquals(0, stopped.getInt("deferredLibraryHandles"));
            assertEquals(stopped.getLong("openedLibraryHandles"), stopped.getLong("closedLibraryHandles"));
            assertEquals(0, stopped.getInt("leakedDispatcherFileDescriptors"));
        } finally {
            if (client.get() != null) InstrumentationRegistry.getInstrumentation().runOnMainSync(() -> client.get().close());
        }
    }
}

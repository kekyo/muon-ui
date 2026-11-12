package dev.muon.runtime;

import static org.junit.Assert.*;
import android.os.Handler;
import android.os.Looper;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import java.util.Arrays;
import java.util.Collections;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import org.json.JSONObject;
import org.junit.Test;
import org.junit.runner.RunWith;

/** Historical OS observations never change the current service generation. */
@RunWith(AndroidJUnit4.class)
public final class MuonServiceExitHistoryTest {
    @Test public void exposesHistoryThroughTheCurrentPlatformState() throws Exception {
        CountDownLatch done = new CountDownLatch(1);
        AtomicReference<JSONObject> snapshot = new AtomicReference<>();
        AtomicReference<String> failure = new AtomicReference<>();
        InstrumentationRegistry.getInstrumentation().runOnMainSync(() -> {
            MuonServiceClient client = new MuonServiceClient(InstrumentationRegistry.getInstrumentation().getTargetContext());
            try {
                client.invoke("getState", new JSONObject().put("serviceId", "beta"), new MuonServiceClient.Completion() {
                    @Override public void complete(JSONObject result) { snapshot.set(result); client.close(); done.countDown(); }
                    @Override public void fail(String error) { failure.set(error); client.close(); done.countDown(); }
                });
            } catch (Exception error) { failure.set(error.toString()); client.close(); done.countDown(); }
        });
        assertTrue(done.await(30, TimeUnit.SECONDS));
        assertNull(failure.get());
        JSONObject state = snapshot.get();
        assertEquals("stopped", state.getString("state"));
        assertEquals(0, state.getInt("clientReferences"));
        assertTrue(state.has("lastHostExit"));
        if (android.os.Build.VERSION.SDK_INT < 30) assertTrue(state.isNull("lastHostExit"));
        else if (!state.isNull("lastHostExit"))
            assertEquals("android", state.getJSONObject("lastHostExit").getString("platform"));
    }

    @Test public void selectsTheLatestExactHostFromAllRetainedProcesses() throws Exception {
        JSONObject result = MuonServiceExitHistory.latest("app:muon_services", Arrays.asList(
                new MuonServiceExitHistory.Entry("app", 99, 900, 10, 0, "UI only"),
                new MuonServiceExitHistory.Entry("other:muon_services", 88, 800, 10, 0, null),
                new MuonServiceExitHistory.Entry("app:muon_services", 11, 100, 2, 9, "old"),
                new MuonServiceExitHistory.Entry("app:muon_services", 22, 200, 10, 0, "停止😀")));
        assertEquals(22, result.getInt("pid"));
        assertEquals("200", result.getString("timestampMs"));
        assertEquals("user-requested", result.getString("reason"));
        assertEquals("停止😀", result.getString("description"));
        assertEquals("android", result.getString("platform"));
    }

    @Test public void preservesUnknownOsReasonsAndAbsentDescriptions() throws Exception {
        JSONObject result = MuonServiceExitHistory.latest("host", Collections.singletonList(
                new MuonServiceExitHistory.Entry("host", 10, Long.MAX_VALUE, 12345, 42, null)));
        assertEquals("unknown", result.getString("reason"));
        assertEquals(12345, result.getInt("reasonCode"));
        assertEquals(42, result.getInt("status"));
        assertEquals(Long.toString(Long.MAX_VALUE), result.getString("timestampMs"));
        assertTrue(result.isNull("description"));
        assertNull(MuonServiceExitHistory.latest("other", Collections.singletonList(
                new MuonServiceExitHistory.Entry("host", 10, 20, 10, 0, null))));
    }

    @Test public void olderApisNeverResolveTheUnavailableAndroidApi() throws Exception {
        CountDownLatch done = new CountDownLatch(1);
        AtomicReference<Looper> delivered = new AtomicReference<>();
        AtomicReference<Object> value = new AtomicReference<>("pending");
        MuonServiceExitHistory.load(24, "host", () -> { throw new AssertionError("Unavailable API accessed"); },
                new Handler(Looper.getMainLooper()), (result, error) -> {
                    value.set(result == null ? error : result); delivered.set(Looper.myLooper()); done.countDown();
                });
        assertTrue(done.await(30, TimeUnit.SECONDS));
        assertNull(value.get());
        assertSame(Looper.getMainLooper(), delivered.get());
    }

    @Test public void reportsHistoryFailureWithoutFabricatingAnExit() throws Exception {
        CountDownLatch done = new CountDownLatch(1);
        AtomicReference<Looper> reading = new AtomicReference<>();
        AtomicReference<String> failure = new AtomicReference<>();
        AtomicReference<Object> value = new AtomicReference<>("pending");
        MuonServiceExitHistory.load(30, "host", () -> {
            reading.set(Looper.myLooper()); throw new SecurityException("history unavailable");
        }, new Handler(Looper.getMainLooper()), (result, error) -> {
            value.set(result); failure.set(error); done.countDown();
        });
        assertTrue(done.await(30, TimeUnit.SECONDS));
        assertNull(value.get());
        assertNotSame(Looper.getMainLooper(), reading.get());
        assertTrue(failure.get().contains("history unavailable"));
    }
}

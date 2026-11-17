package dev.muon.e2e.observer;

import static org.junit.Assert.*;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import androidx.test.uiautomator.By;
import androidx.test.uiautomator.UiDevice;
import androidx.test.uiautomator.UiObject2;
import androidx.test.uiautomator.Until;
import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.util.UUID;
import org.json.JSONObject;
import org.junit.After;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;

/** Observes a real host death from another UID without retaining the service. */
@RunWith(AndroidJUnit4.class)
public final class ServiceExitReasonTest {
    private static final String APP = "dev.muon.servicefixture";
    private final UiDevice device = UiDevice.getInstance(InstrumentationRegistry.getInstrumentation());
    private String shell(String value) throws Exception { return device.executeShellCommand(value).trim(); }

    @Before @After public void reset() throws Exception {
        shell("am force-stop " + APP);
        shell("run-as " + APP + " rm -rf files/service-autostart files/constraint-result.json");
    }

    private JSONObject start() throws Exception {
        shell("am start -W -n " + APP + "/dev.muon.runtime.MuonServiceConstraintsActivity --es service alpha.v1-test");
        UiObject2 ready = device.wait(Until.findObject(By.pkg(APP).desc("constraint-running")), 60000);
        assertNotNull("The current service must start normally", ready);
        return new JSONObject(ready.getText());
    }

    private void restartedHistory(String command) throws Exception {
        start();
        int oldPid;
        long stoppedAt;
        try (Socket socket = new Socket()) {
            socket.connect(new InetSocketAddress("127.0.0.1", 49171), 60000);
            socket.setSoTimeout(60000);
            String nonce = UUID.randomUUID().toString();
            socket.getOutputStream().write((nonce + "\n").getBytes(StandardCharsets.UTF_8));
            String reply = new BufferedReader(new InputStreamReader(
                    socket.getInputStream(), StandardCharsets.UTF_8)).readLine();
            assertNotNull("Native processing must reply", reply);
            JSONObject processed = new JSONObject(reply);
            assertEquals(nonce, processed.getString("nonce"));
            oldPid = processed.getInt("pid");
            stoppedAt = System.currentTimeMillis();
            shell(command + " " + APP);
            assertEquals("The previous host must really exit", -1, socket.getInputStream().read());
        }
        JSONObject watched = start();
        assertEquals("running", watched.getString("state"));
        assertEquals("", watched.getString("error"));
        assertEquals(0, watched.getInt("clientReferences"));
        if (android.os.Build.VERSION.SDK_INT < 30) {
            assertTrue(watched.has("lastHostExit"));
            assertTrue(watched.isNull("lastHostExit"));
            return;
        }
        JSONObject history = watched.getJSONObject("lastHostExit");
        assertEquals("android", history.getString("platform"));
        assertEquals(APP + ":muon_services", history.getString("processName"));
        assertEquals(oldPid, history.getInt("pid"));
        assertEquals("user-requested", history.getString("reason"));
        assertEquals(10, history.getInt("reasonCode"));
        long timestamp = Long.parseLong(history.getString("timestampMs"));
        assertTrue(history.toString(), timestamp >= stoppedAt);
        assertTrue(history.toString(), timestamp <= System.currentTimeMillis());
        String result = shell("am broadcast -n " + APP
                + "/dev.muon.runtime.MuonServiceConstraintReceiver --es service alpha.v1-test --es operation inspect");
        assertTrue(result, result.contains("result=0"));
        JSONObject current = new JSONObject(shell("run-as " + APP + " cat files/constraint-result.json"))
                .getJSONObject("state");
        assertEquals("getState and watchState must expose the same historical observation",
                history.toString(), current.getJSONObject("lastHostExit").toString());
    }

    @Test public void reportsTheHostStoppedByActiveAppsAfterRestart() throws Exception {
        org.junit.Assume.assumeTrue(android.os.Build.VERSION.SDK_INT >= 33);
        restartedHistory("cmd activity stop-app");
    }

    @Test public void reportsTheForceStoppedHostAfterRestart() throws Exception {
        restartedHistory("am force-stop");
    }
}

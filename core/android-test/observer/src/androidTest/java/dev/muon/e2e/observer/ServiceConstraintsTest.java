package dev.muon.e2e.observer;

import static org.junit.Assert.*;
import static org.junit.Assume.assumeTrue;
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
import java.util.regex.Pattern;
import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.After;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;

/** The observer has no Binder connection or lifetime reference to the target. */
@RunWith(AndroidJUnit4.class)
public final class ServiceConstraintsTest {
    private static final String APP = "dev.muon.servicefixture";
    private static final String PERMISSION = "android.permission.POST_NOTIFICATIONS";
    private final UiDevice device = UiDevice.getInstance(InstrumentationRegistry.getInstrumentation());
    private String shell(String value) throws Exception { return device.executeShellCommand(value).trim(); }
    @Before @After public void reset() throws Exception {
        shell("am force-stop " + APP);
        shell("run-as " + APP + " rm -rf files/service-autostart files/services/held files/constraint-result.json files/constraint-notifications.json");
    }
    private JSONObject launch(String mode) throws Exception {
        shell("am start -W -n " + APP + "/dev.muon.runtime.MuonServiceDriverActivity --es mode " + mode);
        UiObject2 ready = device.wait(Until.findObject(By.pkg(APP).desc(Pattern.compile(
                "service-driver-(ready|preference-ready|error)"))), 60000);
        assertNotNull(ready);
        assertFalse(ready.getText(), ready.getContentDescription().equals("service-driver-error"));
        return new JSONObject(ready.getText());
    }
    private JSONObject state(String description) throws Exception {
        UiObject2 ready = device.wait(Until.findObject(By.pkg(APP).desc(description)), 60000);
        assertNotNull(description, ready);
        return new JSONObject(ready.getText());
    }
    private Socket connect(int port) throws Exception {
        Socket socket = new Socket();
        socket.connect(new InetSocketAddress("127.0.0.1", port), 60000);
        socket.setSoTimeout(60000);
        return socket;
    }
    private JSONObject challenge(Socket socket) throws Exception {
        String nonce = UUID.randomUUID().toString();
        socket.getOutputStream().write((nonce + "\n").getBytes(StandardCharsets.UTF_8));
        String line = new BufferedReader(new InputStreamReader(socket.getInputStream(), StandardCharsets.UTF_8)).readLine();
        assertNotNull("Native processing must reply", line);
        JSONObject result = new JSONObject(line);
        assertEquals(nonce, result.getString("nonce"));
        return result;
    }
    private JSONObject inspect(String id, boolean start) throws Exception {
        // No foreground receiver or temporary allowlist options are supplied.
        // An ordinary explicit shell broadcast does not permit starting an FGS.
        String command = "am broadcast --include-stopped-packages -n " + APP
                + "/dev.muon.runtime.MuonServiceConstraintReceiver --es service " + id
                + " --es operation " + (start ? "start" : "inspect");
        String result = shell(command);
        assertTrue(result, result.contains("result=0"));
        JSONObject response = new JSONObject(shell("run-as " + APP + " cat files/constraint-result.json"));
        assertFalse(response.toString(), response.has("error"));
        return response.getJSONObject("state");
    }
    private boolean notificationsGranted() throws Exception {
        // Cross-package PackageManager checks are filtered by package visibility.
        // Inspect the target's actual OS permission state from the shell instead.
        String permissions = shell("dumpsys package " + APP);
        java.util.regex.Matcher match = Pattern.compile("android\\.permission\\.POST_NOTIFICATIONS: granted=(true|false)")
                .matcher(permissions);
        assertTrue(permissions, match.find());
        return Boolean.parseBoolean(match.group(1));
    }
    @Test public void processesForegroundWorkWithNotificationPermissionGrantedAndDenied() throws Exception {
        boolean original = notificationsGranted();
        try {
            for (boolean granted : new boolean[]{true, false}) {
                reset();
                shell("pm " + (granted ? "grant " : "revoke ") + APP + " " + PERMISSION);
                assertEquals(granted, notificationsGranted());
                launch("normal");
                try (Socket socket = connect(49171)) { challenge(socket); }
                JSONObject current = inspect("alpha.v1-test", false);
                assertEquals("running", current.getString("state"));
                assertTrue(current.getBoolean("resident"));
                assertTrue(current.getBoolean("autostart"));
                assertTrue(shell("dumpsys activity services " + APP).contains("isForeground=true"));
            }
        } finally { shell("pm " + (original ? "grant " : "revoke ") + APP + " " + PERMISSION); }
    }
    @Test public void rejectsBackgroundStartupWithoutAVisibleOrInstrumentedTarget() throws Exception {
        assertFalse(shell("pidof " + APP).matches(".*[0-9].*"));
        assertFalse(shell("dumpsys activity instrumentation").contains("target=" + APP));
        assertTrue(device.wait(Until.gone(By.pkg(APP)), 60000));
        JSONObject failed = inspect("beta", true);
        assertEquals("failed", failed.getString("state"));
        assertTrue(failed.getString("error"), failed.getString("error").contains("ForegroundServiceStartNotAllowedException"));
        assertTrue(failed.getBoolean("autostart"));
        assertTrue(failed.getBoolean("resident"));
        assertEquals(0, failed.getInt("clientReferences"));
        assertFalse(shell("dumpsys activity services " + APP).contains("isForeground=true"));
    }
    @Test public void foregroundsAndPostsNotificationBeforeNativeInitializationCompletes() throws Exception {
        boolean original = notificationsGranted();
        try {
            shell("pm grant " + APP + " " + PERMISSION);
            shell("am start -W -n " + APP + "/dev.muon.runtime.MuonServiceConstraintsActivity --es service held");
            assertNotNull(device.wait(Until.findObject(By.pkg(APP).desc(Pattern.compile(
                    "constraint-(starting|running|error)"))), 60000));
            try (Socket socket = connect(49173)) {
                challenge(socket); // Native factory entered but its completion is gated.
                JSONObject starting = inspect("held", false);
                assertEquals("starting", starting.getString("state"));
                assertEquals(0, starting.getInt("clientReferences"));
                String details = shell("dumpsys activity services " + APP + "/dev.muon.runtime.MuonFixtureHeldService");
                assertTrue(details, details.contains("isForeground=true"));
                // Query the OS from the posting app. Notification shade grouping
                // and unrelated notifications must not determine this assertion.
                String queried = shell("am broadcast -n " + APP
                        + "/dev.muon.runtime.MuonServiceConstraintReceiver --es operation notifications");
                assertTrue(queried, queried.contains("result=0"));
                JSONArray notifications = new JSONArray(shell("run-as " + APP + " cat files/constraint-notifications.json"));
                boolean posted = false;
                for (int index = 0; index < notifications.length(); ++index) {
                    JSONObject notification = notifications.getJSONObject(index);
                    if (notification.optString("title").equals("Muon held fixture")) {
                        assertTrue(notification.toString(), notification.getBoolean("foreground"));
                        posted = true;
                    }
                }
                assertTrue(notifications.toString(), posted);
                assertEquals("starting", inspect("held", false).getString("state"));
                String released = shell("am broadcast -n " + APP
                        + "/dev.muon.runtime.MuonServiceConstraintReceiver --es operation release-start");
                assertTrue(released, released.contains("result=0"));
                state("constraint-running");
                challenge(socket);
            }
        } finally { shell("pm " + (original ? "grant " : "revoke ") + APP + " " + PERMISSION); }
    }
    @Test public void acceptsRealOsTimeoutWithoutRestartingOrStoppingOtherServices() throws Exception {
        // Never alter a physical device's duration budget.
        assumeTrue("Timeout configuration is restricted to the disposable emulator", shell("getprop ro.kernel.qemu").equals("1"));
        String key = "activity_manager data_sync_fgs_timeout_duration";
        String original = shell("device_config get " + key);
        try {
            shell("device_config put " + key + " 3000");
            assertEquals("3000", shell("device_config get " + key));
            shell("am start -W -n " + APP + "/dev.muon.runtime.MuonServiceConstraintsActivity --es service timed");
            JSONObject before = state("constraint-running");
            try (Socket timed = connect(49174); Socket alpha = connect(49171)) {
                challenge(timed);
                JSONObject other = challenge(alpha);
                device.pressHome();
                assertTrue(device.wait(Until.gone(By.pkg(APP)), 60000));
                // EOF is generated by real onTimeout cleanup; no sleep or UI
                // relaunch can reset the OS duration budget before observation.
                assertEquals(-1, timed.getInputStream().read());
                JSONObject failed = inspect("timed", false);
                assertEquals("failed", failed.getString("state"));
                assertTrue(failed.getString("error"), failed.getString("error").contains("foreground-service deadline"));
                assertEquals(before.getString("generation"), failed.getString("generation"));
                assertTrue(failed.getBoolean("autostart"));
                assertTrue(failed.getBoolean("resident"));
                assertEquals(0, failed.getInt("clientReferences"));
                assertEquals(other.getInt("pid"), challenge(alpha).getInt("pid"));
                assertFalse(shell("dumpsys activity services " + APP + "/dev.muon.runtime.MuonFixtureTimedService")
                        .contains("isForeground=true"));
            }
        } finally {
            shell("am force-stop " + APP);
            shell("device_config " + (original.equals("null") ? "delete " + key : "put " + key + " " + original));
            assertEquals(original, shell("device_config get " + key));
        }
    }
    @Test public void restoresSavedOffAfterTheActiveAppsStopAction() throws Exception {
        launch("preferences");
        try (Socket socket = connect(49171)) {
            challenge(socket);
            device.findObject(By.desc("autostart-off")).click();
            JSONObject off = state("service-driver-autostart-off");
            assertFalse(off.getBoolean("autostart"));
            assertEquals(2, off.getInt("clientReferences"));
            shell("cmd activity stop-app " + APP);
            assertEquals(-1, socket.getInputStream().read());
            JSONObject restored = launch("inspect-preferences");
            assertEquals("stopped", restored.getString("state"));
            assertFalse(restored.getBoolean("autostart"));
            assertEquals(0, restored.getInt("clientReferences"));
        }
    }
}

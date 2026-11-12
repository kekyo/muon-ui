// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

package dev.muon.e2e.observer;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;

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
import org.json.JSONObject;
import org.junit.Test;
import org.junit.Before;
import org.junit.After;
import org.junit.runner.RunWith;

/** Uses no service Binder or lifetime reference in the observed application. */
@RunWith(AndroidJUnit4.class)
public final class NativeServiceProcessTest {
    private static final String APPLICATION = "dev.muon.servicefixture";
    private final UiDevice device = UiDevice.getInstance(InstrumentationRegistry.getInstrumentation());

    @Before @After public void resetPreferences() throws Exception {
        device.executeShellCommand("am force-stop " + APPLICATION);
        device.executeShellCommand("run-as " + APPLICATION + " rm -rf files/service-autostart");
    }

    private JSONObject startDriver() throws Exception {
        return startDriver("normal", "ready");
    }

    private JSONObject startDriver(String mode, String expected) throws Exception {
        String launch = device.executeShellCommand("am start -W -n " + APPLICATION
                + "/dev.muon.runtime.MuonServiceDriverActivity --es mode " + mode);
        assertFalse("The native service driver must launch: " + launch, launch.contains("Error"));
        UiObject2 status = device.wait(Until.findObject(By.pkg(APPLICATION)
                .desc(Pattern.compile("service-driver-(ready|rejected|preference-ready|error)"))), 60000);
        assertNotNull("The UI must acquire both independent services", status);
        assertEquals(status.getText(), "service-driver-" + expected, status.getContentDescription());
        return new JSONObject(status.getText());
    }

    @Test
    public void releasesExplicitReferenceWithoutStoppingResidentWork() throws Exception {
        device.executeShellCommand("am force-stop " + APPLICATION);
        startDriver();
        try (Socket alpha = connect("alpha"); Socket beta = connect("beta")) {
            JSONObject before = challenge(alpha);
            challenge(beta);
            UiObject2 release = device.findObject(By.desc("release-beta"));
            assertNotNull("The driver must expose explicit reference release", release);
            release.click();
            assertEquals(-1, beta.getInputStream().read());
            UiObject2 released = device.wait(Until.findObject(By.desc("service-driver-released")), 60000);
            assertNotNull(released);
            JSONObject result = new JSONObject(released.getText());
            assertTrue(result.getBoolean("firstRelease"));
            assertFalse(result.getBoolean("secondRelease"));
            assertEquals(before.getString("generation"), challenge(alpha).getString("generation"));
        } finally { device.executeShellCommand("am force-stop " + APPLICATION); }
    }

    @Test
    public void recoversAcquisitionReservationWhenAndroidRejectsForegroundService() throws Exception {
        device.executeShellCommand("am force-stop " + APPLICATION);
        try {
            JSONObject result = startDriver("reject", "rejected");
            assertEquals("failed", result.getString("state"));
            assertEquals(0, result.getInt("clientReferences"));
            assertTrue(result.getString("error").contains("SecurityException"));
            assertTrue(result.getString("acquireError").contains("SecurityException"));
        } finally { device.executeShellCommand("am force-stop " + APPLICATION); }
    }

    private Socket connect(String id) throws Exception {
        Socket socket = new Socket();
        socket.connect(new InetSocketAddress("127.0.0.1", id.equals("alpha") ? 49171 : 49172), 60000);
        socket.setSoTimeout(60000);
        return socket;
    }

    private JSONObject preferenceState(String action) throws Exception {
        UiObject2 status = device.wait(Until.findObject(By.desc("service-driver-" + action)), 60000);
        assertNotNull("Preference operation must complete: " + action, status);
        return new JSONObject(status.getText());
    }

    @Test public void preservesOtherOwnersWhileTurningAutostartOffAndRestoresOffAfterRestart() throws Exception {
        startDriver("preferences", "preference-ready");
        try (Socket alpha = connect("alpha")) {
            JSONObject before = challenge(alpha);
            device.findObject(By.desc("foreign-watch")).click();
            preferenceState("foreign-rejected");
            device.findObject(By.desc("autostart-off")).click();
            JSONObject off = preferenceState("autostart-off");
            assertFalse(off.getBoolean("autostart"));
            assertFalse(off.getBoolean("resident"));
            assertEquals(2, off.getInt("clientReferences"));
            device.findObject(By.desc("release-first")).click();
            assertEquals(1, preferenceState("release-first").getInt("clientReferences"));
            assertEquals(before.getString("generation"), challenge(alpha).getString("generation"));
            device.findObject(By.desc("release-second")).click();
            preferenceState("release-second");
            assertEquals("Watching must not retain native work", -1, alpha.getInputStream().read());
            assertNotNull(device.wait(Until.findObject(By.desc("service-watch-stopped")), 60000));
        }
        // This cold restart deliberately stops both processes to exercise disk
        // restoration, unlike the separate UI-only death test above.
        device.executeShellCommand("am force-stop " + APPLICATION);
        JSONObject restored = startDriver("inspect-preferences", "preference-ready");
        assertEquals("stopped", restored.getString("state"));
        assertFalse(restored.getBoolean("autostart"));
        assertEquals(0, restored.getInt("clientReferences"));
        device.findObject(By.desc("autostart-on")).click();
        preferenceState("autostart-on");
        assertNotNull(device.wait(Until.findObject(By.desc("service-watch-running")), 60000));
        try (Socket alpha = connect("alpha")) { challenge(alpha); }
    }

    @Test public void observesAndroidComponentStopWithoutRestartingTheResidentService() throws Exception {
        startDriver("preferences", "preference-ready");
        try (Socket alpha = connect("alpha")) {
            challenge(alpha);
            device.findObject(By.desc("os-stop")).click();
            assertEquals(-1, alpha.getInputStream().read());
            UiObject2 failure = device.wait(Until.findObject(By.desc("service-watch-failed")), 60000);
            assertNotNull("Watch must observe the OS-required stop", failure);
            JSONObject state = new JSONObject(failure.getText());
            assertTrue(state.getBoolean("autostart"));
            assertTrue(state.getBoolean("resident"));
            assertEquals(0, state.getInt("clientReferences"));
            assertTrue(state.getString("error").contains("destroyed"));
        }
    }

    @Test public void endsOldPageConnectionsWhenTheHostProcessDies() throws Exception {
        JSONObject original = startDriver("connection", "ready");
        int oldProcess;
        try (Socket alpha = connect("alpha")) {
            oldProcess = challenge(alpha).getInt("pid");
            device.executeShellCommand("run-as " + APPLICATION + " kill -9 " + oldProcess);
            assertEquals(-1, alpha.getInputStream().read());
        }
        assertNotNull(device.wait(Until.findObject(By.desc("service-driver-disconnected")), 60000));
        device.findObject(By.desc("old-acquire")).click();
        UiObject2 old = device.wait(Until.findObject(By.desc(Pattern.compile(
                "service-driver-old-acquire-(accepted|rejected)"))), 60000);
        assertNotNull(old);
        assertEquals("A dead page connection cannot join a new host", "service-driver-old-acquire-rejected",
                old.getContentDescription());
        device.findObject(By.desc("new-acquire")).click();
        JSONObject next = preferenceState("new-acquire-accepted");
        // The host restarts its identity counters. The old connection must not
        // forward the old reference even when those values happen to match.
        assertEquals(original.getString("refId"), next.getString("refId"));
        assertEquals(original.getString("generation"), next.getString("generation"));
        try (Socket alpha = connect("alpha")) {
            assertTrue(oldProcess != challenge(alpha).getInt("pid"));
            device.findObject(By.desc("old-release")).click();
            assertNotNull(device.wait(Until.findObject(By.desc("service-driver-old-release-rejected")), 60000));
            challenge(alpha);
        }
    }

    @Test public void changesAutostartAndClosesAPendingWatchThroughThePublicPageApi() throws Exception {
        device.executeShellCommand("am start -W -n " + APPLICATION + "/dev.muon.runtime.MuonAppActivity");
        pageReady();
        try (Socket alpha = connect("alpha")) {
            challenge(alpha);
            device.findObject(By.text("Disable service autostart")).click();
            assertNotNull(device.wait(Until.findObject(By.text("page-autostart-off")), 60000));
            UiObject2 changed = device.wait(Until.findObject(By.text(Pattern.compile(
                    "page-watch .*\\\"autostart\\\":false.*"))), 60000);
            assertNotNull("The public watcher must observe the committed preference", changed);
            JSONObject state = new JSONObject(changed.getText().substring("page-watch ".length()));
            assertEquals(1, state.getInt("clientReferences"));
            assertEquals("running", state.getString("state"));
            device.findObject(By.text("Close state watch")).click();
            assertNotNull(device.wait(Until.findObject(By.text("page-watch-closed")), 60000));
            device.findObject(By.text("Release page")).click();
            assertNotNull(device.wait(Until.findObject(By.text("Page released")), 60000));
            assertEquals(-1, alpha.getInputStream().read());
        }
    }

    @Test public void preservesResidentWorkWhenAutostartPersistenceFails() throws Exception {
        startDriver("preferences", "preference-ready");
        device.executeShellCommand("run-as " + APPLICATION + " mkdir -p files/service-autostart");
        device.executeShellCommand("run-as " + APPLICATION + " chmod 500 files/service-autostart");
        try (Socket alpha = connect("alpha")) {
            JSONObject before = challenge(alpha);
            device.findObject(By.desc("autostart-off")).click();
            JSONObject unchanged = preferenceState("save-failed");
            assertTrue(unchanged.getBoolean("autostart"));
            assertTrue(unchanged.getBoolean("resident"));
            assertEquals(2, unchanged.getInt("clientReferences"));
            assertFalse(unchanged.getString("saveError").isEmpty());
            assertEquals(before.getString("generation"), challenge(alpha).getString("generation"));
        } finally {
            device.executeShellCommand("run-as " + APPLICATION + " chmod 700 files/service-autostart");
        }
    }

    private JSONObject challenge(Socket socket) throws Exception {
        String nonce = UUID.randomUUID().toString();
        socket.getOutputStream().write((nonce + "\n").getBytes(StandardCharsets.UTF_8));
        String line = new BufferedReader(new InputStreamReader(socket.getInputStream(),
                StandardCharsets.UTF_8)).readLine();
        assertNotNull("Native service must process this new request", line);
        JSONObject result = new JSONObject(line);
        assertEquals(nonce, result.getString("nonce"));
        return result;
    }

    @Test
    public void releasesPageOwnersOnNavigationAndAcquiresAgainInTheSameUiProcess() throws Exception {
        device.executeShellCommand("am force-stop " + APPLICATION);
        try {
            String launch = device.executeShellCommand("am start -W -n " + APPLICATION
                    + "/dev.muon.runtime.MuonAppActivity");
            assertFalse("The WebView page fixture must launch: " + launch, launch.contains("Error"));
            JSONObject before = pageReady();
            try (Socket alpha = connect("alpha"); Socket beta = connect("beta")) {
                JSONObject first = challenge(alpha);
                challenge(beta);
                device.findObject(By.text("Release page")).click();
                assertNotNull(device.wait(Until.findObject(By.text("Page released")), 60000));
                assertEquals("Navigation must release the old document owner", -1, beta.getInputStream().read());
                assertEquals(first.getString("generation"), challenge(alpha).getString("generation"));
                device.findObject(By.text("Acquire services")).click();
                JSONObject next = pageReady();
                assertEquals(before.getInt("uiPid"), next.getInt("uiPid"));
                assertEquals(before.getString("alphaGeneration"), next.getString("alphaGeneration"));
                assertTrue(next.getLong("betaGeneration") > before.getLong("betaGeneration"));
                assertEquals(1, next.getInt("betaClientReferences"));
            }
        } finally { device.executeShellCommand("am force-stop " + APPLICATION); }
    }

    private JSONObject pageReady() throws Exception {
        UiObject2 status = device.wait(Until.findObject(By.text(Pattern.compile("page-(ready|error) .*"))), 60000);
        assertNotNull("The page must acquire services through the public API", status);
        String text = status.getText();
        assertTrue(text, text.startsWith("page-ready "));
        return new JSONObject(text.substring("page-ready ".length()));
    }

    @Test
    public void keepsResidentNativeWorkAfterUiProcessDeathAndReconnects() throws Exception {
        device.executeShellCommand("am force-stop " + APPLICATION);
        JSONObject before = startDriver();
        int uiPid = before.getInt("uiPid");
        try (Socket alpha = connect("alpha"); Socket beta = connect("beta")) {
            JSONObject first = challenge(alpha);
            assertEquals("Native configuration must use standard UTF-8", "e697a5e69cace8aa9ef09f9880", first.getString("labelHex"));
            assertFalse("Service must run outside the UI process", uiPid == first.getInt("pid"));
            challenge(beta);
            // Kill only the UI process. Force-stop would terminate every component.
            device.pressHome();
            assertTrue(device.wait(Until.gone(By.pkg(APPLICATION)), 60000));
            device.executeShellCommand("run-as " + APPLICATION + " kill -9 " + uiPid);
            assertEquals("A read-only observer socket must not retain beta", -1,
                    beta.getInputStream().read());
            JSONObject after = challenge(alpha);
            assertEquals(first.getInt("pid"), after.getInt("pid"));
            assertEquals(first.getLong("generation"), after.getLong("generation"));
            assertTrue(after.getLong("count") > first.getLong("count"));
            JSONObject reconnected = startDriver();
            assertFalse(uiPid == reconnected.getInt("uiPid"));
            assertEquals(first.getLong("generation"), reconnected.getLong("alphaGeneration"));
            assertEquals(1, reconnected.getInt("alphaClientReferences"));
            assertEquals(1, reconnected.getInt("betaClientReferences"));
            assertTrue(reconnected.getLong("betaGeneration") > before.getLong("betaGeneration"));
        } finally {
            device.executeShellCommand("am force-stop " + APPLICATION);
        }
    }
}

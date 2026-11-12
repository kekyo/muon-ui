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
import org.junit.runner.RunWith;

/** Uses no service Binder or lifetime reference in the observed application. */
@RunWith(AndroidJUnit4.class)
public final class NativeServiceProcessTest {
    private static final String APPLICATION = "dev.muon.servicefixture";
    private final UiDevice device = UiDevice.getInstance(InstrumentationRegistry.getInstrumentation());

    private JSONObject startDriver() throws Exception {
        return startDriver("normal", "ready");
    }

    private JSONObject startDriver(String mode, String expected) throws Exception {
        String launch = device.executeShellCommand("am start -W -n " + APPLICATION
                + "/dev.muon.runtime.MuonServiceDriverActivity --es mode " + mode);
        assertFalse("The native service driver must launch: " + launch, launch.contains("Error"));
        UiObject2 status = device.wait(Until.findObject(By.pkg(APPLICATION)
                .desc(Pattern.compile("service-driver-(ready|rejected|error)"))), 60000);
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

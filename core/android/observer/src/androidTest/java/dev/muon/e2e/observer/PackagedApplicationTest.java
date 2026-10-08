// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

package dev.muon.e2e.observer;

import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import androidx.test.uiautomator.By;
import androidx.test.uiautomator.UiDevice;
import androidx.test.uiautomator.UiObject2;
import androidx.test.uiautomator.Until;
import java.io.ByteArrayOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.regex.Pattern;
import org.junit.Test;
import org.junit.runner.RunWith;

/** Observes only the public UI of the independently installed consumer APK. */
@RunWith(AndroidJUnit4.class)
public final class PackagedApplicationTest {
    private static final String APPLICATION_ID = "dev.muon.e2e.publicconsumer";
    private final UiDevice device = UiDevice.getInstance(InstrumentationRegistry.getInstrumentation());

    private UiObject2 requireText(String text) throws Exception {
        UiObject2 view = device.wait(Until.findObject(By.pkg(APPLICATION_ID)
                .text(Pattern.compile(Pattern.quote(text) + "|failed:.*"))), 60000);
        if (view == null) {
            ByteArrayOutputStream hierarchy = new ByteArrayOutputStream();
            device.dumpWindowHierarchy(hierarchy);
            assertNotNull("Missing " + text + ": " + hierarchy.toString(StandardCharsets.UTF_8.name()), view);
        }
        assertEquals(text, view.getText());
        return view;
    }

    @Test
    public void operatesPackagedApplication() throws Exception {
        if ("catalog-mismatch".equals(InstrumentationRegistry.getArguments().getString("mode"))) {
            UiObject2 failure = device.wait(Until.findObject(By.pkg(APPLICATION_ID)
                    .textContains("Plugin catalog mismatch")), 60000);
            assertNotNull("A mismatched producer catalog must prevent startup", failure);
            assertTrue(failure.getText().contains("consumer_alpha"));
            return;
        }
        requireText("ready:android-webview:package-consumer");
        requireText("Policy: blocked");
        String version = InstrumentationRegistry.getArguments().getString("version");
        if (version != null) requireText("Version: " + version);
        if ("true".equals(InstrumentationRegistry.getArguments().getString("plugin"))) {
            requireText("Plugin: 7:consumer-registry:blocked");
        }
        if ("verify".equals(InstrumentationRegistry.getArguments().getString("mode"))) {
            requireText("Stored: saved-on-device");
            return;
        }
        requireText("Stored: empty");
        requireText("Save note").click();
        requireText("Stored: saved-on-device");
        UiObject2 generation = device.wait(Until.findObject(By.pkg(APPLICATION_ID).res("generation")), 60000);
        assertNotNull("The page generation must be observable", generation);
        int previous = Integer.parseInt(generation.getText().replace("Page loads: ", ""));
        requireText("Reload page").click();
        requireText("Page loads: " + (previous + 1));
        requireText("ready:android-webview:package-consumer");
        requireText("Stored: saved-on-device");
    }
}

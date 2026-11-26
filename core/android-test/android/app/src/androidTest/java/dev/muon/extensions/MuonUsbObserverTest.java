/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
package dev.muon.extensions;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;
import android.content.Context;
import android.net.Uri;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import dev.muon.runtime.MuonUsbDeviceProvider;
import java.util.concurrent.Semaphore;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import org.json.JSONArray;
import org.junit.Test;
import org.junit.runner.RunWith;

/** USB enumeration and lifetime of registry notifications as seen by an application extension. */
@RunWith(AndroidJUnit4.class)
public final class MuonUsbObserverTest {
    @Test public void observersCanReadAttachmentsAndCloseIndependently() throws Exception {
        Context app = InstrumentationRegistry.getInstrumentation().getTargetContext();
        Uri changes = Uri.parse("content://" + app.getPackageName() + ".muon.usb");
        AtomicInteger calls = new AtomicInteger();
        Semaphore first = new Semaphore(0), other = new Semaphore(0);
        try (AutoCloseable second = MuonUsbDeviceProvider.observe(app, other::release)) {
            AutoCloseable observer = MuonUsbDeviceProvider.observe(app, () -> {
                calls.incrementAndGet();
                first.release();
            });
            try {
                JSONArray attached = new JSONArray(MuonUsbDeviceProvider.list(app));
                for (int i = 0; i < attached.length(); i++)
                    assertTrue(attached.getJSONObject(i).getString("connectionId").length() > 0);
                app.getContentResolver().notifyChange(changes, null, 0);
                assertTrue(first.tryAcquire(5, TimeUnit.SECONDS));
                assertTrue(other.tryAcquire(5, TimeUnit.SECONDS));
            } finally { observer.close(); }
            observer.close();
            int before = calls.get();
            app.getContentResolver().notifyChange(changes, null, 0);
            assertTrue(other.tryAcquire(5, TimeUnit.SECONDS));
            InstrumentationRegistry.getInstrumentation().waitForIdleSync();
            assertEquals(before, calls.get());
        }
    }
}

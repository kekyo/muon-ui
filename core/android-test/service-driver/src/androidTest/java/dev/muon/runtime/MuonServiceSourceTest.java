package dev.muon.runtime;

import static org.junit.Assert.*;
import android.os.Handler;
import android.os.Looper;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import java.io.ByteArrayInputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.Test;
import org.junit.runner.RunWith;

/** Bundle storage boundaries, independent of QuickJS's JavaScript parser. */
@RunWith(AndroidJUnit4.class)
public final class MuonServiceSourceTest {
    @Test public void preservesUnicodeAndEmbeddedNul() throws Exception {
        String source = "source 日本語😀\u0000tail";
        assertEquals(source, MuonServiceSource.decode(new ByteArrayInputStream(source.getBytes(StandardCharsets.UTF_8))));
    }

    @Test public void rejectsMalformedAndOversizedSources() throws Exception {
        for (byte[] input : new byte[][]{new byte[]{(byte)0xc0, (byte)0x80}, new byte[8 * 1024 * 1024 + 1]}) {
            try { MuonServiceSource.decode(new ByteArrayInputStream(input)); fail("Invalid source accepted"); }
            catch (IOException expected) { assertNotNull(expected.getMessage()); }
        }
    }

    @Test public void reportsReadFailureOnTheOwnerLooper() throws Exception {
        CountDownLatch done = new CountDownLatch(1);
        AtomicReference<String> failure = new AtomicReference<>();
        AtomicReference<Looper> delivered = new AtomicReference<>();
        MuonServiceSource.read(InstrumentationRegistry.getInstrumentation().getTargetContext(),
                "muon/services/missing.js", new Handler(Looper.getMainLooper()), (source, error) -> {
                    delivered.set(Looper.myLooper()); failure.set(error); done.countDown();
                });
        assertTrue("Asynchronous source completion", done.await(30, TimeUnit.SECONDS));
        assertNotNull(failure.get());
        assertSame(Looper.getMainLooper(), delivered.get());
    }
}

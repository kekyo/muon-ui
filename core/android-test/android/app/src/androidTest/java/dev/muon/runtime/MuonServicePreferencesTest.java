/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
package dev.muon.runtime;

import static org.junit.Assert.*;

import android.os.Looper;
import android.system.Os;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import java.io.File;
import java.io.FileOutputStream;
import java.util.UUID;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.After;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;

/** Real app-private storage; completions return to the service owner Looper. */
@RunWith(AndroidJUnit4.class)
public final class MuonServicePreferencesTest {
    private File directory;

    @Before public void prepare() {
        directory = new File(InstrumentationRegistry.getInstrumentation().getTargetContext()
                .getCacheDir(), "service-preferences-" + UUID.randomUUID());
        assertTrue(directory.mkdir());
    }

    @After public void cleanup() throws Exception {
        Os.chmod(directory.getAbsolutePath(), 0700);
        File[] children = directory.listFiles();
        if (children != null) for (File child : children) assertTrue(child.delete());
        assertTrue(directory.delete());
    }

    private static final class Result<T> {
        T value;
        String error;
    }

    private interface Operation<T> { void begin(MuonServicePreferences.Completion<T> completion); }

    private <T> Result<T> await(Operation<T> operation) throws Exception {
        CountDownLatch done = new CountDownLatch(1);
        Result<T> result = new Result<>();
        AtomicReference<Throwable> failure = new AtomicReference<>();
        operation.begin((value, error) -> {
            try {
                assertEquals(Looper.getMainLooper(), Looper.myLooper());
                result.value = value;
                result.error = error;
            } catch (Throwable assertion) { failure.set(assertion); }
            finally { done.countDown(); }
        });
        assertTrue("Storage must settle its completion", done.await(60, TimeUnit.SECONDS));
        if (failure.get() != null) throw new AssertionError(failure.get());
        return result;
    }

    @Test public void restoresExplicitOffSeparatelyFromAnAbsentPreference() throws Exception {
        MuonServicePreferences store = new MuonServicePreferences(directory);
        Result<int[]> missing = await(done -> store.load(new String[]{"alpha", "beta"}, done));
        assertNull(missing.error);
        assertArrayEquals(new int[]{-1, -1}, missing.value);
        assertNull(this.<Void>await(done -> store.save("alpha", false, done)).error);
        MuonServicePreferences reopened = new MuonServicePreferences(directory);
        Result<int[]> loaded = await(done -> reopened.load(new String[]{"alpha", "beta"}, done));
        assertNull(loaded.error);
        assertArrayEquals(new int[]{0, -1}, loaded.value);
    }

    @Test public void retainsIndependentOverridesDuringConcurrentSaves() throws Exception {
        MuonServicePreferences store = new MuonServicePreferences(directory);
        CountDownLatch complete = new CountDownLatch(2);
        AtomicReference<String> failure = new AtomicReference<>();
        MuonServicePreferences.Completion<Void> done = (value, error) -> {
            if (error != null) failure.set(error);
            complete.countDown();
        };
        store.save("alpha", false, done);
        store.save("beta", true, done);
        assertTrue(complete.await(60, TimeUnit.SECONDS));
        assertNull(failure.get());
        Result<int[]> loaded = await(callback -> store.load(new String[]{"alpha", "beta"}, callback));
        assertNull(loaded.error);
        assertArrayEquals(new int[]{0, 1}, loaded.value);
    }

    @Test public void preservesCommittedPreferenceWhenStorageRejectsTheNextWrite() throws Exception {
        MuonServicePreferences store = new MuonServicePreferences(directory);
        assertNull(this.<Void>await(done -> store.save("alpha", true, done)).error);
        Os.chmod(directory.getAbsolutePath(), 0500);
        try {
            assertNotNull(this.<Void>await(done -> store.save("alpha", false, done)).error);
        } finally { Os.chmod(directory.getAbsolutePath(), 0700); }
        Result<int[]> loaded = await(done -> store.load(new String[]{"alpha"}, done));
        assertNull(loaded.error);
        assertArrayEquals(new int[]{1}, loaded.value);
        assertEquals("Failed writes must leave no staging files", 1, directory.list().length);
    }

    @Test public void rejectsCorruptPreferencesInsteadOfRestoringManifestDefaults() throws Exception {
        MuonServicePreferences store = new MuonServicePreferences(directory);
        assertNull(this.<Void>await(done -> store.save("alpha", true, done)).error);
        File[] files = directory.listFiles();
        assertEquals(1, files.length);
        try (FileOutputStream output = new FileOutputStream(files[0])) { output.write(new byte[]{'1', '0'}); }
        Result<int[]> loaded = await(done -> store.load(new String[]{"alpha"}, done));
        assertNotNull(loaded.error);
        assertNull(loaded.value);
    }
}

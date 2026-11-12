/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
package dev.muon.runtime;

import android.os.Handler;
import android.os.Looper;
import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;
import android.util.Log;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.util.concurrent.Callable;
import java.util.concurrent.ForkJoinPool;

/** One atomic override per service, independent of manifest defaults. */
final class MuonServicePreferences {
    interface Completion<T> { void complete(T value, String error); }
    private final File directory;
    private final Handler owner = new Handler(Looper.getMainLooper());

    MuonServicePreferences(File directory) { this.directory = directory; }

    private <T> void execute(Callable<T> work, Completion<T> completion) {
        try {
            // Android has no cardio regular-file async adapter. Reuse the JVM
            // pool for blocking storage calls, returning only on the owner queue.
            ForkJoinPool.commonPool().execute(() -> {
                try {
                    T value = work.call();
                    owner.post(() -> completion.complete(value, null));
                } catch (Exception error) {
                    owner.post(() -> completion.complete(null, error.toString()));
                }
            });
        } catch (RuntimeException error) {
            owner.post(() -> completion.complete(null, error.toString()));
        }
    }

    private File preference(String id) {
        if (!id.matches("[A-Za-z][A-Za-z0-9_.-]{0,63}"))
            throw new IllegalArgumentException("Invalid service preference identity");
        return new File(directory, id);
    }

    /** Loads before writes begin: -1 absent, 0 explicit OFF, 1 explicit ON. */
    void load(String[] ids, Completion<int[]> completion) {
        String[] names = ids.clone();
        execute(() -> {
            File[] stale = directory.listFiles();
            if (stale != null) for (File file : stale) {
                if (file.getName().startsWith(".save-") && file.getName().endsWith(".tmp") && !file.delete())
                    Log.w("MuonNativeService", "Cannot remove interrupted service preference write");
            }
            int[] values = new int[names.length];
            for (int index = 0; index < names.length; ++index) {
                File file = preference(names[index]);
                try {
                    if (!OsConstants.S_ISREG(Os.stat(file.getAbsolutePath()).st_mode))
                        throw new IOException("Service preference is not a regular file: " + names[index]);
                } catch (ErrnoException error) {
                    if (error.errno != OsConstants.ENOENT) throw error;
                    values[index] = -1;
                    continue;
                }
                try (FileInputStream input = new FileInputStream(file)) {
                    int value = input.read();
                    if ((value != '0' && value != '1') || input.read() != -1)
                        throw new IOException("Invalid service preference: " + names[index]);
                    values[index] = value - '0';
                }
            }
            return values;
        }, completion);
    }

    /** Completes successfully only after the replacement has committed. */
    void save(String id, boolean enabled, Completion<Void> completion) {
        execute(() -> {
            File destination = preference(id);
            if (!directory.isDirectory() && !directory.mkdirs() && !directory.isDirectory())
                throw new IOException("Cannot create service preference directory");
            File temporary = File.createTempFile(".save-", ".tmp", directory);
            boolean committed = false;
            try {
                try (FileOutputStream output = new FileOutputStream(temporary)) {
                    output.write(enabled ? '1' : '0');
                    output.getFD().sync();
                }
                // Rename is the commit point. No fallible work follows it: an
                // acknowledged failure must always preserve the old preference.
                Os.rename(temporary.getAbsolutePath(), destination.getAbsolutePath());
                committed = true;
            } finally {
                if (!committed && !temporary.delete())
                    Log.w("MuonNativeService", "Cannot remove failed service preference write");
            }
            return null;
        }, completion);
    }
}

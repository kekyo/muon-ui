/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

package dev.muon.prototype;

import android.app.Service;
import android.content.Intent;
import android.os.IBinder;
import android.os.ParcelFileDescriptor;
import android.os.Process;

import androidx.annotation.NonNull;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;

/** Hosts independent QuickJS runtimes in an application-private process. */
public final class MuonJavaScriptRuntimeService extends Service {
    private static final int MAXIMUM_RUNTIME_COUNT = 16;

    static {
        System.loadLibrary("muon_javascript_runtime");
    }

    private final IMuonJavaScriptRuntimeService.Stub binder =
            new IMuonJavaScriptRuntimeService.Stub() {
                @Override
                public ParcelFileDescriptor createRuntime(String runtimeId) {
                    if (runtimeId == null || runtimeId.isEmpty()) {
                        throw new IllegalArgumentException(
                                "The JavaScript runtime id must not be empty.");
                    }
                    if (nativeGetRuntimeCount() >= MAXIMUM_RUNTIME_COUNT) {
                        throw new IllegalStateException(
                                "The Android JavaScript runtime limit was reached.");
                    }

                    ParcelFileDescriptor[] sockets;
                    try {
                        sockets = ParcelFileDescriptor.createSocketPair();
                    } catch (IOException error) {
                        throw new IllegalStateException(
                                "Unable to create the JavaScript protocol socket.",
                                error);
                    }
                    int runtimeFileDescriptor = sockets[1].detachFd();
                    try {
                        nativeStart(
                                runtimeId,
                                runtimeFileDescriptor,
                                requireRuntimeSource(),
                                requireBackendSource(),
                                requireFilesystemRoot().getAbsolutePath());
                        return sockets[0];
                    } catch (RuntimeException | Error error) {
                        closeQuietly(sockets[0]);
                        closeQuietly(ParcelFileDescriptor.adoptFd(runtimeFileDescriptor));
                        throw error;
                    }
                }

                @Override
                public String getEngineVersion() {
                    return nativeGetEngineVersion();
                }

                @Override
                public int getProcessId() {
                    return Process.myPid();
                }

                @Override
                public int getRuntimeCount() {
                    return nativeGetRuntimeCount();
                }

                @Override
                public void shutdownRuntime(String runtimeId) {
                    if (runtimeId != null) {
                        nativeShutdown(runtimeId);
                    }
                }

                @Override
                public void shutdownAll() {
                    nativeShutdownAll();
                }

                @Override
                public void terminateForTest() {
                    if (!BuildConfig.DEBUG) {
                        throw new SecurityException(
                                "JavaScript Service termination is available only in debug builds.");
                    }
                    Process.killProcess(Process.myPid());
                }
            };

    private volatile String runtimeSource;
    private volatile String backendSource;
    private volatile RuntimeException sourceFailure;
    private volatile File filesystemRoot;

    @Override
    public void onCreate() {
        super.onCreate();
        try {
            runtimeSource = readAsset("muon-javascript/runtime.js");
            backendSource = readAsset("muon-javascript/backend.mjs");
            File root = new File(getFilesDir(), "javascript-runtime");
            if (!root.isDirectory() && !root.mkdirs()) {
                throw new IOException(
                        "Unable to create the JavaScript filesystem root.");
            }
            filesystemRoot = root;
        } catch (IOException error) {
            sourceFailure = new IllegalStateException(
                    "Unable to load the packaged JavaScript runtime.",
                    error);
        }
    }

    @Override
    public IBinder onBind(Intent intent) {
        return binder;
    }

    @Override
    public void onDestroy() {
        nativeShutdownAll();
        super.onDestroy();
    }

    @NonNull
    private String requireRuntimeSource() {
        requireSourcesReady();
        return runtimeSource;
    }

    @NonNull
    private String requireBackendSource() {
        requireSourcesReady();
        return backendSource;
    }

    @NonNull
    private File requireFilesystemRoot() {
        requireSourcesReady();
        return filesystemRoot;
    }

    private void requireSourcesReady() {
        RuntimeException failure = sourceFailure;
        if (failure != null) {
            throw failure;
        }
        if (runtimeSource == null || backendSource == null || filesystemRoot == null) {
            throw new IllegalStateException(
                    "The packaged JavaScript runtime is not ready.");
        }
    }

    @NonNull
    private String readAsset(@NonNull String path) throws IOException {
        try (InputStream input = getAssets().open(path)) {
            ByteArrayOutputStream output = new ByteArrayOutputStream();
            byte[] buffer = new byte[8192];
            while (true) {
                int read = input.read(buffer);
                if (read < 0) {
                    break;
                }
                output.write(buffer, 0, read);
            }
            return output.toString(StandardCharsets.UTF_8.name());
        }
    }

    private static void closeQuietly(ParcelFileDescriptor descriptor) {
        try {
            descriptor.close();
        } catch (IOException ignored) {
            // Preserve the startup failure that required this cleanup.
        }
    }

    private static native void nativeStart(
            String runtimeId,
            int fileDescriptor,
            String runtimeSource,
            String backendSource,
            String filesystemRoot);

    private static native void nativeShutdown(String runtimeId);

    private static native void nativeShutdownAll();

    private static native int nativeGetRuntimeCount();

    private static native String nativeGetEngineVersion();
}

/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

package dev.muon.runtime;

import android.app.Activity;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.net.Uri;
import android.os.IBinder;
import android.os.ParcelFileDescriptor;
import android.os.RemoteException;
import android.webkit.WebView;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.webkit.JavaScriptReplyProxy;
import androidx.webkit.WebMessageCompat;
import androidx.webkit.WebViewCompat;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.EOFException;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.LinkedHashMap;
import java.util.Map;

/** Connects the trusted WebView facade to private-process QuickJS runtimes. */
public final class MuonJavaScriptRuntimeBridge
        implements WebViewCompat.WebMessageListener, AutoCloseable {
    private static final int PROTOCOL_VERSION = 1;
    private static final int MAXIMUM_FRAME_LENGTH = 16 * 1024 * 1024;
    private static final String SERVICE_PROTOCOL = "muon-js/1";

    private final Activity activity;
    private final Map<String, RuntimeConnection> runtimes =
            new LinkedHashMap<>();
    private final ArrayDeque<String> pendingMessages = new ArrayDeque<>();
    private final ServiceConnection serviceConnection = new ServiceConnection() {
        @Override
        public void onServiceConnected(ComponentName name, IBinder binder) {
            if (closed) {
                return;
            }
            service = IMuonJavaScriptRuntimeService.Stub.asInterface(binder);
            while (!pendingMessages.isEmpty()) {
                handleMessage(pendingMessages.removeFirst());
            }
        }

        @Override
        public void onServiceDisconnected(ComponentName name) {
            service = null;
            closeRuntimes("QuickJS Service process disconnected");
        }

        @Override
        public void onBindingDied(ComponentName name) {
            service = null;
            closeRuntimes("QuickJS Service binding died");
            if (!closed) {
                activity.unbindService(this);
                bound = false;
                bindService();
            }
        }

        @Override
        public void onNullBinding(ComponentName name) {
            service = null;
            closeRuntimes("QuickJS Service returned a null binding");
        }
    };

    @Nullable private IMuonJavaScriptRuntimeService service;
    @Nullable private JavaScriptReplyProxy replyProxy;
    private int nextRuntimeId = 1;
    private boolean bound;
    private boolean closed;

    /**
     * Creates and binds the private JavaScript runtime Service.
     *
     * @param activity Activity owning the trusted WebView.
     */
    public MuonJavaScriptRuntimeBridge(@NonNull Activity activity) {
        this.activity = activity;
        bindService();
    }

    private void bindService() {
        if (bound || closed) {
            return;
        }
        Intent intent = new Intent(activity, MuonJavaScriptRuntimeService.class);
        bound = activity.bindService(
                intent,
                serviceConnection,
                Context.BIND_AUTO_CREATE);
        if (!bound) {
            throw new IllegalStateException(
                    "Unable to bind the Android JavaScript runtime Service.");
        }
    }

    @Override
    public void onPostMessage(
            @NonNull WebView view,
            @NonNull WebMessageCompat message,
            @NonNull Uri sourceOrigin,
            boolean isMainFrame,
            @NonNull JavaScriptReplyProxy currentReplyProxy) {
        if (closed
                || !isMainFrame
                || !MuonActivity.TRUSTED_ORIGIN.equals(sourceOrigin.toString())
                || message.getType() != WebMessageCompat.TYPE_STRING) {
            return;
        }
        String source = message.getData();
        if (source == null) {
            return;
        }
        replyProxy = currentReplyProxy;
        handleMessage(source);
    }

    private void handleMessage(@NonNull String source) {
        JSONObject message;
        try {
            message = new JSONObject(source);
            if (message.optInt("version", -1) != PROTOCOL_VERSION) {
                return;
            }
            String type = message.getString("type");
            if ("dispose".equals(type)) {
                disposeRuntimes();
                return;
            }
            if (service == null) {
                pendingMessages.addLast(source);
                return;
            }
            if ("create".equals(type)) {
                createRuntime(message.getInt("requestId"));
            } else if ("runtimeMessage".equals(type)) {
                sendRuntimeMessage(
                        message.getString("runtimeId"),
                        message.getJSONObject("message"));
            }
        } catch (JSONException | IOException | RemoteException error) {
            int requestId = -1;
            try {
                requestId = new JSONObject(source).optInt("requestId", -1);
            } catch (JSONException ignored) {
                // The original parsing error is the useful diagnostic.
            }
            if (requestId > 0) {
                postCreateFailed(requestId, diagnostic(error));
            }
        }
    }

    private void createRuntime(int requestId)
            throws RemoteException, IOException {
        IMuonJavaScriptRuntimeService currentService = service;
        if (currentService == null) {
            postCreateFailed(
                    requestId,
                    "QuickJS Service process is not connected");
            return;
        }
        String runtimeId = "runtime-" + nextRuntimeId;
        nextRuntimeId += 1;
        ParcelFileDescriptor descriptor = currentService.createRuntime(runtimeId);
        RuntimeConnection connection =
                new RuntimeConnection(runtimeId, requestId, descriptor);
        runtimes.put(runtimeId, connection);
        connection.start();
    }

    private void sendRuntimeMessage(
            @NonNull String runtimeId,
            @NonNull JSONObject message) throws IOException {
        RuntimeConnection connection = runtimes.get(runtimeId);
        if (connection == null) {
            postRuntimeClosed(runtimeId, "JavaScript runtime is unavailable");
            return;
        }
        connection.write(message.toString());
    }

    private void onHandshake(
            @NonNull RuntimeConnection connection,
            @NonNull JSONObject handshake) {
        if (closed || runtimes.get(connection.runtimeId) != connection) {
            connection.close();
            return;
        }
        try {
            if (!"handshake".equals(handshake.getString("kind"))
                    || !SERVICE_PROTOCOL.equals(handshake.getString("protocol"))) {
                throw new JSONException("Unexpected JavaScript Service handshake");
            }
            JSONObject engine = handshake.getJSONObject("engine");
            if (!"quickjs".equals(engine.getString("name"))
                    || engine.getString("version").isEmpty()) {
                throw new JSONException("Unexpected JavaScript engine identity");
            }
            JSONArray capabilities = handshake.getJSONArray("capabilities");
            if (capabilities.length() == 0) {
                throw new JSONException("JavaScript Service capabilities are empty");
            }
            postEnvelope(new JSONObject()
                    .put("version", PROTOCOL_VERSION)
                    .put("type", "created")
                    .put("requestId", connection.requestId)
                    .put("runtimeId", connection.runtimeId)
                    .put("engine", engine));
        } catch (JSONException error) {
            runtimes.remove(connection.runtimeId);
            connection.close();
            postCreateFailed(connection.requestId, diagnostic(error));
        }
    }

    private void onRuntimeMessage(
            @NonNull RuntimeConnection connection,
            @NonNull JSONObject message) {
        if (closed || runtimes.get(connection.runtimeId) != connection) {
            return;
        }
        try {
            postEnvelope(new JSONObject()
                    .put("version", PROTOCOL_VERSION)
                    .put("type", "runtimeMessage")
                    .put("runtimeId", connection.runtimeId)
                    .put("message", message));
        } catch (JSONException error) {
            onRuntimeClosed(connection, diagnostic(error));
        }
    }

    private void onRuntimeClosed(
            @NonNull RuntimeConnection connection,
            @NonNull String diagnostic) {
        if (runtimes.get(connection.runtimeId) != connection) {
            return;
        }
        runtimes.remove(connection.runtimeId);
        connection.close();
        postRuntimeClosed(connection.runtimeId, diagnostic);
    }

    private void postCreateFailed(int requestId, @NonNull String diagnostic) {
        try {
            postEnvelope(new JSONObject()
                    .put("version", PROTOCOL_VERSION)
                    .put("type", "createFailed")
                    .put("requestId", requestId)
                    .put("error", diagnostic));
        } catch (JSONException ignored) {
            // All values above have native JSON representations.
        }
    }

    private void postRuntimeClosed(
            @NonNull String runtimeId,
            @NonNull String diagnostic) {
        try {
            postEnvelope(new JSONObject()
                    .put("version", PROTOCOL_VERSION)
                    .put("type", "runtimeClosed")
                    .put("runtimeId", runtimeId)
                    .put("error", diagnostic));
        } catch (JSONException ignored) {
            // All values above have native JSON representations.
        }
    }

    private void postEnvelope(@NonNull JSONObject envelope) {
        JavaScriptReplyProxy currentReplyProxy = replyProxy;
        if (!closed && currentReplyProxy != null) {
            currentReplyProxy.postMessage(envelope.toString());
        }
    }

    private void closeRuntimes(@NonNull String diagnostic) {
        RuntimeConnection[] active =
                runtimes.values().toArray(new RuntimeConnection[0]);
        runtimes.clear();
        for (RuntimeConnection connection : active) {
            connection.close();
            postRuntimeClosed(connection.runtimeId, diagnostic);
        }
    }

    private void disposeRuntimes() {
        IMuonJavaScriptRuntimeService currentService = service;
        if (currentService != null) {
            try {
                currentService.shutdownAll();
            } catch (RemoteException ignored) {
                // Closing the sockets below is the fallback shutdown signal.
            }
        }
        RuntimeConnection[] active =
                runtimes.values().toArray(new RuntimeConnection[0]);
        runtimes.clear();
        for (RuntimeConnection connection : active) {
            connection.close();
        }
        pendingMessages.clear();
    }

    private static String diagnostic(@NonNull Exception error) {
        String message = error.getMessage();
        return message == null ? error.getClass().getSimpleName() : message;
    }

    @Override
    public void close() {
        if (closed) {
            return;
        }
        disposeRuntimes();
        closed = true;
        replyProxy = null;
        service = null;
        if (bound) {
            activity.unbindService(serviceConnection);
            bound = false;
        }
    }

    private final class RuntimeConnection implements AutoCloseable {
        private final String runtimeId;
        private final int requestId;
        private final ParcelFileDescriptor descriptor;
        private final DataInputStream input;
        private final DataOutputStream output;
        private boolean connectionClosed;

        private RuntimeConnection(
                @NonNull String runtimeId,
                int requestId,
                @NonNull ParcelFileDescriptor descriptor) throws IOException {
            this.runtimeId = runtimeId;
            this.requestId = requestId;
            this.descriptor = descriptor;
            input = new DataInputStream(
                    new FileInputStream(descriptor.getFileDescriptor()));
            output = new DataOutputStream(
                    new FileOutputStream(descriptor.getFileDescriptor()));
        }

        private void start() {
            Thread reader = new Thread(this::readLoop, "muon-quickjs-" + runtimeId);
            reader.start();
        }

        private void readLoop() {
            String failure = "JavaScript runtime disconnected";
            try {
                JSONObject handshake = new JSONObject(readFrame());
                activity.runOnUiThread(() -> onHandshake(this, handshake));
                while (!isConnectionClosed()) {
                    JSONObject message = new JSONObject(readFrame());
                    activity.runOnUiThread(() -> onRuntimeMessage(this, message));
                }
                return;
            } catch (EOFException error) {
                failure = "JavaScript runtime closed its protocol socket";
            } catch (IOException | JSONException error) {
                failure = diagnostic(error);
            }
            String currentFailure = failure;
            activity.runOnUiThread(() -> onRuntimeClosed(this, currentFailure));
        }

        @NonNull
        private String readFrame() throws IOException {
            int length = input.readInt();
            if (length <= 0 || length > MAXIMUM_FRAME_LENGTH) {
                throw new IOException("Invalid JavaScript runtime frame length");
            }
            byte[] contents = new byte[length];
            input.readFully(contents);
            return new String(contents, StandardCharsets.UTF_8);
        }

        private synchronized void write(@NonNull String message)
                throws IOException {
            if (connectionClosed) {
                throw new IOException("JavaScript runtime socket is closed");
            }
            byte[] contents = message.getBytes(StandardCharsets.UTF_8);
            if (contents.length == 0 || contents.length > MAXIMUM_FRAME_LENGTH) {
                throw new IOException("Invalid JavaScript runtime frame length");
            }
            output.writeInt(contents.length);
            output.write(contents);
            output.flush();
        }

        private synchronized boolean isConnectionClosed() {
            return connectionClosed;
        }

        @Override
        public synchronized void close() {
            if (connectionClosed) {
                return;
            }
            connectionClosed = true;
            try {
                descriptor.close();
            } catch (IOException ignored) {
                // The runtime is already disconnected.
            }
        }
    }
}

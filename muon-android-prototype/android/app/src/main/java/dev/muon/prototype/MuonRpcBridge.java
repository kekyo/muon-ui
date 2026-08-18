/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

package dev.muon.prototype;

import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.webkit.WebView;

import androidx.annotation.NonNull;
import androidx.webkit.JavaScriptReplyProxy;
import androidx.webkit.WebMessageCompat;
import androidx.webkit.WebViewCompat;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.nio.ByteBuffer;
import java.util.Arrays;
import java.util.HashMap;
import java.util.Map;

/** Adapts trusted WebView messages to the CEF-independent native RPC host. */
final class MuonRpcBridge implements WebViewCompat.WebMessageListener, AutoCloseable {
    private static final int PROTOCOL_VERSION = 1;
    private static final int BINARY_HEADER_LENGTH = 16;
    private static final long DELAY_MILLISECONDS = 5000;

    private static final class PendingBinaryCall {
        final int callId;
        final String capabilityId;
        final String functionPath;
        final int byteLength;

        PendingBinaryCall(
                int callId,
                @NonNull String capabilityId,
                @NonNull String functionPath,
                int byteLength) {
            this.callId = callId;
            this.capabilityId = capabilityId;
            this.functionPath = functionPath;
            this.byteLength = byteLength;
        }
    }

    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private final Map<Integer, PendingBinaryCall> pendingBinaryCalls = new HashMap<>();
    private final Map<Integer, Runnable> delayedCompletions = new HashMap<>();
    private long nativeHandle;
    private JavaScriptReplyProxy replyProxy;

    MuonRpcBridge() {
        nativeHandle = nativeCreateHost(this);
        if (nativeHandle == 0) {
            throw new IllegalStateException("Could not create the native muon RPC host");
        }
    }

    @Override
    public void onPostMessage(
            @NonNull WebView view,
            @NonNull WebMessageCompat message,
            @NonNull Uri sourceOrigin,
            boolean isMainFrame,
            @NonNull JavaScriptReplyProxy currentReplyProxy) {
        if (!isMainFrame || !MuonActivity.TRUSTED_ORIGIN.equals(sourceOrigin.toString())) {
            return;
        }
        replyProxy = currentReplyProxy;
        if (message.getType() == WebMessageCompat.TYPE_STRING) {
            String data = message.getData();
            if (data != null) {
                handleTextMessage(data);
            }
        } else if (message.getType() == WebMessageCompat.TYPE_ARRAY_BUFFER) {
            handleBinaryMessage(message.getArrayBuffer());
        }
    }

    private void handleTextMessage(@NonNull String data) {
        try {
            JSONObject message = new JSONObject(data);
            if (message.optInt("version", 0) != PROTOCOL_VERSION) {
                return;
            }
            String type = message.optString("type", "");
            if ("call".equals(type)) {
                handleCallMessage(message);
            } else if ("cancel".equals(type)) {
                int callId = message.optInt("callId", 0);
                if (callId > 0 && nativeHandle != 0) {
                    pendingBinaryCalls.remove(callId);
                    nativeCancelCall(nativeHandle, callId);
                }
            } else if ("release".equals(type) && nativeHandle != 0) {
                pendingBinaryCalls.clear();
                nativeReleaseContext(nativeHandle);
            }
        } catch (JSONException ignored) {
            // Malformed transport messages do not cross the native RPC boundary.
        }
    }

    private void handleCallMessage(@NonNull JSONObject message) throws JSONException {
        int callId = message.optInt("callId", 0);
        String capabilityId = message.optString("capabilityId", "");
        String functionPath = message.optString("functionPath", "");
        JSONArray arguments = message.optJSONArray("arguments");
        if (callId <= 0 || functionPath.isEmpty() || arguments == null) {
            return;
        }
        if (arguments.length() == 0) {
            nativeDispatchCall(nativeHandle, callId, capabilityId, functionPath, null);
            return;
        }
        if (arguments.length() == 1) {
            JSONObject descriptor = arguments.optJSONObject(0);
            if (descriptor != null
                    && "binary".equals(descriptor.optString("type", ""))
                    && descriptor.optInt("attachment", -1) == 0
                    && descriptor.optInt("byteLength", -1) >= 0) {
                PendingBinaryCall pending = new PendingBinaryCall(
                        callId,
                        capabilityId,
                        functionPath,
                        descriptor.getInt("byteLength"));
                if (pendingBinaryCalls.putIfAbsent(callId, pending) == null) {
                    return;
                }
            }
        }
        sendProtocolError(callId, "Unsupported Android prototype RPC arguments");
    }

    private void handleBinaryMessage(@NonNull byte[] frame) {
        if (frame.length < BINARY_HEADER_LENGTH
                || frame[0] != 'M'
                || frame[1] != 'R'
                || frame[2] != 'P'
                || frame[3] != 'C'
                || frame[4] != PROTOCOL_VERSION
                || frame[5] != 1
                || frame[6] != 0
                || frame[7] != 0) {
            return;
        }
        ByteBuffer header = ByteBuffer.wrap(frame, 0, BINARY_HEADER_LENGTH);
        int callId = header.getInt(8);
        int attachment = header.getInt(12);
        PendingBinaryCall pending = pendingBinaryCalls.get(callId);
        int payloadLength = frame.length - BINARY_HEADER_LENGTH;
        if (callId <= 0
                || attachment != 0
                || pending == null
                || pending.byteLength != payloadLength) {
            return;
        }
        pendingBinaryCalls.remove(callId);
        byte[] payload = Arrays.copyOfRange(frame, BINARY_HEADER_LENGTH, frame.length);
        nativeDispatchCall(
                nativeHandle,
                pending.callId,
                pending.capabilityId,
                pending.functionPath,
                payload);
    }

    private void sendProtocolError(int callId, @NonNull String diagnostic) {
        if (replyProxy == null || callId <= 0) {
            return;
        }
        try {
            JSONObject result = new JSONObject();
            result.put("version", PROTOCOL_VERSION);
            result.put("type", "result");
            result.put("callId", callId);
            result.put("success", false);
            result.put("error", diagnostic);
            replyProxy.postMessage(result.toString());
        } catch (JSONException ignored) {
            throw new IllegalStateException("Could not encode an RPC protocol error", ignored);
        }
    }

    @SuppressWarnings("unused")
    private void onNativeTextResult(@NonNull String message) {
        if (nativeHandle != 0 && replyProxy != null) {
            replyProxy.postMessage(message);
        }
    }

    @SuppressWarnings("unused")
    private void onNativeBinaryResult(@NonNull byte[] frame) {
        if (nativeHandle != 0 && replyProxy != null) {
            replyProxy.postMessage(frame);
        }
    }

    @SuppressWarnings("unused")
    private void scheduleNativeDelay(int callId) {
        Runnable completion = () -> {
            delayedCompletions.remove(callId);
            if (nativeHandle != 0) {
                nativeCompleteDelayedCall(nativeHandle, callId);
            }
        };
        Runnable previous = delayedCompletions.put(callId, completion);
        if (previous != null) {
            mainHandler.removeCallbacks(previous);
        }
        mainHandler.postDelayed(completion, DELAY_MILLISECONDS);
    }

    @SuppressWarnings("unused")
    private void cancelNativeDelay(int callId) {
        Runnable completion = delayedCompletions.remove(callId);
        if (completion != null) {
            mainHandler.removeCallbacks(completion);
        }
    }

    @SuppressWarnings("unused")
    private void cancelAllNativeDelays() {
        for (Runnable completion : delayedCompletions.values()) {
            mainHandler.removeCallbacks(completion);
        }
        delayedCompletions.clear();
    }

    int getNativePendingCallCount() {
        return nativeHandle == 0 ? 0 : nativeGetPendingCallCount(nativeHandle);
    }

    @Override
    public void close() {
        pendingBinaryCalls.clear();
        cancelAllNativeDelays();
        if (nativeHandle != 0) {
            nativeReleaseContext(nativeHandle);
            nativeDestroyHost(nativeHandle);
            nativeHandle = 0;
        }
        replyProxy = null;
    }

    private static native long nativeCreateHost(@NonNull MuonRpcBridge bridge);

    private static native void nativeDispatchCall(
            long handle,
            int callId,
            @NonNull String capabilityId,
            @NonNull String functionPath,
            byte[] binaryArgument);

    private static native void nativeCancelCall(long handle, int callId);

    private static native void nativeCompleteDelayedCall(long handle, int callId);

    private static native void nativeReleaseContext(long handle);

    private static native int nativeGetPendingCallCount(long handle);

    private static native void nativeDestroyHost(long handle);
}

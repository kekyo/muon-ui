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
import androidx.annotation.Nullable;
import androidx.webkit.JavaScriptReplyProxy;
import androidx.webkit.WebMessageCompat;
import androidx.webkit.WebViewCompat;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.nio.ByteBuffer;
import java.util.Arrays;
import java.util.HashMap;
import java.util.Iterator;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;

/** Adapts trusted WebView messages to the CEF-independent native RPC host. */
final class MuonRpcBridge implements WebViewCompat.WebMessageListener, AutoCloseable {
    static {
        System.loadLibrary("muon_android_rpc");
    }

    private static final int PROTOCOL_VERSION = 1;
    private static final int BINARY_HEADER_LENGTH = 16;
    private static final int MAXIMUM_ATTACHMENT_COUNT = 1024;
    private static final int MAXIMUM_ATTACHMENT_BYTES = 64 * 1024 * 1024;
    private static final long DELAY_MILLISECONDS = 5000;
    private static final int RESULT_VOID = 0;
    private static final int RESULT_STRING = 1;
    private static final int RESULT_UNSIGNED_INTEGER = 2;
    private static final int RESULT_BOOLEAN = 3;
    private static final int RESULT_BINARY = 4;
    private static final int NATIVE_ARGUMENT_NULL = 0;
    private static final int NATIVE_ARGUMENT_BOOLEAN = 1;
    private static final int NATIVE_ARGUMENT_NUMBER = 2;
    private static final int NATIVE_ARGUMENT_STRING = 3;
    private static final int NATIVE_ARGUMENT_BINARY = 4;
    private static final int NATIVE_ARGUMENT_FUNCTION = 5;
    private static final int NATIVE_ARGUMENT_JSON = 6;
    private static final int NATIVE_FUNCTION_RENDERER_SOURCE = 1;
    private static final int NATIVE_FUNCTION_PLUGIN_PROXY = 2;
    private static final int NATIVE_CALL_PLUGIN = 0;
    private static final int NATIVE_CALL_PLUGIN_PROXY = 1;
    private static final Handler PROCESS_MAIN_HANDLER = new Handler(Looper.getMainLooper());
    private static final LinkedBlockingQueue<String> NATIVE_RUNTIME_STOP_EVENTS =
            new LinkedBlockingQueue<>();

    /** Flat root argument representation decoded again against native metadata. */
    private static final class NativeArgument {
        final int kind;
        final boolean booleanValue;
        final double numberValue;
        @Nullable final String stringValue;
        final int attachment;
        final int functionKind;
        final int rendererContextId;
        final int functionId;
        final int proxyId;
        @Nullable final String leaseToken;

        NativeArgument(
                int kind,
                boolean booleanValue,
                double numberValue,
                @Nullable String stringValue,
                int attachment) {
            this(kind, booleanValue, numberValue, stringValue, attachment,
                    0, 0, 0, 0, null);
        }

        NativeArgument(
                int kind,
                boolean booleanValue,
                double numberValue,
                @Nullable String stringValue,
                int attachment,
                int functionKind,
                int rendererContextId,
                int functionId,
                int proxyId,
                @Nullable String leaseToken) {
            this.kind = kind;
            this.booleanValue = booleanValue;
            this.numberValue = numberValue;
            this.stringValue = stringValue;
            this.attachment = attachment;
            this.functionKind = functionKind;
            this.rendererContextId = rendererContextId;
            this.functionId = functionId;
            this.proxyId = proxyId;
            this.leaseToken = leaseToken;
        }
    }

    private static final class PendingBinaryCall {
        final int callId;
        final int callKind;
        final String capabilityId;
        final String functionPath;
        final int proxyId;
        final String proxyLeaseToken;
        final String argumentsJson;
        final NativeArgument[] nativeArguments;
        final int[] byteLengths;
        final byte[][] attachments;
        int receivedAttachmentCount;

        PendingBinaryCall(
                int callId,
                int callKind,
                @NonNull String capabilityId,
                @NonNull String functionPath,
                int proxyId,
                @NonNull String proxyLeaseToken,
                @NonNull String argumentsJson,
                @NonNull NativeArgument[] nativeArguments,
                @NonNull int[] byteLengths) {
            this.callId = callId;
            this.callKind = callKind;
            this.capabilityId = capabilityId;
            this.functionPath = functionPath;
            this.proxyId = proxyId;
            this.proxyLeaseToken = proxyLeaseToken;
            this.argumentsJson = argumentsJson;
            this.nativeArguments = nativeArguments;
            this.byteLengths = byteLengths;
            attachments = new byte[byteLengths.length][];
        }
    }

    private static final class PendingBinaryRendererResult {
        final int callId;
        final NativeArgument[] nativeResult;
        final int[] byteLengths;
        final byte[][] attachments;
        int receivedAttachmentCount;

        PendingBinaryRendererResult(
                int callId,
                @NonNull NativeArgument[] nativeResult,
                @NonNull int[] byteLengths) {
            this.callId = callId;
            this.nativeResult = nativeResult;
            this.byteLengths = byteLengths;
            attachments = new byte[byteLengths.length][];
        }
    }

    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private final MuonActivity activity;
    private final Map<Integer, PendingBinaryCall> pendingBinaryCalls = new HashMap<>();
    private final Map<Integer, PendingBinaryRendererResult> pendingBinaryRendererResults =
            new HashMap<>();
    private final Map<Integer, Runnable> delayedCompletions = new HashMap<>();
    private final LinkedBlockingQueue<String> nativeRuntimeProbeResults =
            new LinkedBlockingQueue<>();
    private final LinkedBlockingQueue<String> nativeRuntimeProbeSettlements =
            new LinkedBlockingQueue<>();
    private final MuonAndroidPlatformService platformService;
    private String rendererMetadataJson = "";
    private long nativeHandle;
    private JavaScriptReplyProxy replyProxy;
    private boolean nativeHostReady;
    private boolean nativeHostStartupFailed;

    MuonRpcBridge(
            @NonNull MuonActivity activity,
            @NonNull WebView webView) {
        this.activity = activity;
        platformService = new MuonAndroidPlatformService(activity, webView);
        try {
            nativeHandle = nativeCreateHost(this);
        } catch (RuntimeException error) {
            platformService.close();
            throw error;
        }
        if (nativeHandle == 0) {
            platformService.close();
            throw new IllegalStateException("Could not create the native muon RPC host");
        }
        if (nativeIsHostReady(nativeHandle)) {
            rendererMetadataJson = nativeGetRendererMetadata(nativeHandle);
            nativeHostReady = true;
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
            } else if ("renderer-function-result".equals(type)) {
                handleRendererFunctionResult(message);
            } else if ("plugin-proxy-release".equals(type)) {
                int proxyId = message.optInt("proxyId", 0);
                String leaseToken = message.optString("leaseToken", "");
                if (proxyId > 0 && !leaseToken.isEmpty() && nativeHandle != 0) {
                    nativeReleasePluginProxy(nativeHandle, proxyId, leaseToken);
                }
            } else if ("cancel".equals(type)) {
                int callId = message.optInt("callId", 0);
                if (callId > 0 && nativeHandle != 0) {
                    pendingBinaryCalls.remove(callId);
                    nativeCancelCall(nativeHandle, callId);
                }
            } else if ("release".equals(type) && nativeHandle != 0) {
                pendingBinaryCalls.clear();
                pendingBinaryRendererResults.clear();
                nativeReleaseContext(nativeHandle);
            }
        } catch (JSONException ignored) {
            // Malformed transport messages do not cross the native RPC boundary.
        }
    }

    private void handleCallMessage(@NonNull JSONObject message) throws JSONException {
        int callId = message.optInt("callId", 0);
        boolean pluginProxy = "plugin-proxy".equals(message.optString("callKind", ""));
        int callKind = pluginProxy ? NATIVE_CALL_PLUGIN_PROXY : NATIVE_CALL_PLUGIN;
        String capabilityId = message.optString("capabilityId", "");
        String functionPath = message.optString("functionPath", "");
        int proxyId = message.optInt("proxyId", 0);
        String proxyLeaseToken = message.optString("leaseToken", "");
        JSONArray arguments = message.optJSONArray("arguments");
        if (callId <= 0
                || arguments == null
                || (pluginProxy
                    ? proxyId <= 0 || proxyLeaseToken.isEmpty()
                    : functionPath.isEmpty())) {
            return;
        }
        Map<Integer, Integer> binaryDescriptors = new HashMap<>();
        try {
            collectBinaryDescriptors(arguments, binaryDescriptors);
        } catch (JSONException error) {
            sendProtocolError(callId, error.getMessage() == null
                    ? "Invalid Android RPC binary descriptor"
                    : error.getMessage());
            return;
        }
        if (binaryDescriptors.size() > MAXIMUM_ATTACHMENT_COUNT) {
            sendProtocolError(callId, "Too many Android RPC binary attachments");
            return;
        }
        int[] byteLengths = new int[binaryDescriptors.size()];
        long totalBytes = 0;
        for (int attachment = 0; attachment < byteLengths.length; attachment += 1) {
            Integer byteLength = binaryDescriptors.get(attachment);
            if (byteLength == null) {
                sendProtocolError(callId, "Android RPC binary attachments are not contiguous");
                return;
            }
            byteLengths[attachment] = byteLength;
            totalBytes += byteLength;
        }
        if (totalBytes > MAXIMUM_ATTACHMENT_BYTES) {
            sendProtocolError(callId, "Android RPC binary attachments exceed the size limit");
            return;
        }

        String argumentsJson = arguments.toString();
        if (byteLengths.length == 0) {
            nativeDispatchCall(
                    nativeHandle,
                    callId,
                    callKind,
                    capabilityId,
                    functionPath,
                    proxyId,
                    proxyLeaseToken,
                    argumentsJson,
                    createNativeArguments(arguments),
                    new byte[0][]);
            return;
        }
        PendingBinaryCall pending = new PendingBinaryCall(
                callId,
                callKind,
                capabilityId,
                functionPath,
                proxyId,
                proxyLeaseToken,
                argumentsJson,
                createNativeArguments(arguments),
                byteLengths);
        if (pendingBinaryCalls.putIfAbsent(callId, pending) != null) {
            sendProtocolError(callId, "Duplicate Android RPC call id");
        }
    }

    private void handleRendererFunctionResult(@NonNull JSONObject message)
            throws JSONException {
        int callId = message.optInt("callId", 0);
        if (callId <= 0 || nativeHandle == 0 || !message.has("success")) {
            return;
        }
        boolean success = message.getBoolean("success");
        if (!success) {
            nativeCompleteRendererFunctionCall(
                    nativeHandle,
                    callId,
                    false,
                    message.optString("error", "Renderer function failed"),
                    new NativeArgument[0],
                    new byte[0][]);
            return;
        }

        Object value = message.has("value") ? message.get("value") : JSONObject.NULL;
        JSONArray encodedResult = new JSONArray();
        encodedResult.put(value);
        Map<Integer, Integer> binaryDescriptors = new HashMap<>();
        collectBinaryDescriptors(encodedResult, binaryDescriptors);
        int[] byteLengths = createBinaryAttachmentLengths(binaryDescriptors);
        if (byteLengths == null) {
            nativeCompleteRendererFunctionCall(
                    nativeHandle,
                    callId,
                    false,
                    "Invalid renderer function binary descriptors",
                    new NativeArgument[0],
                    new byte[0][]);
            return;
        }
        NativeArgument[] nativeResult = createNativeArguments(encodedResult);
        if (byteLengths.length == 0) {
            nativeCompleteRendererFunctionCall(
                    nativeHandle,
                    callId,
                    true,
                    null,
                    nativeResult,
                    new byte[0][]);
            return;
        }
        PendingBinaryRendererResult pending = new PendingBinaryRendererResult(
                callId, nativeResult, byteLengths);
        if (pendingBinaryRendererResults.putIfAbsent(callId, pending) != null) {
            nativeCompleteRendererFunctionCall(
                    nativeHandle,
                    callId,
                    false,
                    "Duplicate renderer function result",
                    new NativeArgument[0],
                    new byte[0][]);
        }
    }

    private void handleBinaryMessage(@NonNull byte[] frame) {
        if (frame.length < BINARY_HEADER_LENGTH
                || frame[0] != 'M'
                || frame[1] != 'R'
                || frame[2] != 'P'
                || frame[3] != 'C'
                || frame[4] != PROTOCOL_VERSION
                || (frame[5] != 1 && frame[5] != 4)
                || frame[6] != 0
                || frame[7] != 0) {
            return;
        }
        ByteBuffer header = ByteBuffer.wrap(frame, 0, BINARY_HEADER_LENGTH);
        int callId = header.getInt(8);
        int attachment = header.getInt(12);
        if (frame[5] == 4) {
            handleRendererResultBinaryFrame(frame, callId, attachment);
            return;
        }
        PendingBinaryCall pending = pendingBinaryCalls.get(callId);
        int payloadLength = frame.length - BINARY_HEADER_LENGTH;
        if (callId <= 0
                || pending == null
                || attachment < 0
                || attachment >= pending.byteLengths.length
                || pending.byteLengths[attachment] != payloadLength
                || pending.attachments[attachment] != null) {
            return;
        }
        byte[] payload = Arrays.copyOfRange(frame, BINARY_HEADER_LENGTH, frame.length);
        pending.attachments[attachment] = payload;
        pending.receivedAttachmentCount += 1;
        if (pending.receivedAttachmentCount != pending.attachments.length) {
            return;
        }
        pendingBinaryCalls.remove(callId);
        nativeDispatchCall(
                nativeHandle,
                pending.callId,
                pending.callKind,
                pending.capabilityId,
                pending.functionPath,
                pending.proxyId,
                pending.proxyLeaseToken,
                pending.argumentsJson,
                pending.nativeArguments,
                pending.attachments);
    }

    private void handleRendererResultBinaryFrame(
            @NonNull byte[] frame,
            int callId,
            int attachment) {
        PendingBinaryRendererResult pending = pendingBinaryRendererResults.get(callId);
        int payloadLength = frame.length - BINARY_HEADER_LENGTH;
        if (callId <= 0
                || pending == null
                || attachment < 0
                || attachment >= pending.byteLengths.length
                || pending.byteLengths[attachment] != payloadLength
                || pending.attachments[attachment] != null) {
            return;
        }
        pending.attachments[attachment] =
                Arrays.copyOfRange(frame, BINARY_HEADER_LENGTH, frame.length);
        pending.receivedAttachmentCount += 1;
        if (pending.receivedAttachmentCount != pending.attachments.length) {
            return;
        }
        pendingBinaryRendererResults.remove(callId);
        nativeCompleteRendererFunctionCall(
                nativeHandle,
                pending.callId,
                true,
                null,
                pending.nativeResult,
                pending.attachments);
    }

    @NonNull private static NativeArgument[] createNativeArguments(
            @NonNull JSONArray arguments) throws JSONException {
        NativeArgument[] result = new NativeArgument[arguments.length()];
        for (int index = 0; index < arguments.length(); index += 1) {
            Object value = arguments.get(index);
            if (value == JSONObject.NULL) {
                result[index] = new NativeArgument(
                        NATIVE_ARGUMENT_NULL, false, 0, null, -1);
            } else if (value instanceof Boolean) {
                result[index] = new NativeArgument(
                        NATIVE_ARGUMENT_BOOLEAN, (Boolean) value, 0, null, -1);
            } else if (value instanceof Number) {
                result[index] = new NativeArgument(
                        NATIVE_ARGUMENT_NUMBER,
                        false,
                        ((Number) value).doubleValue(),
                        null,
                        -1);
            } else if (value instanceof String) {
                result[index] = new NativeArgument(
                        NATIVE_ARGUMENT_STRING, false, 0, (String) value, -1);
            } else if (value instanceof JSONObject
                    && "binary".equals(((JSONObject) value).optString("type", ""))) {
                result[index] = new NativeArgument(
                        NATIVE_ARGUMENT_BINARY,
                        false,
                        0,
                        null,
                        ((JSONObject) value).getInt("attachment"));
            } else if (value instanceof JSONObject
                    && "function".equals(((JSONObject) value).optString("type", ""))) {
                JSONObject function = (JSONObject) value;
                String functionKind = function.optString("kind", "");
                if ("renderer-source".equals(functionKind)) {
                    result[index] = new NativeArgument(
                            NATIVE_ARGUMENT_FUNCTION,
                            false,
                            0,
                            null,
                            -1,
                            NATIVE_FUNCTION_RENDERER_SOURCE,
                            function.getInt("rendererContextId"),
                            function.getInt("functionId"),
                            0,
                            null);
                } else if ("plugin-proxy".equals(functionKind)) {
                    result[index] = new NativeArgument(
                            NATIVE_ARGUMENT_FUNCTION,
                            false,
                            0,
                            null,
                            -1,
                            NATIVE_FUNCTION_PLUGIN_PROXY,
                            0,
                            0,
                            function.getInt("proxyId"),
                            function.getString("leaseToken"));
                } else {
                    throw new JSONException("Invalid Android RPC function descriptor");
                }
            } else {
                result[index] = new NativeArgument(
                        NATIVE_ARGUMENT_JSON, false, 0, value.toString(), -1);
            }
        }
        return result;
    }

    @Nullable private static int[] createBinaryAttachmentLengths(
            @NonNull Map<Integer, Integer> binaryDescriptors) {
        if (binaryDescriptors.size() > MAXIMUM_ATTACHMENT_COUNT) {
            return null;
        }
        int[] byteLengths = new int[binaryDescriptors.size()];
        long totalBytes = 0;
        for (int attachment = 0; attachment < byteLengths.length; attachment += 1) {
            Integer byteLength = binaryDescriptors.get(attachment);
            if (byteLength == null) {
                return null;
            }
            byteLengths[attachment] = byteLength;
            totalBytes += byteLength;
        }
        return totalBytes > MAXIMUM_ATTACHMENT_BYTES ? null : byteLengths;
    }

    private static void collectBinaryDescriptors(
            @NonNull Object value,
            @NonNull Map<Integer, Integer> descriptors) throws JSONException {
        if (value instanceof JSONArray) {
            JSONArray array = (JSONArray) value;
            for (int index = 0; index < array.length(); index += 1) {
                collectBinaryDescriptors(array.get(index), descriptors);
            }
            return;
        }
        if (!(value instanceof JSONObject)) {
            return;
        }

        JSONObject object = (JSONObject) value;
        if ("binary".equals(object.optString("type", ""))) {
            Object attachmentValue = object.opt("attachment");
            Object byteLengthValue = object.opt("byteLength");
            if (object.length() != 3
                    || !(attachmentValue instanceof Number)
                    || !(byteLengthValue instanceof Number)) {
                throw new JSONException("Invalid Android RPC binary descriptor");
            }
            long attachment = ((Number) attachmentValue).longValue();
            long byteLength = ((Number) byteLengthValue).longValue();
            if (((Number) attachmentValue).doubleValue() != attachment
                    || ((Number) byteLengthValue).doubleValue() != byteLength
                    || attachment < 0
                    || attachment > Integer.MAX_VALUE
                    || byteLength < 0
                    || byteLength > MAXIMUM_ATTACHMENT_BYTES
                    || descriptors.putIfAbsent((int) attachment, (int) byteLength) != null) {
                throw new JSONException("Invalid Android RPC binary descriptor");
            }
            return;
        }

        Iterator<String> keys = object.keys();
        while (keys.hasNext()) {
            collectBinaryDescriptors(object.get(keys.next()), descriptors);
        }
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

    /** Returns the short trusted-origin script installed before page code. */
    @NonNull String getDocumentStartScript() {
        if (!nativeHostReady) {
            throw new IllegalStateException("The Android native host is not ready");
        }
        return "Object.defineProperty(globalThis," +
                "'__muon_android_plugin_metadata',{" +
                "configurable:false,enumerable:false,writable:false,value:" +
                rendererMetadataJson + "});";
    }

    boolean isNativeHostReady() {
        return nativeHostReady;
    }

    @SuppressWarnings("unused")
    private void onNativeHostReady(@NonNull String metadataJson) {
        if (nativeHandle == 0 || nativeHostReady || nativeHostStartupFailed) {
            return;
        }
        rendererMetadataJson = metadataJson;
        nativeHostReady = true;
        activity.onNativeHostReady(this);
    }

    @SuppressWarnings("unused")
    private void onNativeHostStartupFailed(@NonNull String diagnostic) {
        if (nativeHandle == 0 || nativeHostReady || nativeHostStartupFailed) {
            return;
        }
        nativeHostStartupFailed = true;
        activity.onNativeHostStartupFailed(this, diagnostic);
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
    private void onNativeRuntimeProbeResult(int mask) {
        String thread = Looper.myLooper() == Looper.getMainLooper() ? "main" : "other";
        nativeRuntimeProbeResults.add(thread + ":" + mask);
    }

    @SuppressWarnings("unused")
    private void onNativeRuntimeProbeSettled(boolean delivered) {
        String thread = Looper.myLooper() == Looper.getMainLooper() ? "main" : "other";
        nativeRuntimeProbeSettlements.add(thread + ":" + delivered);
    }

    @SuppressWarnings("unused")
    private static void scheduleNativeRuntimeStopCompletion() {
        if (!PROCESS_MAIN_HANDLER.post(MuonRpcBridge::completeNativeRuntimeStop)) {
            throw new IllegalStateException("The Android main Looper is exiting");
        }
    }

    private static void completeNativeRuntimeStop() {
        nativeCompleteRuntimeStop();
        NATIVE_RUNTIME_STOP_EVENTS.add(nativeGetProcessRuntimeDiagnosticsForTest());
    }

    @SuppressWarnings("unused")
    private void invokePlatformFunction(
            int callId,
            @NonNull String functionPath,
            @NonNull String argumentsJson,
            @NonNull byte[][] attachments) {
        try {
            JSONArray arguments = new JSONArray(argumentsJson);
            platformService.invoke(
                    callId,
                    functionPath,
                    arguments,
                    attachments,
                    new MuonAndroidPlatformService.Completion() {
                        @Override
                        public void completeVoid() {
                            completePlatformCall(
                                    callId, RESULT_VOID, null, 0, false, null, null);
                        }

                        @Override
                        public void completeString(@NonNull String value) {
                            completePlatformCall(
                                    callId, RESULT_STRING, value, 0, false, null, null);
                        }

                        @Override
                        public void completeUnsignedInteger(long value) {
                            completePlatformCall(
                                    callId,
                                    RESULT_UNSIGNED_INTEGER,
                                    null,
                                    value,
                                    false,
                                    null,
                                    null);
                        }

                        @Override
                        public void completeBoolean(boolean value) {
                            completePlatformCall(
                                    callId, RESULT_BOOLEAN, null, 0, value, null, null);
                        }

                        @Override
                        public void completeBinary(@NonNull byte[] value) {
                            completePlatformCall(
                                    callId, RESULT_BINARY, null, 0, false, value, null);
                        }

                        @Override
                        public void fail(@NonNull String diagnostic) {
                            completePlatformCall(
                                    callId,
                                    RESULT_VOID,
                                    null,
                                    0,
                                    false,
                                    null,
                                    diagnostic);
                        }
                    });
        } catch (JSONException error) {
            completePlatformCall(
                    callId,
                    RESULT_VOID,
                    null,
                    0,
                    false,
                    null,
                    "Invalid Android RPC arguments");
        }
    }

    @SuppressWarnings("unused")
    private void cancelPlatformCall(int callId) {
        platformService.cancel(callId);
    }

    @SuppressWarnings("unused")
    private void cancelAllPlatformCalls() {
        platformService.cancelAll();
    }

    private void completePlatformCall(
            int callId,
            int resultKind,
            @Nullable String stringValue,
            long unsignedIntegerValue,
            boolean booleanValue,
            @Nullable byte[] binaryValue,
            @Nullable String error) {
        if (nativeHandle != 0) {
            nativeCompletePlatformCall(
                    nativeHandle,
                    callId,
                    resultKind,
                    stringValue,
                    unsignedIntegerValue,
                    booleanValue,
                    binaryValue,
                    error);
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

    boolean isFullscreenForTest() {
        return platformService.isFullscreen();
    }

    float getManagedZoomFactorForTest() {
        return platformService.getManagedZoomFactor();
    }

    int getActiveFilesystemWatchCountForTest() {
        return platformService.getActiveFilesystemWatchCount();
    }

    void startNativeRuntimeProbeForTest() {
        if (nativeHandle == 0) {
            throw new IllegalStateException("The native runtime session is closed");
        }
        nativeStartRuntimeProbe(nativeHandle);
    }

    void releaseNativeContextForTest() {
        if (nativeHandle != 0) {
            nativeReleaseContext(nativeHandle);
        }
    }

    void clearNativeRuntimeProbeEventsForTest() {
        nativeRuntimeProbeResults.clear();
        nativeRuntimeProbeSettlements.clear();
    }

    @Nullable String awaitNativeRuntimeProbeResultForTest(
            long timeout,
            @NonNull TimeUnit unit) throws InterruptedException {
        return nativeRuntimeProbeResults.poll(timeout, unit);
    }

    @Nullable String awaitNativeRuntimeProbeSettlementForTest(
            long timeout,
            @NonNull TimeUnit unit) throws InterruptedException {
        return nativeRuntimeProbeSettlements.poll(timeout, unit);
    }

    int getNativeRuntimeProbeResultCountForTest() {
        return nativeRuntimeProbeResults.size();
    }

    @NonNull String getNativeRuntimeDiagnosticsForTest() {
        if (nativeHandle == 0) {
            throw new IllegalStateException("The native runtime session is closed");
        }
        return nativeGetRuntimeDiagnostics(nativeHandle);
    }

    static void clearNativeRuntimeStopEventsForTest() {
        NATIVE_RUNTIME_STOP_EVENTS.clear();
    }

    @Nullable static String awaitNativeRuntimeStopForTest(
            long timeout,
            @NonNull TimeUnit unit) throws InterruptedException {
        return NATIVE_RUNTIME_STOP_EVENTS.poll(timeout, unit);
    }

    @Nullable static String awaitNativeRuntimeIdleForTest(
            long timeout,
            @NonNull TimeUnit unit) throws InterruptedException {
        long deadline = System.nanoTime() + unit.toNanos(timeout);
        while (true) {
            long remaining = deadline - System.nanoTime();
            if (remaining <= 0) {
                return null;
            }
            String diagnostics = getNativeProcessRuntimeDiagnosticsForTest(
                    remaining, TimeUnit.NANOSECONDS);
            if (diagnostics == null) {
                return null;
            }
            try {
                if ("idle".equals(new JSONObject(diagnostics).optString("runtimeState"))) {
                    return diagnostics;
                }
            } catch (JSONException ignored) {
                // A malformed diagnostic is not an idle-state confirmation.
            }

            remaining = deadline - System.nanoTime();
            if (remaining <= 0
                    || NATIVE_RUNTIME_STOP_EVENTS.poll(
                            remaining, TimeUnit.NANOSECONDS) == null) {
                return null;
            }
        }
    }

    private static @Nullable String getNativeProcessRuntimeDiagnosticsForTest(
            long timeout,
            @NonNull TimeUnit unit) throws InterruptedException {
        if (Looper.myLooper() == Looper.getMainLooper()) {
            return nativeGetProcessRuntimeDiagnosticsForTest();
        }
        AtomicReference<String> result = new AtomicReference<>();
        CountDownLatch completed = new CountDownLatch(1);
        if (!PROCESS_MAIN_HANDLER.post(() -> {
            result.set(nativeGetProcessRuntimeDiagnosticsForTest());
            completed.countDown();
        }) || !completed.await(timeout, unit)) {
            return null;
        }
        return result.get();
    }

    static boolean setNativeRuntimeStartupFaultForTest(
            @NonNull String fault,
            long timeout,
            @NonNull TimeUnit unit) throws InterruptedException {
        if (Looper.myLooper() == Looper.getMainLooper()) {
            return nativeSetRuntimeStartupFaultForTest(fault);
        }
        AtomicBoolean result = new AtomicBoolean();
        CountDownLatch completed = new CountDownLatch(1);
        if (!PROCESS_MAIN_HANDLER.post(() -> {
            result.set(nativeSetRuntimeStartupFaultForTest(fault));
            completed.countDown();
        }) || !completed.await(timeout, unit)) {
            return false;
        }
        return result.get();
    }

    @Override
    public void close() {
        close(false);
    }

    void close(boolean preserveRuntime) {
        pendingBinaryCalls.clear();
        pendingBinaryRendererResults.clear();
        cancelAllNativeDelays();
        if (nativeHandle != 0) {
            nativeReleaseContext(nativeHandle);
            nativeDestroyHost(nativeHandle, preserveRuntime);
            nativeHandle = 0;
        }
        platformService.close();
        replyProxy = null;
    }

    private static native long nativeCreateHost(@NonNull MuonRpcBridge bridge);

    private static native boolean nativeIsHostReady(long handle);

    @NonNull
    private static native String nativeGetRendererMetadata(long handle);

    private static native void nativeDispatchCall(
            long handle,
            int callId,
            int callKind,
            @NonNull String capabilityId,
            @NonNull String functionPath,
            int proxyId,
            @NonNull String proxyLeaseToken,
            @NonNull String argumentsJson,
            @NonNull NativeArgument[] nativeArguments,
            @NonNull byte[][] binaryArguments);

    private static native void nativeCompleteRendererFunctionCall(
            long handle,
            int callId,
            boolean success,
            @Nullable String error,
            @NonNull NativeArgument[] nativeResult,
            @NonNull byte[][] binaryArguments);

    private static native void nativeReleasePluginProxy(
            long handle,
            int proxyId,
            @NonNull String leaseToken);

    private static native void nativeCancelCall(long handle, int callId);

    private static native void nativeCompleteDelayedCall(long handle, int callId);

    private static native void nativeCompletePlatformCall(
            long handle,
            int callId,
            int resultKind,
            @Nullable String stringValue,
            long unsignedIntegerValue,
            boolean booleanValue,
            byte[] binaryValue,
            @Nullable String error);

    private static native void nativeReleaseContext(long handle);

    private static native void nativeStartRuntimeProbe(long handle);

    private static native void nativeCompleteRuntimeStop();

    @NonNull
    private static native String nativeGetProcessRuntimeDiagnosticsForTest();

    private static native boolean nativeSetRuntimeStartupFaultForTest(
            @NonNull String fault);

    @NonNull
    private static native String nativeGetRuntimeDiagnostics(long handle);

    private static native int nativeGetPendingCallCount(long handle);

    private static native void nativeDestroyHost(long handle, boolean preserveRuntime);
}

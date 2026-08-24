/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

package dev.muon.prototype;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.ConnectException;
import java.net.HttpURLConnection;
import java.net.ProtocolException;
import java.net.SocketTimeoutException;
import java.net.URL;
import java.net.URLConnection;
import java.net.UnknownHostException;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.ThreadFactory;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;

import javax.net.ssl.SSLException;

/** Executes bounded Android HTTP operations on behalf of QuickJS runtimes. */
final class MuonJavaScriptHttpClient {
    private static final int MAXIMUM_WORKER_COUNT = 4;
    private static final int MAXIMUM_QUEUED_OPERATION_COUNT = 64;
    private static final int RESPONSE_BUFFER_SIZE = 64 * 1024;
    private static final AtomicInteger NEXT_THREAD_IDENTIFIER = new AtomicInteger(1);
    private static final ConcurrentHashMap<OperationKey, Operation> OPERATIONS =
            new ConcurrentHashMap<>();
    private static final ThreadPoolExecutor EXECUTOR = createExecutor();

    private static final class OperationKey {
        private final String runtimeId;
        private final long identifier;

        private OperationKey(String runtimeId, long identifier) {
            this.runtimeId = runtimeId;
            this.identifier = identifier;
        }

        @Override
        public boolean equals(Object value) {
            if (this == value) {
                return true;
            }
            if (!(value instanceof OperationKey)) {
                return false;
            }
            OperationKey other = (OperationKey) value;
            return identifier == other.identifier && runtimeId.equals(other.runtimeId);
        }

        @Override
        public int hashCode() {
            return 31 * runtimeId.hashCode() + Long.hashCode(identifier);
        }
    }

    private static final class Operation {
        private final OperationKey key;
        private final AtomicBoolean cancelled = new AtomicBoolean(false);
        private final Object flowControl = new Object();
        private volatile HttpURLConnection connection;
        private boolean dataHandled = true;
        private boolean paused;

        private Operation(OperationKey key) {
            this.key = key;
        }

        private void cancel() {
            cancelled.set(true);
            HttpURLConnection active = connection;
            if (active != null) {
                active.disconnect();
            }
            synchronized (flowControl) {
                flowControl.notifyAll();
            }
        }

        private void prepareData() {
            synchronized (flowControl) {
                dataHandled = false;
            }
        }

        private boolean awaitDataHandled() throws InterruptedException {
            synchronized (flowControl) {
                while (!cancelled.get() && (!dataHandled || paused)) {
                    flowControl.wait();
                }
            }
            return !cancelled.get();
        }

        private void handleData(boolean pause) {
            synchronized (flowControl) {
                dataHandled = true;
                paused = pause;
                flowControl.notifyAll();
            }
        }

        private void resume() {
            synchronized (flowControl) {
                paused = false;
                flowControl.notifyAll();
            }
        }
    }

    private MuonJavaScriptHttpClient() {
    }

    private static ThreadPoolExecutor createExecutor() {
        ThreadFactory factory = task -> {
            Thread thread = new Thread(
                    task,
                    "muon-quickjs-http-" + NEXT_THREAD_IDENTIFIER.getAndIncrement());
            thread.setDaemon(true);
            return thread;
        };
        ThreadPoolExecutor executor = new ThreadPoolExecutor(
                MAXIMUM_WORKER_COUNT,
                MAXIMUM_WORKER_COUNT,
                30,
                TimeUnit.SECONDS,
                new ArrayBlockingQueue<>(MAXIMUM_QUEUED_OPERATION_COUNT),
                factory,
                new ThreadPoolExecutor.AbortPolicy());
        executor.allowCoreThreadTimeOut(true);
        return executor;
    }

    static void start(
            String runtimeId,
            long identifier,
            String method,
            String url,
            String headersJson,
            byte[] body,
            int connectTimeout,
            int readTimeout,
            String certificateAuthority) {
        Objects.requireNonNull(runtimeId, "runtimeId");
        Objects.requireNonNull(method, "method");
        Objects.requireNonNull(url, "url");
        Objects.requireNonNull(headersJson, "headersJson");
        Objects.requireNonNull(body, "body");
        OperationKey key = new OperationKey(runtimeId, identifier);
        Operation operation = new Operation(key);
        if (OPERATIONS.putIfAbsent(key, operation) != null) {
            throw new IllegalStateException("The HTTP operation identifier is already active.");
        }
        try {
            EXECUTOR.execute(() -> execute(
                    operation,
                    method,
                    url,
                    headersJson,
                    body,
                    connectTimeout,
                    readTimeout,
                    certificateAuthority));
        } catch (RejectedExecutionException error) {
            OPERATIONS.remove(key, operation);
            nativeOnHttpEvent(
                    runtimeId,
                    identifier,
                    "httpError",
                    createErrorPayload(
                            "ERR_HTTP_OPERATION_LIMIT",
                            "The Android HTTP operation limit was reached",
                            url),
                    null);
        }
    }

    static boolean cancel(String runtimeId, long identifier) {
        Operation operation = OPERATIONS.remove(new OperationKey(runtimeId, identifier));
        if (operation == null) {
            return false;
        }
        operation.cancel();
        return true;
    }

    static boolean handleData(String runtimeId, long identifier, boolean paused) {
        Operation operation = OPERATIONS.get(new OperationKey(runtimeId, identifier));
        if (operation == null) {
            return false;
        }
        operation.handleData(paused);
        return true;
    }

    static boolean resume(String runtimeId, long identifier) {
        Operation operation = OPERATIONS.get(new OperationKey(runtimeId, identifier));
        if (operation == null) {
            return false;
        }
        operation.resume();
        return true;
    }

    static void cancelRuntime(String runtimeId) {
        for (Map.Entry<OperationKey, Operation> entry : OPERATIONS.entrySet()) {
            if (entry.getKey().runtimeId.equals(runtimeId)
                    && OPERATIONS.remove(entry.getKey(), entry.getValue())) {
                entry.getValue().cancel();
            }
        }
    }

    private static void execute(
            Operation operation,
            String method,
            String url,
            String headersJson,
            byte[] body,
            int connectTimeout,
            int readTimeout,
            String certificateAuthority) {
        try {
            URLConnection opened = new URL(url).openConnection();
            if (!(opened instanceof HttpURLConnection)) {
                throw new ProtocolException("URL is not HTTP or HTTPS");
            }
            HttpURLConnection connection = (HttpURLConnection) opened;
            operation.connection = connection;
            if (operation.cancelled.get()) {
                return;
            }
            connection.setUseCaches(false);
            connection.setInstanceFollowRedirects(false);
            connection.setConnectTimeout(connectTimeout);
            connection.setReadTimeout(readTimeout);
            connection.setRequestMethod(method);

            JSONArray headers = new JSONArray(headersJson);
            boolean hasAcceptEncoding = false;
            for (int index = 0; index < headers.length(); index++) {
                JSONArray header = headers.getJSONArray(index);
                String name = header.getString(0);
                String value = header.getString(1);
                connection.addRequestProperty(name, value);
                if (name.equalsIgnoreCase("accept-encoding")) {
                    hasAcceptEncoding = true;
                }
            }
            if (!hasAcceptEncoding) {
                connection.setRequestProperty("Accept-Encoding", "identity");
            }
            if (certificateAuthority != null && !certificateAuthority.isEmpty()) {
                throw new ProtocolException(
                        "Custom certificate authorities require node:https support");
            }
            if (body.length > 0) {
                connection.setDoOutput(true);
                connection.setFixedLengthStreamingMode(body.length);
                try (OutputStream output = connection.getOutputStream()) {
                    output.write(body);
                }
            }

            int statusCode = connection.getResponseCode();
            if (operation.cancelled.get()) {
                return;
            }
            nativeOnHttpEvent(
                    operation.key.runtimeId,
                    operation.key.identifier,
                    "httpResponse",
                    createResponsePayload(connection, statusCode, url),
                    null);

            try (InputStream input = responseStream(connection, statusCode)) {
                if (input != null) {
                    byte[] buffer = new byte[RESPONSE_BUFFER_SIZE];
                    while (!operation.cancelled.get()) {
                        int count = input.read(buffer);
                        if (count < 0) {
                            break;
                        }
                        byte[] chunk = new byte[count];
                        System.arraycopy(buffer, 0, chunk, 0, count);
                        operation.prepareData();
                        nativeOnHttpEvent(
                                operation.key.runtimeId,
                                operation.key.identifier,
                                "httpData",
                                "",
                                chunk);
                        if (!operation.awaitDataHandled()) {
                            return;
                        }
                    }
                }
            }
            if (!operation.cancelled.get()) {
                nativeOnHttpEvent(
                        operation.key.runtimeId,
                        operation.key.identifier,
                        "httpEnd",
                        "",
                        null);
            }
        } catch (InterruptedException error) {
            Thread.currentThread().interrupt();
            if (!operation.cancelled.get()) {
                emitError(operation, "EINTR", error);
            }
        } catch (Throwable error) {
            if (!operation.cancelled.get()) {
                emitError(operation, errorCode(error), error);
            }
        } finally {
            HttpURLConnection connection = operation.connection;
            if (connection != null) {
                connection.disconnect();
            }
            OPERATIONS.remove(operation.key, operation);
        }
    }

    private static InputStream responseStream(
            HttpURLConnection connection,
            int statusCode) throws IOException {
        if (statusCode >= 400) {
            InputStream error = connection.getErrorStream();
            if (error != null) {
                return error;
            }
        }
        return connection.getInputStream();
    }

    private static String createResponsePayload(
            HttpURLConnection connection,
            int statusCode,
            String url) throws Exception {
        JSONObject payload = new JSONObject();
        payload.put("statusCode", statusCode);
        payload.put("statusMessage", connection.getResponseMessage());
        payload.put("url", url);
        String statusLine = connection.getHeaderField(0);
        String httpVersion = "1.1";
        if (statusLine != null && statusLine.startsWith("HTTP/")) {
            int separator = statusLine.indexOf(' ');
            if (separator > 5) {
                httpVersion = statusLine.substring(5, separator);
            }
        }
        payload.put("httpVersion", httpVersion);
        JSONArray headers = new JSONArray();
        for (Map.Entry<String, List<String>> entry
                : connection.getHeaderFields().entrySet()) {
            if (entry.getKey() == null || entry.getValue() == null) {
                continue;
            }
            for (String value : entry.getValue()) {
                headers.put(new JSONArray().put(entry.getKey()).put(value));
            }
        }
        payload.put("headers", headers);
        return payload.toString();
    }

    private static void emitError(Operation operation, String code, Throwable error) {
        String message = error.getMessage();
        if (message == null || message.isEmpty()) {
            message = error.getClass().getSimpleName();
        }
        nativeOnHttpEvent(
                operation.key.runtimeId,
                operation.key.identifier,
                "httpError",
                createErrorPayload(code, message, ""),
                null);
    }

    private static String createErrorPayload(String code, String message, String url) {
        try {
            return new JSONObject()
                    .put("code", code)
                    .put("message", message)
                    .put("syscall", "request")
                    .put("url", url)
                    .toString();
        } catch (Exception error) {
            return "{\"code\":\"EIO\",\"message\":\"HTTP operation failed\","
                    + "\"syscall\":\"request\",\"url\":\"\"}";
        }
    }

    private static String errorCode(Throwable error) {
        Throwable current = error;
        while (current != null) {
            if (current instanceof SocketTimeoutException) {
                return "ETIMEDOUT";
            }
            if (current instanceof UnknownHostException) {
                return "ENOTFOUND";
            }
            if (current instanceof ConnectException) {
                return "ECONNREFUSED";
            }
            if (current instanceof SSLException) {
                return "ERR_TLS_HANDSHAKE";
            }
            if (current instanceof ProtocolException) {
                return "ERR_HTTP_PROTOCOL";
            }
            current = current.getCause();
        }
        String message = error.getMessage();
        if (message != null && message.contains("CLEARTEXT")) {
            return "ERR_CLEARTEXT_NOT_PERMITTED";
        }
        return "EIO";
    }

    private static native void nativeOnHttpEvent(
            String runtimeId,
            long identifier,
            String type,
            String payload,
            byte[] data);
}

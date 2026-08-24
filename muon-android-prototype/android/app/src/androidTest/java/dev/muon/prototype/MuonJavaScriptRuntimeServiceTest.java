/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

package dev.muon.prototype;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.os.IBinder;
import android.os.ParcelFileDescriptor;
import android.os.Process;
import android.system.Os;
import android.system.OsConstants;
import android.system.StructPollfd;

import androidx.test.core.app.ApplicationProvider;
import androidx.test.ext.junit.runners.AndroidJUnit4;

import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.Test;
import org.junit.runner.RunWith;

import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

@RunWith(AndroidJUnit4.class)
public final class MuonJavaScriptRuntimeServiceTest {
    private static final int MAXIMUM_FRAME_LENGTH = 16 * 1024 * 1024;

    private static final class BoundService implements AutoCloseable {
        private final Context context;
        private final ServiceConnection connection;
        private final CountDownLatch disconnected;
        private final IMuonJavaScriptRuntimeService service;
        private boolean closed;

        private BoundService(
                Context context,
                ServiceConnection connection,
                CountDownLatch disconnected,
                IMuonJavaScriptRuntimeService service) {
            this.context = context;
            this.connection = connection;
            this.disconnected = disconnected;
            this.service = service;
        }

        private boolean awaitDisconnected() throws InterruptedException {
            return disconnected.await(30, TimeUnit.SECONDS);
        }

        @Override
        public void close() {
            if (!closed) {
                closed = true;
                context.unbindService(connection);
            }
        }
    }

    private static final class RuntimeSocket implements AutoCloseable {
        private final ParcelFileDescriptor descriptor;
        private final DataInputStream input;
        private final DataOutputStream output;

        private RuntimeSocket(ParcelFileDescriptor descriptor) throws Exception {
            this.descriptor = descriptor;
            input = new DataInputStream(new FileInputStream(descriptor.getFileDescriptor()));
            output = new DataOutputStream(new FileOutputStream(descriptor.getFileDescriptor()));
        }

        private JSONObject read() throws Exception {
            StructPollfd pollDescriptor = new StructPollfd();
            pollDescriptor.fd = descriptor.getFileDescriptor();
            pollDescriptor.events = (short) (OsConstants.POLLIN | OsConstants.POLLHUP);
            assertTrue(
                    "Timed out waiting for a JavaScript runtime frame",
                    Os.poll(new StructPollfd[]{pollDescriptor}, 30_000) > 0);
            int length = input.readInt();
            assertTrue(length > 0 && length <= MAXIMUM_FRAME_LENGTH);
            byte[] contents = new byte[length];
            input.readFully(contents);
            return new JSONObject(new String(contents, StandardCharsets.UTF_8));
        }

        private void write(JSONObject message) throws Exception {
            byte[] contents = message.toString().getBytes(StandardCharsets.UTF_8);
            assertTrue(contents.length > 0 && contents.length <= MAXIMUM_FRAME_LENGTH);
            output.writeInt(contents.length);
            output.write(contents);
            output.flush();
        }

        @Override
        public void close() throws Exception {
            descriptor.close();
        }
    }

    private static BoundService bindService() throws Exception {
        Context context = ApplicationProvider.getApplicationContext();
        CountDownLatch connected = new CountDownLatch(1);
        CountDownLatch disconnected = new CountDownLatch(1);
        AtomicReference<IMuonJavaScriptRuntimeService> serviceReference =
                new AtomicReference<>();
        ServiceConnection connection = new ServiceConnection() {
            @Override
            public void onServiceConnected(ComponentName name, IBinder binder) {
                serviceReference.set(
                        IMuonJavaScriptRuntimeService.Stub.asInterface(binder));
                connected.countDown();
            }

            @Override
            public void onServiceDisconnected(ComponentName name) {
                disconnected.countDown();
            }

            @Override
            public void onBindingDied(ComponentName name) {
                disconnected.countDown();
            }
        };
        assertTrue(context.bindService(
                new Intent(context, MuonJavaScriptRuntimeService.class),
                connection,
                Context.BIND_AUTO_CREATE));
        assertTrue(connected.await(30, TimeUnit.SECONDS));
        IMuonJavaScriptRuntimeService service = serviceReference.get();
        assertNotNull(service);
        return new BoundService(context, connection, disconnected, service);
    }

    private static RuntimeSocket createRuntime(
            IMuonJavaScriptRuntimeService service,
            String runtimeId) throws Exception {
        RuntimeSocket runtime = new RuntimeSocket(service.createRuntime(runtimeId));
        JSONObject handshake = runtime.read();
        assertEquals("handshake", handshake.getString("kind"));
        assertEquals("muon-js/1", handshake.getString("protocol"));
        JSONObject engine = handshake.getJSONObject("engine");
        assertEquals("quickjs", engine.getString("name"));
        assertEquals("2026-06-04", engine.getString("version"));
        assertTrue(handshake.getJSONArray("capabilities").length() >= 7);
        return runtime;
    }

    private static JSONObject request(
            RuntimeSocket runtime,
            String id,
            String command,
            JSONObject parameters) throws Exception {
        runtime.write(new JSONObject()
                .put("kind", "request")
                .put("id", id)
                .put("command", command)
                .put("params", parameters));
        JSONObject response = runtime.read();
        assertEquals("response", response.getString("kind"));
        assertEquals(id, response.getString("id"));
        return response;
    }

    private static String importModule(
            RuntimeSocket runtime,
            String id,
            String specifier) throws Exception {
        JSONObject response = request(
                runtime,
                id,
                "importModule",
                new JSONObject().put("specifier", specifier));
        assertTrue(response.toString(), response.getBoolean("ok"));
        return response.getJSONObject("value").getString("moduleId");
    }

    private static JSONObject call(
            RuntimeSocket runtime,
            String id,
            String moduleId,
            String exportName,
            JSONArray arguments) throws Exception {
        return request(
                runtime,
                id,
                "call",
                new JSONObject()
                        .put("moduleId", moduleId)
                        .put("exportName", exportName)
                        .put("arguments", arguments));
    }

    @Test
    public void runsIndependentQuickJsRuntimesAndNodeHostModules()
            throws Exception {
        try (BoundService binding = bindService()) {
            IMuonJavaScriptRuntimeService service = binding.service;
            assertEquals("2026-06-04", service.getEngineVersion());
            assertNotEquals(Process.myPid(), service.getProcessId());

            try (RuntimeSocket first = createRuntime(service, "test-first");
                 RuntimeSocket second = createRuntime(service, "test-second")) {
                assertEquals(2, service.getRuntimeCount());
                String firstRoot = importModule(first, "first-import", ".");
                String secondRoot = importModule(second, "second-import", ".");

                JSONObject firstIncrement = call(
                        first,
                        "first-increment",
                        firstRoot,
                        "increment",
                        new JSONArray());
                JSONObject secondIncrement = call(
                        second,
                        "second-increment",
                        secondRoot,
                        "increment",
                        new JSONArray());
                assertEquals(1, firstIncrement.getInt("value"));
                assertEquals(1, secondIncrement.getInt("value"));

                JSONObject integer = new JSONObject()
                        .put("kind", "u64")
                        .put("value", "18446744073709551615");
                JSONObject integerResult = call(
                        first,
                        "bigint",
                        firstRoot,
                        "echo",
                        new JSONArray().put(integer));
                assertEquals(integer.toString(),
                        integerResult.getJSONObject("value").toString());

                JSONObject buffer = new JSONObject()
                        .put("kind", "buffer")
                        .put("data", "AwEE");
                JSONObject bufferResult = call(
                        first,
                        "buffer",
                        firstRoot,
                        "echo",
                        new JSONArray().put(buffer));
                assertEquals(buffer.toString(),
                        bufferResult.getJSONObject("value").toString());

                first.write(new JSONObject()
                        .put("kind", "request")
                        .put("id", "callback-call")
                        .put("command", "call")
                        .put("params", new JSONObject()
                                .put("moduleId", firstRoot)
                                .put("exportName", "invokeCallback")
                                .put("arguments", new JSONArray()
                                        .put("from-runtime")
                                        .put(new JSONObject()
                                                .put("kind", "function")
                                                .put("handle", "renderer-callback")))));
                JSONObject callback = first.read();
                assertEquals("callback", callback.getString("kind"));
                assertEquals("renderer-callback", callback.getString("handle"));
                assertEquals("from-runtime", callback.getJSONArray("arguments").getString(0));
                first.write(new JSONObject()
                        .put("kind", "callbackResult")
                        .put("id", callback.getString("id"))
                        .put("ok", true)
                        .put("value", "from-renderer")
                        .put("error", JSONObject.NULL));
                JSONObject callbackResponse = first.read();
                assertEquals("callback-call", callbackResponse.getString("id"));
                assertEquals("from-renderer", callbackResponse.getString("value"));

                String promises = importModule(first, "fs-promises", "node:fs/promises");
                assertTrue(call(
                        first,
                        "mkdir",
                        promises,
                        "mkdir",
                        new JSONArray()
                                .put("service-test")
                                .put(new JSONObject().put("kind", "json")
                                        .put("value", new JSONObject().put("recursive", true))))
                        .getBoolean("ok"));
                assertTrue(call(
                        first,
                        "write",
                        promises,
                        "writeFile",
                        new JSONArray().put("service-test/value.txt").put("quickjs"))
                        .getBoolean("ok"));
                assertEquals("quickjs", call(
                        first,
                        "read",
                        promises,
                        "readFile",
                        new JSONArray().put("service-test/value.txt").put("utf8"))
                        .getString("value"));

                String promisesAlias = importModule(
                        first,
                        "fs-promises-alias",
                        "fs/promises");
                assertEquals("quickjs", call(
                        first,
                        "read-alias",
                        promisesAlias,
                        "readFile",
                        new JSONArray().put("service-test/value.txt").put("utf8"))
                        .getString("value"));

                String callbackFs = importModule(first, "fs-callback", "node:fs");
                first.write(new JSONObject()
                        .put("kind", "request")
                        .put("id", "fs-callback-call")
                        .put("command", "call")
                        .put("params", new JSONObject()
                                .put("moduleId", callbackFs)
                                .put("exportName", "readFile")
                                .put("arguments", new JSONArray()
                                        .put("service-test/value.txt")
                                        .put("utf8")
                                        .put(new JSONObject()
                                                .put("kind", "function")
                                                .put("handle", "fs-reader")))));
                JSONObject fsCallback = first.read();
                assertEquals("callback", fsCallback.getString("kind"));
                assertEquals("fs-reader", fsCallback.getString("handle"));
                assertTrue(fsCallback.getJSONArray("arguments").isNull(0));
                assertEquals("quickjs", fsCallback.getJSONArray("arguments").getString(1));
                first.write(new JSONObject()
                        .put("kind", "callbackResult")
                        .put("id", fsCallback.getString("id"))
                        .put("ok", true)
                        .put("value", new JSONObject().put("kind", "undefined"))
                        .put("error", JSONObject.NULL));
                assertTrue(first.read().getBoolean("ok"));

                String path = importModule(first, "path", "node:path");
                assertEquals("alpha/gamma", call(
                        first,
                        "join",
                        path,
                        "join",
                        new JSONArray().put("alpha").put("beta").put("..").put("gamma"))
                        .getString("value"));

                String timers = importModule(first, "timers", "node:timers/promises");
                assertEquals("awake", call(
                        first,
                        "timer",
                        timers,
                        "setTimeout",
                        new JSONArray().put(10).put("awake"))
                        .getString("value"));

                assertTrue(request(
                        first,
                        "shutdown-first",
                        "shutdown",
                        new JSONObject()).getBoolean("ok"));
                JSONObject survivingIncrement = call(
                        second,
                        "surviving-increment",
                        secondRoot,
                        "increment",
                        new JSONArray());
                assertTrue(survivingIncrement.getBoolean("ok"));
                assertEquals(2, survivingIncrement.getInt("value"));
                assertTrue(request(
                        second,
                        "shutdown-second",
                        "shutdown",
                        new JSONObject()).getBoolean("ok"));
            }
        }
    }

    @Test
    public void interruptsRunawayJavaScriptWithoutKillingTheService()
            throws Exception {
        try (BoundService binding = bindService();
             RuntimeSocket runtime = createRuntime(binding.service, "test-interrupt")) {
            String root = importModule(runtime, "import", ".");
            JSONObject interrupted = call(
                    runtime,
                    "spin",
                    root,
                    "spin",
                    new JSONArray());
            assertFalse(interrupted.getBoolean("ok"));
            assertEquals(
                    "ERR_MUON_JS_INTERRUPTED",
                    interrupted.getJSONObject("error").getString("code"));

            JSONObject recovered = call(
                    runtime,
                    "recovered",
                    root,
                    "echo",
                    new JSONArray().put("still-running"));
            assertTrue(recovered.getBoolean("ok"));
            assertEquals("still-running", recovered.getString("value"));
        }
    }

    @Test
    public void enforcesTheRuntimeMemoryLimitWithoutKillingTheService()
            throws Exception {
        try (BoundService binding = bindService();
             RuntimeSocket runtime = createRuntime(binding.service, "test-memory-limit")) {
            String root = importModule(runtime, "import", ".");
            JSONObject exhausted = call(
                    runtime,
                    "exhaust-memory",
                    root,
                    "exhaustMemory",
                    new JSONArray());
            assertFalse(exhausted.getBoolean("ok"));
            assertEquals(
                    "ERR_MUON_JS_OUT_OF_MEMORY",
                    exhausted.getJSONObject("error").getString("code"));

            JSONObject recovered = call(
                    runtime,
                    "recovered",
                    root,
                    "echo",
                    new JSONArray().put("still-running"));
            assertTrue(recovered.getBoolean("ok"));
            assertEquals("still-running", recovered.getString("value"));
        }
    }

    @Test
    public void restartsAfterThePrivateServiceProcessIsTerminated()
            throws Exception {
        int firstProcessId;
        BoundService first = bindService();
        try {
            firstProcessId = first.service.getProcessId();
            try {
                first.service.terminateForTest();
            } catch (Exception ignored) {
                // The Binder transaction can lose its reply when the target exits.
            }
            assertTrue(first.awaitDisconnected());
        } finally {
            first.close();
        }

        try (BoundService replacement = bindService();
             RuntimeSocket runtime = createRuntime(
                     replacement.service,
                     "test-restarted")) {
            assertNotEquals(firstProcessId, replacement.service.getProcessId());
            String root = importModule(runtime, "import", ".");
            JSONObject response = call(
                    runtime,
                    "echo",
                    root,
                    "echo",
                    new JSONArray().put("restarted"));
            assertTrue(response.getBoolean("ok"));
            assertEquals("restarted", response.getString("value"));
        }
    }
}

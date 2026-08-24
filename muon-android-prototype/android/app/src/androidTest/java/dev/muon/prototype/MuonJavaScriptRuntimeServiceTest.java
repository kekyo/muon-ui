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
import android.os.Build;
import android.os.IBinder;
import android.os.ParcelFileDescriptor;
import android.os.Process;
import android.system.Os;
import android.system.OsConstants;
import android.system.StructPollfd;
import android.util.Base64;

import androidx.test.core.app.ApplicationProvider;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;

import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.Test;
import org.junit.runner.RunWith;

import java.io.ByteArrayOutputStream;
import java.io.ByteArrayInputStream;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.EOFException;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.security.KeyFactory;
import java.security.KeyStore;
import java.security.PrivateKey;
import java.security.cert.Certificate;
import java.security.cert.CertificateFactory;
import java.security.spec.PKCS8EncodedKeySpec;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

import javax.net.ssl.KeyManagerFactory;
import javax.net.ssl.SSLContext;
import javax.net.ssl.SSLException;
import javax.net.ssl.SSLServerSocket;

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
        JSONArray capabilities = handshake.getJSONArray("capabilities");
        assertTrue(capabilities.toString(),
                capabilities.toString().contains("\"node:dns\""));
        assertTrue(capabilities.toString(),
                capabilities.toString().contains("\"node:net\""));
        assertTrue(capabilities.toString(),
                capabilities.toString().contains("\"tcp\""));
        assertTrue(capabilities.toString(),
                capabilities.toString().contains("\"tcp-server\""));
        assertTrue(capabilities.toString(),
                capabilities.toString().contains("\"node:http\""));
        assertTrue(capabilities.toString(),
                capabilities.toString().contains("\"http-server\""));
        assertTrue(capabilities.toString(),
                capabilities.toString().contains("\"node:https\""));
        assertTrue(capabilities.toString(),
                capabilities.toString().contains("\"fetch\""));
        assertTrue(capabilities.toString(),
                capabilities.toString().contains("\"node:process\""));
        assertTrue(capabilities.toString(),
                capabilities.toString().contains("\"node:os\""));
        assertTrue(capabilities.toString(),
                capabilities.toString().contains("\"node:util\""));
        assertTrue(capabilities.toString(),
                capabilities.toString().contains("\"node:assert\""));
        assertTrue(capabilities.toString(),
                capabilities.toString().contains("\"node:querystring\""));
        assertTrue(capabilities.toString(),
                capabilities.toString().contains("\"node:string_decoder\""));
        assertTrue(capabilities.toString(),
                capabilities.toString().contains("\"node:crypto\""));
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

    private static String readHttpLine(InputStream input) throws IOException {
        ByteArrayOutputStream line = new ByteArrayOutputStream();
        while (true) {
            int value = input.read();
            if (value < 0) {
                if (line.size() == 0) {
                    return null;
                }
                throw new EOFException("HTTP line ended before a newline");
            }
            if (value == '\n') {
                return line.toString(StandardCharsets.ISO_8859_1.name());
            }
            if (value != '\r') {
                line.write(value);
                if (line.size() > 64 * 1024) {
                    throw new IOException("HTTP line exceeded the test limit");
                }
            }
        }
    }

    private static String readTestAsset(String name) throws IOException {
        try (InputStream input = InstrumentationRegistry
                .getInstrumentation()
                .getContext()
                .getAssets()
                .open(name);
             ByteArrayOutputStream output = new ByteArrayOutputStream()) {
            byte[] buffer = new byte[4096];
            while (true) {
                int count = input.read(buffer);
                if (count < 0) {
                    break;
                }
                output.write(buffer, 0, count);
            }
            return output.toString(StandardCharsets.US_ASCII.name());
        }
    }

    private static SSLContext createLoopbackTlsContext(
            String certificatePem,
            String privateKeyPem) throws Exception {
        CertificateFactory certificateFactory = CertificateFactory.getInstance("X.509");
        Certificate certificate = certificateFactory.generateCertificate(
                new ByteArrayInputStream(certificatePem.getBytes(StandardCharsets.US_ASCII)));
        String encodedKey = privateKeyPem
                .replace("-----BEGIN PRIVATE KEY-----", "")
                .replace("-----END PRIVATE KEY-----", "")
                .replaceAll("\\s", "");
        PrivateKey privateKey = KeyFactory
                .getInstance("RSA")
                .generatePrivate(new PKCS8EncodedKeySpec(
                        Base64.decode(encodedKey, Base64.DEFAULT)));
        char[] password = new char[0];
        KeyStore keyStore = KeyStore.getInstance(KeyStore.getDefaultType());
        keyStore.load(null, null);
        keyStore.setKeyEntry(
                "localhost",
                privateKey,
                password,
                new Certificate[]{certificate});
        KeyManagerFactory keyManagerFactory = KeyManagerFactory.getInstance(
                KeyManagerFactory.getDefaultAlgorithm());
        keyManagerFactory.init(keyStore, password);
        SSLContext context = SSLContext.getInstance("TLS");
        context.init(keyManagerFactory.getKeyManagers(), null, null);
        return context;
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

                JSONObject importedPathBasename = call(
                        first,
                        "imported-path-basename",
                        firstRoot,
                        "importedPathBasename",
                        new JSONArray().put("alpha/value.txt"));
                assertTrue(importedPathBasename.toString(),
                        importedPathBasename.getBoolean("ok"));
                assertEquals("value.txt", importedPathBasename.getString("value"));

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
    public void supportsNodeRuntimePrimitives() throws Exception {
        try (BoundService binding = bindService();
             RuntimeSocket runtime = createRuntime(binding.service, "test-runtime-primitives")) {
            String root = importModule(runtime, "import", ".");
            JSONObject response = call(
                    runtime,
                    "runtime-primitives",
                    root,
                    "exerciseRuntimePrimitives",
                    new JSONArray());
            assertTrue(response.toString(), response.getBoolean("ok"));
            JSONObject values = response
                    .getJSONObject("value")
                    .getJSONObject("value");

            JSONObject events = values.getJSONObject("events");
            assertEquals(
                    "[\"pre:first\",\"on:first:true\",\"once:first\",\"pre:second\"]",
                    events.getJSONArray("values").toString());
            assertTrue(events.getBoolean("firstEmit"));
            assertTrue(events.getBoolean("secondEmit"));
            assertFalse(events.getBoolean("emptyEmit"));
            assertEquals("event-ready", events.getString("awaitedEvent"));

            JSONObject buffer = values.getJSONObject("buffer");
            assertEquals("muon✓!", buffer.getString("text"));
            assertEquals("6d756f6ee29c9321", buffer.getString("hex"));
            assertEquals("bXVvbuKckyE=", buffer.getString("base64"));
            assertEquals(7, buffer.getInt("byteLength"));
            assertTrue(buffer.getBoolean("isBuffer"));
            assertTrue(buffer.getBoolean("isUint8Array"));
            assertTrue(buffer.getBoolean("equalsCopy"));

            JSONObject timers = values.getJSONObject("timers");
            assertEquals("timeout-ready", timers.getString("timeoutValue"));
            assertEquals("immediate-ready", timers.getString("immediateValue"));
            assertEquals(2, timers.getInt("intervalCount"));
            assertEquals(1, timers.getInt("zeroDelayIntervalCount"));
            assertFalse(timers.getBoolean("cancelledTimeoutCalled"));
            assertFalse(timers.getBoolean("cancelledImmediateCalled"));
            assertTrue(timers.getBoolean("initiallyReferenced"));
            assertFalse(timers.getBoolean("referencedAfterUnref"));
            assertTrue(timers.getBoolean("referencedAfterRef"));

            JSONObject abort = values.getJSONObject("abort");
            assertEquals("AbortError", abort.getString("abortName"));
            assertEquals("ABORT_ERR", abort.getString("abortCode"));
            assertEquals("static-reason", abort.getString("staticReason"));
            assertEquals("combined-reason", abort.getString("combinedReason"));
            assertEquals("TimeoutError", abort.getString("timeoutReasonName"));
        }
    }

    @Test
    public void supportsNodeProcessAndOs() throws Exception {
        try (BoundService binding = bindService();
             RuntimeSocket runtime = createRuntime(binding.service, "test-process-os")) {
            String root = importModule(runtime, "import", ".");
            JSONObject response = call(
                    runtime,
                    "process-os",
                    root,
                    "exerciseProcessAndOs",
                    new JSONArray());
            assertTrue(response.toString(), response.getBoolean("ok"));
            JSONObject values = response
                    .getJSONObject("value")
                    .getJSONObject("value");

            String abi = Build.SUPPORTED_ABIS[0];
            String expectedArchitecture;
            switch (abi) {
                case "arm64-v8a":
                    expectedArchitecture = "arm64";
                    break;
                case "armeabi-v7a":
                    expectedArchitecture = "arm";
                    break;
                case "x86_64":
                    expectedArchitecture = "x64";
                    break;
                case "x86":
                    expectedArchitecture = "ia32";
                    break;
                default:
                    throw new AssertionError("Unsupported test ABI: " + abi);
            }

            JSONObject process = values.getJSONObject("process");
            assertTrue(process.getBoolean("moduleAlias"));
            assertTrue(process.getBoolean("globalAlias"));
            assertTrue(process.getBoolean("isEventEmitter"));
            assertEquals("event-ready", process.getString("emittedValue"));
            assertEquals(expectedArchitecture, process.getString("arch"));
            assertEquals("android", process.getString("platform"));
            assertEquals("/", process.getString("cwd"));
            assertEquals("[\"muon-quickjs\"]", process.getJSONArray("argv").toString());
            assertEquals("[]", process.getJSONArray("execArgv").toString());
            assertTrue(process.getBoolean("pidIsPositiveInteger"));
            assertEquals("v0.0.0-muon-quickjs", process.getString("version"));
            assertEquals("2026-06-04", process.getString("quickjsVersion"));
            assertEquals("muon-quickjs", process.getString("releaseName"));
            assertEquals("42", process.getString("environmentValue"));
            assertTrue(process.getBoolean("environmentDeleted"));
            assertTrue(process.getBoolean("uptimeIncreased"));
            assertTrue(process.getLong("elapsedNanoseconds") > 0);
            assertTrue(process.getBoolean("bigintIncreased"));

            JSONObject os = values.getJSONObject("os");
            assertTrue(os.getBoolean("moduleAlias"));
            assertEquals(expectedArchitecture, os.getString("arch"));
            assertEquals("android", os.getString("platform"));
            assertEquals("Android", os.getString("type"));
            assertEquals("LE", os.getString("endianness"));
            assertEquals("\n", os.getString("eol"));
            assertEquals("/dev/null", os.getString("devNull"));
            assertEquals("/", os.getString("homedir"));
            assertEquals("/tmp", os.getString("tmpdir"));
            JSONObject userInfo = os.getJSONObject("userInfo");
            assertEquals("muon", userInfo.getString("username"));
            assertEquals(-1, userInfo.getInt("uid"));
            assertEquals(-1, userInfo.getInt("gid"));
            assertTrue(userInfo.isNull("shell"));
            assertEquals("/", userInfo.getString("homedir"));

            assertTrue(request(
                    runtime,
                    "shutdown-process-os",
                    "shutdown",
                    new JSONObject()).getBoolean("ok"));
        }
    }

    @Test
    public void supportsNodeUtilityModules() throws Exception {
        try (BoundService binding = bindService();
             RuntimeSocket runtime = createRuntime(binding.service, "test-utility-modules")) {
            String root = importModule(runtime, "import", ".");
            JSONObject response = call(
                    runtime,
                    "utility-modules",
                    root,
                    "exerciseUtilityModules",
                    new JSONArray());
            assertTrue(response.toString(), response.getBoolean("ok"));
            JSONObject values = response
                    .getJSONObject("value")
                    .getJSONObject("value");

            JSONObject util = values.getJSONObject("util");
            assertTrue(util.getBoolean("moduleAlias"));
            assertEquals(42, util.getInt("promisifiedValue"));
            assertEquals("EUTIL", util.getString("promisifiedErrorCode"));
            assertTrue(util.getBoolean("customPromisified"));
            assertEquals(
                    "name=muon count=2 json={\"ok\":true} %",
                    util.getString("formatted"));
            assertEquals(
                    "{ name: 'muon', count: 2 }",
                    util.getString("inspected"));
            assertEquals("MuonCustom", util.getString("customInspected"));
            assertTrue(util.getString("circularInspected").contains("[Circular]"));
            assertTrue(util.getBoolean("deepCircular"));
            assertFalse(util.getBoolean("deepDifferent"));
            assertEquals("muon", util.getString("stripped"));
            JSONObject types = util.getJSONObject("types");
            assertTrue(types.getBoolean("bufferIsUint8Array"));
            assertTrue(types.getBoolean("promiseIsPromise"));
            assertTrue(types.getBoolean("mapIsMap"));

            JSONObject assertValues = values.getJSONObject("assert");
            assertTrue(assertValues.getBoolean("moduleAlias"));
            assertTrue(assertValues.getBoolean("strictLegacyAliasRejected"));
            JSONObject failure = assertValues.getJSONObject("failure");
            assertTrue(failure.getBoolean("isAssertionError"));
            assertEquals("AssertionError", failure.getString("name"));
            assertEquals("ERR_ASSERTION", failure.getString("code"));
            assertEquals("different values", failure.getString("message"));
            assertEquals(1, failure.getInt("actual"));
            assertEquals(2, failure.getInt("expected"));
            assertEquals("strictEqual", failure.getString("operator"));
            assertFalse(failure.getBoolean("generatedMessage"));

            JSONObject querystring = values.getJSONObject("querystring");
            assertTrue(querystring.getBoolean("moduleAlias"));
            assertTrue(querystring.getBoolean("encodeAlias"));
            assertTrue(querystring.getBoolean("decodeAlias"));
            assertEquals(
                    "foo=bar&abc=xyz&abc=123&space=a%20b&symbol=%E2%9C%93"
                            + "&nil=&truth=true&object=",
                    querystring.getString("encoded"));
            JSONObject decoded = querystring.getJSONObject("decoded");
            assertEquals("bar", decoded.getString("foo"));
            assertEquals(
                    "[\"xyz\",\"123\"]",
                    decoded.getJSONArray("abc").toString());
            assertEquals("a b", decoded.getString("space"));
            assertEquals("%zz", decoded.getString("bad"));
            assertEquals("key:one;key:two", querystring.getString("custom"));
            assertEquals("%zz", querystring.getString("malformed"));

            JSONObject decoder = values.getJSONObject("stringDecoder");
            assertTrue(decoder.getBoolean("moduleAlias"));
            assertEquals(
                    "[\"\",\"\",\"€\"]",
                    decoder.getJSONArray("utf8Parts").toString());
            assertEquals("�", decoder.getString("incompleteUtf8"));
            assertEquals(
                    "[\"\",\"𝄞\"]",
                    decoder.getJSONArray("utf16Parts").toString());
            assertEquals(
                    "[\"\",\"bXVvbg==\"]",
                    decoder.getJSONArray("base64Parts").toString());
            assertEquals("â", decoder.getString("latin1"));

            assertTrue(request(
                    runtime,
                    "shutdown-utility-modules",
                    "shutdown",
                    new JSONObject()).getBoolean("ok"));
        }
    }

    @Test
    public void supportsNodeCrypto() throws Exception {
        try (BoundService binding = bindService();
             RuntimeSocket runtime = createRuntime(binding.service, "test-crypto")) {
            String root = importModule(runtime, "import", ".");
            JSONObject response = call(
                    runtime,
                    "crypto",
                    root,
                    "exerciseCrypto",
                    new JSONArray());
            assertTrue(response.toString(), response.getBoolean("ok"));
            JSONObject values = response
                    .getJSONObject("value")
                    .getJSONObject("value");

            assertTrue(values.getBoolean("moduleAlias"));
            assertTrue(values.getBoolean("globalRandomValues"));
            assertEquals("[\"sha256\"]", values.getJSONArray("hashes").toString());
            assertEquals(
                    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                    values.getString("sha256"));
            assertEquals(
                    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                    values.getString("rollingSha256"));
            assertEquals(
                    "ca978112ca1bbdcafac231b39a23dc4da786eff8147c4e72b9807785afee48bb",
                    values.getString("copiedSha256"));
            assertEquals(
                    "ungWv48Bz+pBQUDeXa4iI7ADYaOWF3qctBD/YfIAFa0=",
                    values.getString("sha256Base64"));
            assertEquals(values.getString("sha256"), values.getString("oneShotSha256"));
            assertEquals(
                    "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8",
                    values.getString("hmacSha256"));

            JSONObject randomBytes = values.getJSONObject("randomBytes");
            assertEquals(32, randomBytes.getInt("length"));
            assertTrue(randomBytes.getBoolean("isBuffer"));
            assertEquals(12, randomBytes.getInt("callbackLength"));

            JSONObject partialFill = values.getJSONObject("partialFill");
            assertEquals(8, partialFill.getInt("length"));
            assertTrue(partialFill.getBoolean("prefixPreserved"));
            assertTrue(partialFill.getBoolean("suffixPreserved"));
            assertEquals(9, partialFill.getInt("callbackLength"));

            JSONObject randomInt = values.getJSONObject("randomInt");
            assertTrue(randomInt.getInt("synchronous") >= 10);
            assertTrue(randomInt.getInt("synchronous") < 20);
            assertTrue(randomInt.getInt("callback") >= 20);
            assertTrue(randomInt.getInt("callback") < 30);

            JSONObject randomValues = values.getJSONObject("randomValues");
            assertTrue(randomValues.getBoolean("sameObject"));
            assertEquals(16, randomValues.getInt("byteLength"));

            String uuid = values.getString("uuid");
            assertTrue(uuid, uuid.matches(
                    "^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}"
                            + "-[0-9a-f]{12}$"));
            assertEquals("4", values.getString("uuidVersion"));
            assertTrue("89ab".contains(values.getString("uuidVariant")));

            JSONObject timingSafeEqual = values.getJSONObject("timingSafeEqual");
            assertTrue(timingSafeEqual.getBoolean("equal"));
            assertFalse(timingSafeEqual.getBoolean("different"));
            assertEquals(
                    "ERR_CRYPTO_TIMING_SAFE_EQUAL_LENGTH",
                    timingSafeEqual.getString("lengthErrorCode"));
            assertEquals("ERR_OUT_OF_RANGE", values.getString("sizeErrorCode"));
            assertEquals(
                    "ERR_CRYPTO_UNKNOWN_HASH",
                    values.getString("algorithmErrorCode"));

            assertTrue(request(
                    runtime,
                    "shutdown-crypto",
                    "shutdown",
                    new JSONObject()).getBoolean("ok"));
        }
    }

    @Test
    public void supportsNodeStreamsAndUrls() throws Exception {
        try (BoundService binding = bindService();
             RuntimeSocket runtime = createRuntime(binding.service, "test-stream-url")) {
            String root = importModule(runtime, "import", ".");
            JSONObject response = call(
                    runtime,
                    "stream-url",
                    root,
                    "exerciseStreamAndUrl",
                    new JSONArray());
            assertTrue(response.toString(), response.getBoolean("ok"));
            JSONObject values = response
                    .getJSONObject("value")
                    .getJSONObject("value");

            JSONObject stream = values.getJSONObject("stream");
            assertEquals("MUON-STREAM", stream.getString("output"));
            assertEquals("async-iterator", stream.getString("asyncIteratorOutput"));
            assertEquals("pass-through", stream.getString("passThroughOutput"));
            assertFalse(stream.getBoolean("acceptedWithoutBackpressure"));
            assertEquals("[\"four\",\"done\"]", stream.getJSONArray("slowWrites").toString());
            assertEquals(
                    "{\"readable\":true,\"writable\":true,\"destroyed\":false}",
                    stream.getJSONObject("stateBeforeDestroy").toString());
            assertEquals(
                    "{\"readable\":false,\"writable\":false,\"destroyed\":true}",
                    stream.getJSONObject("stateAfterDestroy").toString());

            JSONObject url = values.getJSONObject("url");
            assertEquals(
                    "https://user:pass@example.com:8443/root/child"
                            + "?alpha=3&space=a+b#section",
                    url.getString("href"));
            assertEquals("https:", url.getString("protocol"));
            assertEquals("user", url.getString("username"));
            assertEquals("pass", url.getString("password"));
            assertEquals("example.com", url.getString("hostname"));
            assertEquals("8443", url.getString("port"));
            assertEquals("example.com:8443", url.getString("host"));
            assertEquals("https://example.com:8443", url.getString("origin"));
            assertEquals("/root/child", url.getString("pathname"));
            assertEquals("?alpha=3&space=a+b", url.getString("search"));
            assertEquals("#section", url.getString("hash"));
            assertEquals("[\"3\"]", url.getJSONArray("alpha").toString());
            assertEquals(
                    "[[\"alpha\",\"3\"],[\"space\",\"a b\"]]",
                    url.getJSONArray("entries").toString());

            JSONObject httpOptions = url.getJSONObject("httpOptions");
            assertEquals("https:", httpOptions.getString("protocol"));
            assertEquals("example.com", httpOptions.getString("hostname"));
            assertEquals(8443, httpOptions.getInt("port"));
            assertEquals("user:pass", httpOptions.getString("auth"));
            assertEquals(
                    "/root/child?alpha=3&space=a+b",
                    httpOptions.getString("path"));

            JSONObject parameters = url.getJSONObject("parameters");
            assertEquals("plus=a+b&empty=&dup=y", parameters.getString("text"));
            assertEquals("a b", parameters.getString("plus"));
            assertTrue(parameters.getBoolean("hasDuplicate"));
            assertEquals(3, parameters.getInt("size"));
            assertEquals(
                    "file:///data/user/0/app%20files/%C3%A9.txt",
                    url.getString("fileHref"));
            assertEquals(
                    "/data/user/0/app files/é.txt",
                    url.getString("filePath"));
            assertTrue(url.getBoolean("canParseRelative"));
        }
    }

    @Test
    public void supportsAsynchronousDnsAndTcpClients() throws Exception {
        AtomicReference<String> receivedByServer = new AtomicReference<>();
        AtomicReference<Throwable> serverFailure = new AtomicReference<>();
        CountDownLatch serverFinished = new CountDownLatch(1);
        try (ServerSocket server = new ServerSocket(
                0,
                1,
                InetAddress.getByName("127.0.0.1"))) {
            server.setSoTimeout(30_000);
            int closedPort;
            try (ServerSocket closedServer = new ServerSocket(
                    0,
                    1,
                    InetAddress.getByName("127.0.0.1"))) {
                closedPort = closedServer.getLocalPort();
            }
            Thread serverThread = new Thread(() -> {
                try (Socket connection = server.accept()) {
                    connection.setSoTimeout(30_000);
                    ByteArrayOutputStream received = new ByteArrayOutputStream();
                    byte[] buffer = new byte[64];
                    while (true) {
                        int count = connection.getInputStream().read(buffer);
                        if (count < 0) {
                            break;
                        }
                        received.write(buffer, 0, count);
                    }
                    receivedByServer.set(received.toString(StandardCharsets.UTF_8.name()));
                    connection.getOutputStream().write(
                            "reply:".getBytes(StandardCharsets.UTF_8));
                    connection.getOutputStream().flush();
                    connection.getOutputStream().write(
                            received.toByteArray());
                    connection.getOutputStream().flush();
                    connection.shutdownOutput();
                } catch (Throwable error) {
                    serverFailure.set(error);
                } finally {
                    serverFinished.countDown();
                }
            }, "muon-quickjs-tcp-server");
            serverThread.setDaemon(true);
            serverThread.start();

            try (BoundService binding = bindService();
                 RuntimeSocket runtime = createRuntime(binding.service, "test-dns-tcp")) {
                String root = importModule(runtime, "import", ".");
                JSONObject response = call(
                        runtime,
                        "dns-tcp",
                        root,
                        "exerciseDnsAndTcp",
                        new JSONArray()
                                .put(server.getLocalPort())
                                .put(closedPort));
                assertTrue(response.toString(), response.getBoolean("ok"));
                JSONObject values = response
                        .getJSONObject("value")
                        .getJSONObject("value");

                JSONObject modules = values.getJSONObject("modules");
                assertTrue(modules.getBoolean("netAlias"));
                assertTrue(modules.getBoolean("dnsAlias"));
                assertTrue(modules.getBoolean("promises"));

                JSONObject ip = values.getJSONObject("ip");
                assertEquals(4, ip.getInt("ipv4"));
                assertEquals(6, ip.getInt("ipv6"));
                assertEquals(0, ip.getInt("invalid"));
                assertTrue(ip.getBoolean("isIpv4"));
                assertTrue(ip.getBoolean("isIpv6"));

                JSONObject dns = values.getJSONObject("dns");
                assertEquals("127.0.0.1",
                        dns.getJSONObject("callbackLookup").getString("address"));
                assertEquals(4,
                        dns.getJSONObject("callbackLookup").getInt("family"));
                assertEquals("127.0.0.1",
                        dns.getJSONObject("promiseLookup").getString("address"));
                assertEquals(4,
                        dns.getJSONObject("promiseLookup").getInt("family"));
                assertTrue(dns.getJSONArray("allLookup").length() >= 1);
                assertEquals(3, dns.getJSONArray("concurrentLookups").length());
                assertEquals("ipv4first", dns.getString("resultOrder"));

                JSONObject tcp = values.getJSONObject("tcp");
                assertTrue(tcp.getBoolean("initiallyConnecting"));
                assertTrue(tcp.getBoolean("initiallyPending"));
                assertEquals("opening", tcp.getString("initialReadyState"));
                assertTrue(tcp.getBoolean("isSocket"));
                assertEquals("reply:quickjs-tcp", tcp.getString("received"));
                JSONArray events = tcp.getJSONArray("events");
                assertEquals("connect", events.getString(0));
                assertEquals("ready", events.getString(1));
                assertEquals("end", events.getString(events.length() - 2));
                assertEquals("close", events.getString(events.length() - 1));
                JSONObject connection = tcp.getJSONObject("connection");
                assertEquals("127.0.0.1", connection.getString("remoteAddress"));
                assertEquals("IPv4", connection.getString("remoteFamily"));
                assertEquals(server.getLocalPort(), connection.getInt("remotePort"));
                assertEquals("open", connection.getString("readyState"));
                assertEquals("IPv4",
                        connection.getJSONObject("local").getString("family"));
                assertEquals(11, tcp.getInt("bytesWritten"));
                assertEquals(17, tcp.getInt("bytesRead"));
                assertFalse(tcp.getBoolean("hadError"));
                assertTrue(tcp.getBoolean("destroyed"));

                JSONObject failure = values.getJSONObject("failure");
                assertEquals("[\"error\",\"close\"]",
                        failure.getJSONArray("events").toString());
                assertEquals("ECONNREFUSED", failure.getString("code"));
                assertEquals("connect", failure.getString("syscall"));
                assertTrue(failure.getBoolean("hadError"));
                assertTrue(failure.getBoolean("destroyed"));
            }
        }
        assertTrue(serverFinished.await(30, TimeUnit.SECONDS));
        if (serverFailure.get() != null) {
            throw new AssertionError("The loopback TCP server failed", serverFailure.get());
        }
        assertEquals("quickjs-tcp", receivedByServer.get());
    }

    @Test
    public void supportsNodeNetLoopbackServers() throws Exception {
        try (BoundService binding = bindService();
             RuntimeSocket runtime = createRuntime(binding.service, "test-net-server")) {
            String root = importModule(runtime, "import", ".");
            JSONObject response = call(
                    runtime,
                    "net-server",
                    root,
                    "exerciseNetServer",
                    new JSONArray());
            assertTrue(response.toString(), response.getBoolean("ok"));
            JSONObject values = response
                    .getJSONObject("value")
                    .getJSONObject("value");
            assertTrue(values.getBoolean("moduleAlias"));
            assertTrue(values.getBoolean("isServer"));
            assertTrue(values.getBoolean("listeningBeforeClose"));
            JSONObject address = values.getJSONObject("address");
            assertEquals("127.0.0.1", address.getString("address"));
            assertEquals("IPv4", address.getString("family"));
            assertTrue(address.getInt("port") > 0);
            assertTrue(values.isNull("addressAfterClose"));
            assertEquals(
                    "[\"listening\",\"connection\",\"close\"]",
                    values.getJSONArray("events").toString());
            assertEquals("loopback-input", values.getString("receivedByServer"));
            assertEquals("loopback-response", values.getString("receivedByClient"));
            JSONObject accepted = values.getJSONObject("acceptedSocketState");
            assertTrue(accepted.getBoolean("isSocket"));
            assertEquals("127.0.0.1", accepted.getString("localAddress"));
            assertEquals("IPv4", accepted.getString("localFamily"));
            assertEquals(address.getInt("port"), accepted.getInt("localPort"));
            assertEquals("127.0.0.1", accepted.getString("remoteAddress"));
            assertEquals("IPv4", accepted.getString("remoteFamily"));
            assertEquals("open", accepted.getString("readyState"));
            assertEquals(1, values.getInt("activeConnectionCount"));
            assertEquals(0, values.getInt("finalConnectionCount"));
            assertEquals("EACCES", values.getString("nonLoopbackCode"));
        }
    }

    @Test
    public void supportsNodeHttpAndFetchClients() throws Exception {
        AtomicReference<Throwable> serverFailure = new AtomicReference<>();
        AtomicReference<String> observedPaths = new AtomicReference<>();
        CountDownLatch serverFinished = new CountDownLatch(1);
        try (ServerSocket server = new ServerSocket(
                0,
                6,
                InetAddress.getByName("127.0.0.1"))) {
            server.setSoTimeout(30_000);
            int port = server.getLocalPort();
            Thread serverThread = new Thread(() -> {
                StringBuilder paths = new StringBuilder();
                try {
                    for (int index = 0; index < 6; index++) {
                        try (Socket connection = server.accept()) {
                            connection.setSoTimeout(30_000);
                            InputStream input = connection.getInputStream();
                            String requestLine = readHttpLine(input);
                            if (requestLine == null) {
                                throw new EOFException("HTTP request line was missing");
                            }
                            String[] requestParts = requestLine.split(" ", 3);
                            assertEquals(3, requestParts.length);
                            String method = requestParts[0];
                            String path = requestParts[1];
                            if (paths.length() > 0) {
                                paths.append(',');
                            }
                            paths.append(path);

                            Map<String, String> headers = new HashMap<>();
                            while (true) {
                                String line = readHttpLine(input);
                                if (line == null) {
                                    throw new EOFException("HTTP headers ended early");
                                }
                                if (line.isEmpty()) {
                                    break;
                                }
                                int separator = line.indexOf(':');
                                assertTrue(line, separator > 0);
                                headers.put(
                                        line.substring(0, separator)
                                                .trim()
                                                .toLowerCase(Locale.ROOT),
                                        line.substring(separator + 1).trim());
                            }
                            int contentLength = Integer.parseInt(
                                    headers.getOrDefault("content-length", "0"));
                            byte[] requestBody = new byte[contentLength];
                            new DataInputStream(input).readFully(requestBody);
                            String body = new String(requestBody, StandardCharsets.UTF_8);

                            String status;
                            String extraHeaders;
                            byte[] responseBody;
                            switch (path) {
                                case "/node":
                                    assertEquals("POST", method);
                                    assertEquals("node-body", body);
                                    assertEquals("node", headers.get("x-client"));
                                    assertEquals("yes", headers.get("x-later"));
                                    assertFalse(headers.containsKey("x-remove"));
                                    status = "201 Created";
                                    extraHeaders = "X-Test: node\r\n"
                                            + "Content-Type: text/plain; charset=utf-8\r\n";
                                    responseBody = "node-response"
                                            .getBytes(StandardCharsets.UTF_8);
                                    break;
                                case "/node-get":
                                    assertEquals("GET", method);
                                    assertEquals("", body);
                                    assertEquals("get", headers.get("x-client"));
                                    status = "200 OK";
                                    extraHeaders = "X-Test: get\r\n";
                                    responseBody = "get-response"
                                            .getBytes(StandardCharsets.UTF_8);
                                    break;
                                case "/fetch":
                                    assertEquals("POST", method);
                                    assertEquals("fetch-body", body);
                                    assertEquals("yes", headers.get("x-fetch"));
                                    status = "200 OK";
                                    extraHeaders = "X-Test: fetch\r\n"
                                            + "Content-Type: application/json\r\n";
                                    responseBody = ("{\"received\":\"fetch-body\","
                                            + "\"header\":\"fetch\"}")
                                            .getBytes(StandardCharsets.UTF_8);
                                    break;
                                case "/redirect":
                                    assertEquals("GET", method);
                                    status = "302 Found";
                                    extraHeaders = "Location: http://localhost:"
                                            + port
                                            + "/fetch-target\r\n";
                                    responseBody = new byte[0];
                                    break;
                                case "/fetch-target":
                                    assertEquals("GET", method);
                                    status = "200 OK";
                                    extraHeaders = "X-Test: redirected\r\n";
                                    responseBody = "redirected"
                                            .getBytes(StandardCharsets.UTF_8);
                                    break;
                                case "/slow":
                                    assertEquals("GET", method);
                                    DataOutputStream slowOutput = new DataOutputStream(
                                            connection.getOutputStream());
                                    slowOutput.write(("HTTP/1.1 200 OK\r\n"
                                            + "Content-Type: text/plain\r\n"
                                            + "Content-Length: 64\r\n"
                                            + "Connection: close\r\n"
                                            + "\r\n")
                                            .getBytes(StandardCharsets.ISO_8859_1));
                                    slowOutput.flush();
                                    while (input.read() >= 0) {
                                        // Aborting the fetch must close the connection.
                                    }
                                    continue;
                                default:
                                    throw new AssertionError("Unexpected path: " + path);
                            }

                            DataOutputStream output = new DataOutputStream(
                                    connection.getOutputStream());
                            output.write(("HTTP/1.1 "
                                    + status
                                    + "\r\n"
                                    + extraHeaders
                                    + "Content-Length: "
                                    + responseBody.length
                                    + "\r\n"
                                    + "Connection: close\r\n"
                                    + "\r\n")
                                    .getBytes(StandardCharsets.ISO_8859_1));
                            output.write(responseBody);
                            output.flush();
                        }
                    }
                    observedPaths.set(paths.toString());
                } catch (Throwable error) {
                    serverFailure.set(error);
                } finally {
                    serverFinished.countDown();
                }
            }, "muon-quickjs-http-server");
            serverThread.setDaemon(true);
            serverThread.start();

            try (BoundService binding = bindService();
                 RuntimeSocket runtime = createRuntime(binding.service, "test-http-fetch")) {
                String root = importModule(runtime, "import", ".");
                JSONObject response = call(
                        runtime,
                        "http-fetch",
                        root,
                        "exerciseHttpAndFetch",
                        new JSONArray().put(port));
                assertTrue(response.toString(), response.getBoolean("ok"));
                JSONObject values = response
                        .getJSONObject("value")
                        .getJSONObject("value");

                assertTrue(values.getBoolean("moduleAlias"));
                JSONObject node = values.getJSONObject("node");
                JSONObject request = node.getJSONObject("request");
                assertTrue(request.getBoolean("isClientRequest"));
                assertEquals("POST", request.getString("method"));
                assertEquals("/node", request.getString("path"));
                assertTrue(request.getBoolean("hasClientHeader"));
                assertEquals("yes", request.getString("laterHeader"));
                assertFalse(request.getBoolean("removedHeader"));
                assertTrue(node.getBoolean("isIncomingMessage"));
                assertEquals(201, node.getInt("statusCode"));
                assertEquals("Created", node.getString("statusMessage"));
                assertEquals("node", node.getString("header"));
                assertTrue(node.getJSONArray("rawHeaders").toString().contains("X-Test"));
                assertEquals("node-response", node.getString("body"));
                assertTrue(node.getBoolean("complete"));
                assertEquals(200, node.getInt("getStatusCode"));
                assertEquals("get-response", node.getString("getBody"));

                JSONObject fetch = values.getJSONObject("fetch");
                assertTrue(fetch.getBoolean("globals"));
                assertTrue(fetch.getBoolean("isResponse"));
                assertEquals(200, fetch.getInt("status"));
                assertEquals("OK", fetch.getString("statusText"));
                assertTrue(fetch.getBoolean("ok"));
                assertFalse(fetch.getBoolean("redirected"));
                assertEquals("http://localhost:" + port + "/fetch",
                        fetch.getString("url"));
                assertEquals("fetch", fetch.getString("header"));
                assertEquals("fetch-body",
                        fetch.getJSONObject("json").getString("received"));
                assertEquals("fetch",
                        fetch.getJSONObject("json").getString("header"));
                assertEquals(200, fetch.getInt("redirectStatus"));
                assertEquals("http://localhost:" + port + "/fetch-target",
                        fetch.getString("redirectUrl"));
                assertTrue(fetch.getBoolean("redirectedResult"));
                assertEquals("redirected", fetch.getString("redirectBody"));
                assertEquals("AbortError", fetch.getString("abortName"));
                assertEquals("stop", fetch.getString("abortReason"));
                assertTrue(fetch.getBoolean("slowBodyUsed"));
            }
            assertTrue(serverFinished.await(30, TimeUnit.SECONDS));
        }
        if (serverFailure.get() != null) {
            throw new AssertionError("The loopback HTTP server failed", serverFailure.get());
        }
        assertEquals(
                "/node,/node-get,/fetch,/redirect,/fetch-target,/slow",
                observedPaths.get());
    }

    @Test
    public void supportsNodeHttpLoopbackServers() throws Exception {
        try (BoundService binding = bindService();
             RuntimeSocket runtime = createRuntime(binding.service, "test-http-server")) {
            String root = importModule(runtime, "import", ".");
            JSONObject response = call(
                    runtime,
                    "http-server",
                    root,
                    "exerciseHttpServer",
                    new JSONArray());
            assertTrue(response.toString(), response.getBoolean("ok"));
            JSONObject values = response
                    .getJSONObject("value")
                    .getJSONObject("value");

            assertTrue(values.getBoolean("moduleAlias"));
            assertTrue(values.getBoolean("isServer"));
            JSONObject address = values.getJSONObject("address");
            assertEquals("127.0.0.1", address.getString("address"));
            assertEquals("IPv4", address.getString("family"));
            assertTrue(address.getInt("port") > 0);
            assertTrue(values.isNull("addressAfterClose"));

            JSONObject responseState = values.getJSONObject("responseState");
            assertTrue(responseState.getBoolean("isServerResponse"));
            assertTrue(responseState.getBoolean("headersSent"));
            assertTrue(responseState.getBoolean("finished"));
            assertEquals(201, responseState.getInt("statusCode"));
            assertEquals("Created", responseState.getString("statusMessage"));
            assertEquals("quickjs", responseState.getString("serverHeader"));
            assertFalse(responseState.getBoolean("removedHeader"));

            JSONObject node = values.getJSONObject("node");
            assertTrue(node.getBoolean("isIncomingMessage"));
            assertEquals(201, node.getInt("statusCode"));
            assertEquals("Created", node.getString("statusMessage"));
            assertEquals("quickjs", node.getString("serverHeader"));
            assertTrue(node.isNull("removedHeader"));
            assertEquals("server-response", node.getString("body"));
            assertTrue(node.getBoolean("complete"));

            JSONObject fetch = values.getJSONObject("fetch");
            assertEquals(200, fetch.getInt("status"));
            JSONObject fetchValue = fetch.getJSONObject("value");
            assertEquals("POST", fetchValue.getString("method"));
            assertEquals("fetch-body", fetchValue.getString("body"));
            assertEquals("quickjs", fetchValue.getString("runtime"));

            JSONArray requests = values.getJSONArray("observedRequests");
            assertEquals(3, requests.length());
            assertEquals("POST", requests.getJSONObject(0).getString("method"));
            assertEquals("/node-server", requests.getJSONObject(0).getString("url"));
            assertEquals("node-body", requests.getJSONObject(0).getString("body"));
            assertTrue(requests.getJSONObject(0).getBoolean("complete"));
            assertEquals("/fetch-server", requests.getJSONObject(1).getString("url"));
            assertEquals("fetch-body", requests.getJSONObject(1).getString("body"));
            assertEquals("/chunked", requests.getJSONObject(2).getString("url"));
            assertEquals("Wikipedia", requests.getJSONObject(2).getString("body"));
            assertEquals("yes", requests.getJSONObject(2).getString("trailer"));

            String chunked = values.getString("chunkedResponse");
            assertTrue(chunked, chunked.startsWith("HTTP/1.1 202 Accepted\r\n"));
            assertTrue(chunked, chunked.toLowerCase(Locale.ROOT)
                    .contains("transfer-encoding: chunked\r\n"));
            assertTrue(chunked, chunked.contains("\r\n15\r\nchunked:Wikipedia:yes\r\n0\r\n\r\n"));
            assertEquals("HPE_INVALID_CONSTANT", values.getString("clientErrorCode"));
            assertTrue(values.getString("invalidResponse"),
                    values.getString("invalidResponse")
                            .startsWith("HTTP/1.1 400 Bad Request\r\n"));
        }
    }

    @Test
    public void enforcesPerRuntimeNetworkResourceLimits() throws Exception {
        AtomicReference<Throwable> serverFailure = new AtomicReference<>();
        CountDownLatch serverFinished = new CountDownLatch(1);
        Socket[] acceptedConnections = new Socket[65];
        try (ServerSocket server = new ServerSocket(
                0,
                acceptedConnections.length,
                InetAddress.getByName("127.0.0.1"))) {
            int closedPort;
            try (ServerSocket closedServer = new ServerSocket(
                    0,
                    1,
                    InetAddress.getByName("127.0.0.1"))) {
                closedPort = closedServer.getLocalPort();
            }
            Thread acceptor = new Thread(() -> {
                try {
                    for (int index = 0; index < acceptedConnections.length; index++) {
                        acceptedConnections[index] = server.accept();
                    }
                } catch (Throwable error) {
                    if (!server.isClosed()) {
                        serverFailure.set(error);
                    }
                } finally {
                    for (Socket connection : acceptedConnections) {
                        if (connection == null) {
                            continue;
                        }
                        try {
                            connection.close();
                        } catch (IOException error) {
                            serverFailure.compareAndSet(null, error);
                        }
                    }
                    serverFinished.countDown();
                }
            }, "muon-quickjs-network-limit-server");
            acceptor.setDaemon(true);
            acceptor.start();

            try (BoundService binding = bindService();
                 RuntimeSocket runtime = createRuntime(binding.service, "test-network-limits")) {
                String root = importModule(runtime, "import", ".");
                JSONObject response = call(
                        runtime,
                        "network-limits",
                        root,
                        "exerciseNetworkResourceLimits",
                        new JSONArray().put(server.getLocalPort()).put(closedPort));
                assertTrue(response.toString(), response.getBoolean("ok"));
                JSONObject values = response
                        .getJSONObject("value")
                        .getJSONObject("value");

                JSONObject dns = values.getJSONObject("dns");
                assertEquals(64, dns.getInt("fulfilled"));
                assertEquals(
                        "[\"ERR_MUON_DNS_OPERATION_LIMIT\"]",
                        dns.getJSONArray("codes").toString());

                JSONObject tcp = values.getJSONObject("tcp");
                assertEquals(64, tcp.getInt("connected"));
                assertEquals(
                        "[\"ERR_MUON_TCP_SOCKET_LIMIT\"]",
                        tcp.getJSONArray("codes").toString());

                JSONObject servers = values.getJSONObject("servers");
                assertEquals(8, servers.getInt("listening"));
                assertEquals(
                        "[\"ERR_MUON_TCP_SERVER_LIMIT\"]",
                        servers.getJSONArray("codes").toString());

                JSONArray httpCodes = values.getJSONObject("http").getJSONArray("codes");
                int refused = 0;
                int limited = 0;
                for (int index = 0; index < httpCodes.length(); index++) {
                    String code = httpCodes.getString(index);
                    if ("ECONNREFUSED".equals(code)) {
                        refused++;
                    } else if ("ERR_HTTP_OPERATION_LIMIT".equals(code)) {
                        limited++;
                    }
                }
                assertEquals(16, refused);
                assertEquals(1, limited);
            }
        }
        assertTrue(serverFinished.await(30, TimeUnit.SECONDS));
        if (serverFailure.get() != null) {
            throw new AssertionError(
                    "The network limit server failed",
                    serverFailure.get());
        }
    }

    @Test
    public void sustainsConcurrentNetworkLoadAcrossIndependentRuntimes()
            throws Exception {
        final int runtimeCount = 4;
        final int iterationCount = 8;
        final int expectedConnectionCount = runtimeCount * iterationCount + 1;
        AtomicReference<Throwable> serverFailure = new AtomicReference<>();
        AtomicReference<Throwable> workerFailure = new AtomicReference<>();
        CountDownLatch tcpServerFinished = new CountDownLatch(1);
        CountDownLatch httpServerFinished = new CountDownLatch(1);
        Set<String> tcpMarkers = Collections.synchronizedSet(new HashSet<>());
        Set<String> httpPaths = Collections.synchronizedSet(new HashSet<>());

        try (ServerSocket tcpServer = new ServerSocket(
                     0,
                     expectedConnectionCount,
                     InetAddress.getByName("127.0.0.1"));
             ServerSocket httpServer = new ServerSocket(
                     0,
                     expectedConnectionCount,
                     InetAddress.getByName("127.0.0.1"))) {
            tcpServer.setSoTimeout(120_000);
            httpServer.setSoTimeout(120_000);

            Thread tcpAcceptor = new Thread(() -> {
                try {
                    for (int index = 0; index < expectedConnectionCount; index++) {
                        try (Socket connection = tcpServer.accept()) {
                            connection.setSoTimeout(30_000);
                            ByteArrayOutputStream received = new ByteArrayOutputStream();
                            byte[] buffer = new byte[256];
                            while (true) {
                                int count = connection.getInputStream().read(buffer);
                                if (count < 0) {
                                    break;
                                }
                                received.write(buffer, 0, count);
                            }
                            String marker = received.toString(StandardCharsets.UTF_8.name());
                            assertTrue(marker, tcpMarkers.add(marker));
                            connection.getOutputStream().write(
                                    ("tcp:" + marker).getBytes(StandardCharsets.UTF_8));
                            connection.getOutputStream().flush();
                        }
                    }
                } catch (Throwable error) {
                    if (!tcpServer.isClosed()) {
                        serverFailure.compareAndSet(null, error);
                    }
                } finally {
                    tcpServerFinished.countDown();
                }
            }, "muon-quickjs-load-tcp-server");
            tcpAcceptor.setDaemon(true);
            tcpAcceptor.start();

            Thread httpAcceptor = new Thread(() -> {
                try {
                    for (int index = 0; index < expectedConnectionCount; index++) {
                        try (Socket connection = httpServer.accept()) {
                            connection.setSoTimeout(30_000);
                            InputStream input = connection.getInputStream();
                            String requestLine = readHttpLine(input);
                            assertNotNull(requestLine);
                            String[] requestParts = requestLine.split(" ");
                            assertEquals(requestLine, 3, requestParts.length);
                            assertEquals("GET", requestParts[0]);
                            assertTrue(requestParts[1], httpPaths.add(requestParts[1]));
                            while (true) {
                                String line = readHttpLine(input);
                                assertNotNull(line);
                                if (line.isEmpty()) {
                                    break;
                                }
                            }
                            byte[] body = ("http:" + requestParts[1])
                                    .getBytes(StandardCharsets.UTF_8);
                            DataOutputStream output = new DataOutputStream(
                                    connection.getOutputStream());
                            output.write(("HTTP/1.1 200 OK\r\n"
                                    + "Content-Type: text/plain; charset=utf-8\r\n"
                                    + "Content-Length: " + body.length + "\r\n"
                                    + "Connection: close\r\n"
                                    + "\r\n")
                                    .getBytes(StandardCharsets.ISO_8859_1));
                            output.write(body);
                            output.flush();
                        }
                    }
                } catch (Throwable error) {
                    if (!httpServer.isClosed()) {
                        serverFailure.compareAndSet(null, error);
                    }
                } finally {
                    httpServerFinished.countDown();
                }
            }, "muon-quickjs-load-http-server");
            httpAcceptor.setDaemon(true);
            httpAcceptor.start();

            try (BoundService binding = bindService()) {
                RuntimeSocket[] runtimes = new RuntimeSocket[runtimeCount];
                String[] moduleIds = new String[runtimeCount];
                JSONObject[] results = new JSONObject[runtimeCount];
                try {
                    for (int index = 0; index < runtimeCount; index++) {
                        runtimes[index] = createRuntime(
                                binding.service,
                                "test-network-load-" + index);
                        moduleIds[index] = importModule(
                                runtimes[index],
                                "import-" + index,
                                ".");
                    }
                    assertEquals(runtimeCount, binding.service.getRuntimeCount());

                    CountDownLatch workersReady = new CountDownLatch(runtimeCount);
                    CountDownLatch startWorkers = new CountDownLatch(1);
                    CountDownLatch workersFinished = new CountDownLatch(runtimeCount);
                    for (int index = 0; index < runtimeCount; index++) {
                        final int runtimeIndex = index;
                        Thread worker = new Thread(() -> {
                            workersReady.countDown();
                            try {
                                startWorkers.await();
                                JSONObject response = call(
                                        runtimes[runtimeIndex],
                                        "load-" + runtimeIndex,
                                        moduleIds[runtimeIndex],
                                        "exerciseConcurrentNetworkLoad",
                                        new JSONArray()
                                                .put(tcpServer.getLocalPort())
                                                .put(httpServer.getLocalPort())
                                                .put("runtime-" + runtimeIndex)
                                                .put(iterationCount));
                                assertTrue(response.toString(), response.getBoolean("ok"));
                                results[runtimeIndex] = response
                                        .getJSONObject("value")
                                        .getJSONObject("value");
                            } catch (Throwable error) {
                                workerFailure.compareAndSet(null, error);
                            } finally {
                                workersFinished.countDown();
                            }
                        }, "muon-quickjs-network-load-" + index);
                        worker.setDaemon(true);
                        worker.start();
                    }
                    assertTrue(workersReady.await(30, TimeUnit.SECONDS));
                    startWorkers.countDown();
                    assertTrue(workersFinished.await(120, TimeUnit.SECONDS));

                    for (int index = 0; index < runtimeCount; index++) {
                        assertTrue(request(
                                runtimes[index],
                                "shutdown-" + index,
                                "shutdown",
                                new JSONObject()).getBoolean("ok"));
                    }
                    if (workerFailure.get() != null) {
                        throw new AssertionError(
                                "A concurrent QuickJS network worker failed",
                                workerFailure.get());
                    }
                    assertEquals(0, binding.service.getRuntimeCount());

                    for (int runtimeIndex = 0;
                         runtimeIndex < runtimeCount;
                         runtimeIndex++) {
                        JSONArray iterations = results[runtimeIndex]
                                .getJSONArray("iterations");
                        assertEquals(iterationCount, iterations.length());
                        for (int iteration = 0;
                             iteration < iterationCount;
                             iteration++) {
                            String marker = "runtime-" + runtimeIndex + "-" + iteration;
                            JSONObject result = iterations.getJSONObject(iteration);
                            assertEquals(4, result.getInt("dnsFamily"));
                            assertEquals("tcp:" + marker, result.getString("tcp"));
                            assertEquals(
                                    "http:/load/" + marker,
                                    result.getString("http"));
                        }
                    }

                    try (RuntimeSocket recovered = createRuntime(
                            binding.service,
                            "test-network-load-recovered")) {
                        String recoveredRoot = importModule(
                                recovered,
                                "recovered-import",
                                ".");
                        JSONObject recoveredResponse = call(
                                recovered,
                                "recovered-load",
                                recoveredRoot,
                                "exerciseConcurrentNetworkLoad",
                                new JSONArray()
                                        .put(tcpServer.getLocalPort())
                                        .put(httpServer.getLocalPort())
                                        .put("recovered")
                                        .put(1));
                        assertTrue(
                                recoveredResponse.toString(),
                                recoveredResponse.getBoolean("ok"));
                        JSONObject recoveredIteration = recoveredResponse
                                .getJSONObject("value")
                                .getJSONObject("value")
                                .getJSONArray("iterations")
                                .getJSONObject(0);
                        assertEquals(4, recoveredIteration.getInt("dnsFamily"));
                        assertEquals("tcp:recovered-0", recoveredIteration.getString("tcp"));
                        assertEquals(
                                "http:/load/recovered-0",
                                recoveredIteration.getString("http"));
                        assertTrue(request(
                                recovered,
                                "shutdown-recovered",
                                "shutdown",
                                new JSONObject()).getBoolean("ok"));
                    }
                    assertEquals(0, binding.service.getRuntimeCount());
                } finally {
                    binding.service.shutdownAll();
                    for (RuntimeSocket runtime : runtimes) {
                        if (runtime != null) {
                            runtime.close();
                        }
                    }
                }
            }

            assertTrue(tcpServerFinished.await(30, TimeUnit.SECONDS));
            assertTrue(httpServerFinished.await(30, TimeUnit.SECONDS));
            if (serverFailure.get() != null) {
                throw new AssertionError(
                        "A concurrent QuickJS network server failed",
                        serverFailure.get());
            }
            assertEquals(expectedConnectionCount, tcpMarkers.size());
            assertEquals(expectedConnectionCount, httpPaths.size());
        }
    }

    @Test
    public void supportsNodeHttpsWithCertificateValidation() throws Exception {
        String certificatePem = readTestAsset("localhost-cert.pem");
        String privateKeyPem = readTestAsset("localhost-key.pem");
        SSLContext tlsContext = createLoopbackTlsContext(certificatePem, privateKeyPem);
        AtomicReference<Throwable> serverFailure = new AtomicReference<>();
        AtomicReference<String> observedPath = new AtomicReference<>();
        CountDownLatch serverFinished = new CountDownLatch(1);
        try (SSLServerSocket server = (SSLServerSocket) tlsContext
                .getServerSocketFactory()
                .createServerSocket(
                        0,
                        2,
                        InetAddress.getByName("127.0.0.1"))) {
            server.setSoTimeout(30_000);
            int port = server.getLocalPort();
            Thread serverThread = new Thread(() -> {
                try {
                    for (int index = 0; index < 2; index++) {
                        try (Socket connection = server.accept()) {
                            connection.setSoTimeout(30_000);
                            InputStream input = connection.getInputStream();
                            String requestLine;
                            try {
                                requestLine = readHttpLine(input);
                            } catch (SSLException error) {
                                if (index != 0) {
                                    throw error;
                                }
                                continue;
                            }
                            if (index == 0) {
                                throw new AssertionError(
                                        "The untrusted TLS certificate was accepted");
                            }
                            assertEquals("GET /secure HTTP/1.1", requestLine);
                            Map<String, String> headers = new HashMap<>();
                            while (true) {
                                String line = readHttpLine(input);
                                if (line == null) {
                                    throw new EOFException("HTTPS headers ended early");
                                }
                                if (line.isEmpty()) {
                                    break;
                                }
                                int separator = line.indexOf(':');
                                assertTrue(line, separator > 0);
                                headers.put(
                                        line.substring(0, separator)
                                                .trim()
                                                .toLowerCase(Locale.ROOT),
                                        line.substring(separator + 1).trim());
                            }
                            assertEquals("yes", headers.get("x-secure"));
                            observedPath.set("/secure");
                            byte[] responseBody = "secure-response"
                                    .getBytes(StandardCharsets.UTF_8);
                            DataOutputStream output = new DataOutputStream(
                                    connection.getOutputStream());
                            output.write(("HTTP/1.1 200 OK\r\n"
                                    + "X-Secure: verified\r\n"
                                    + "Content-Length: "
                                    + responseBody.length
                                    + "\r\n"
                                    + "Connection: close\r\n"
                                    + "\r\n")
                                    .getBytes(StandardCharsets.ISO_8859_1));
                            output.write(responseBody);
                            output.flush();
                        }
                    }
                } catch (Throwable error) {
                    serverFailure.set(error);
                } finally {
                    serverFinished.countDown();
                }
            }, "muon-quickjs-https-server");
            serverThread.setDaemon(true);
            serverThread.start();

            try (BoundService binding = bindService();
                 RuntimeSocket runtime = createRuntime(binding.service, "test-https")) {
                String root = importModule(runtime, "import", ".");
                JSONObject response = call(
                        runtime,
                        "https",
                        root,
                        "exerciseHttps",
                        new JSONArray().put(port).put(certificatePem));
                assertTrue(response.toString(), response.getBoolean("ok"));
                JSONObject values = response
                        .getJSONObject("value")
                        .getJSONObject("value");
                assertTrue(values.getBoolean("moduleAlias"));
                assertTrue(values.getBoolean("agent"));
                assertTrue(values.getBoolean("isClientRequest"));
                assertTrue(values.getBoolean("isIncomingMessage"));
                assertEquals("https:", values.getString("protocol"));
                assertEquals(200, values.getInt("statusCode"));
                assertEquals("verified", values.getString("header"));
                assertEquals("secure-response", values.getString("body"));
                assertTrue(values.getBoolean("complete"));
                assertEquals("ERR_TLS_HANDSHAKE", values.getString("rejectionCode"));
                assertEquals(
                        "https://localhost:" + port + "/untrusted",
                        values.getString("rejectionUrl"));
                assertEquals("ERR_NOT_SUPPORTED", values.getString("unsafeOptionCode"));
            }
            assertTrue(serverFinished.await(30, TimeUnit.SECONDS));
        }
        if (serverFailure.get() != null) {
            throw new AssertionError("The loopback HTTPS server failed", serverFailure.get());
        }
        assertEquals("/secure", observedPath.get());
    }

    @Test
    public void isolatesAndClosesTcpSocketsForEachRuntime() throws Exception {
        AtomicReference<Throwable> serverFailure = new AtomicReference<>();
        CountDownLatch openedConnections = new CountDownLatch(2);
        CountDownLatch closedConnections = new CountDownLatch(2);
        StringBuilder markers = new StringBuilder();
        try (ServerSocket server = new ServerSocket(
                0,
                2,
                InetAddress.getByName("127.0.0.1"))) {
            server.setSoTimeout(30_000);
            Thread acceptor = new Thread(() -> {
                try {
                    for (int index = 0; index < 2; index++) {
                        Socket connection = server.accept();
                        connection.setSoTimeout(30_000);
                        Thread reader = new Thread(() -> {
                            boolean opened = false;
                            try (Socket active = connection) {
                                byte[] marker = new byte[5];
                                int offset = 0;
                                while (offset < marker.length) {
                                    int count = active.getInputStream().read(
                                            marker,
                                            offset,
                                            marker.length - offset);
                                    if (count < 0) {
                                        throw new AssertionError(
                                                "TCP socket closed before its marker");
                                    }
                                    offset += count;
                                }
                                synchronized (markers) {
                                    markers.append(new String(
                                            marker,
                                            StandardCharsets.UTF_8));
                                }
                                opened = true;
                                openedConnections.countDown();
                                while (active.getInputStream().read() >= 0) {
                                    // Runtime shutdown must eventually close this socket.
                                }
                            } catch (Throwable error) {
                                serverFailure.compareAndSet(null, error);
                            } finally {
                                if (!opened) {
                                    openedConnections.countDown();
                                }
                                closedConnections.countDown();
                            }
                        }, "muon-quickjs-retained-tcp-reader-" + index);
                        reader.setDaemon(true);
                        reader.start();
                    }
                } catch (Throwable error) {
                    serverFailure.compareAndSet(null, error);
                    while (openedConnections.getCount() > 0) {
                        openedConnections.countDown();
                    }
                    while (closedConnections.getCount() > 0) {
                        closedConnections.countDown();
                    }
                }
            }, "muon-quickjs-retained-tcp-acceptor");
            acceptor.setDaemon(true);
            acceptor.start();

            try (BoundService binding = bindService();
                 RuntimeSocket first = createRuntime(binding.service, "test-tcp-first");
                 RuntimeSocket second = createRuntime(binding.service, "test-tcp-second")) {
                String firstRoot = importModule(first, "first-import", ".");
                String secondRoot = importModule(second, "second-import", ".");
                JSONObject firstOpened = call(
                        first,
                        "first-open",
                        firstRoot,
                        "retainTcpConnection",
                        new JSONArray().put(server.getLocalPort()).put("one-1"));
                JSONObject secondOpened = call(
                        second,
                        "second-open",
                        secondRoot,
                        "retainTcpConnection",
                        new JSONArray().put(server.getLocalPort()).put("two-2"));
                assertTrue(firstOpened.toString(), firstOpened.getBoolean("ok"));
                assertTrue(secondOpened.toString(), secondOpened.getBoolean("ok"));
                assertTrue(openedConnections.await(30, TimeUnit.SECONDS));
                if (serverFailure.get() != null) {
                    throw new AssertionError(
                            "The retained TCP server failed",
                            serverFailure.get());
                }

                assertTrue(request(
                        first,
                        "shutdown-first",
                        "shutdown",
                        new JSONObject()).getBoolean("ok"));
                JSONObject surviving = call(
                        second,
                        "second-state",
                        secondRoot,
                        "retainedTcpConnectionState",
                        new JSONArray());
                assertTrue(surviving.toString(), surviving.getBoolean("ok"));
                JSONObject survivingState = surviving
                        .getJSONObject("value")
                        .getJSONObject("value");
                assertEquals("open", survivingState.getString("readyState"));
                assertFalse(survivingState.getBoolean("destroyed"));
                assertTrue(request(
                        second,
                        "shutdown-second",
                        "shutdown",
                        new JSONObject()).getBoolean("ok"));
            }
            assertTrue(closedConnections.await(30, TimeUnit.SECONDS));
        }
        if (serverFailure.get() != null) {
            throw new AssertionError("The retained TCP server failed", serverFailure.get());
        }
        synchronized (markers) {
            assertEquals("one-1two-2", markers.toString());
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

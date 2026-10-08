/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

package dev.muon.runtime;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;

import android.app.UiAutomation;
import android.content.Context;
import android.content.Intent;

import androidx.test.core.app.ActivityScenario;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;

import org.json.JSONObject;
import org.junit.Test;
import org.junit.runner.RunWith;

import java.io.BufferedReader;
import java.io.BufferedWriter;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.NetworkInterface;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Enumeration;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;

@RunWith(AndroidJUnit4.class)
public final class MuonNetworkFilterTest {
    @Test
    public void allowsExternalMainFrameThroughTheNormalWebViewNetwork() throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        UiAutomation automation = InstrumentationRegistry.getInstrumentation().getUiAutomation();
        automation.grantRuntimePermission(
                context.getPackageName(), "android.permission.ACCESS_LOCAL_NETWORK");

        try (NetworkTestServer server = new NetworkTestServer();
             ActivityScenario<MuonActivity> scenario = ActivityScenario.launch(MuonActivity.class)) {
            AtomicReference<MuonActivity> activityReference = new AtomicReference<>();
            scenario.onActivity(activityReference::set);
            MuonActivity activity = activityReference.get();
            assertNotNull(activity);
            assertTrue(activity.awaitPageReadyForTest(30, TimeUnit.SECONDS));
            activity.clearFinishedPageUrlsForTest();

            String url = server.url("/external-main-frame");
            scenario.onActivity(current -> current.getWebViewForTest().loadUrl(url));

            assertEquals(
                    url,
                    activity.awaitFinishedPageUrlForTest(30, TimeUnit.SECONDS));
            assertTrue(server.wasRequested("/external-main-frame"));
        }
    }

    @Test
    public void returnsLocalNotFoundForMissingTrustedAssets() throws Exception {
        try (ActivityScenario<MuonActivity> scenario = ActivityScenario.launch(MuonActivity.class)) {
            AtomicReference<MuonActivity> activityReference = new AtomicReference<>();
            scenario.onActivity(activityReference::set);
            MuonActivity activity = activityReference.get();
            assertNotNull(activity);
            assertTrue(activity.awaitPageReadyForTest(30, TimeUnit.SECONDS));
            activity.clearMainFrameHttpStatusesForTest();

            scenario.onActivity(current -> current.getWebViewForTest().loadUrl(
                    MuonActivity.TRUSTED_ORIGIN + "/missing-asset.html"));

            assertEquals(
                    Integer.valueOf(404),
                    activity.awaitMainFrameHttpStatusForTest(30, TimeUnit.SECONDS));
        }
    }

    @Test
    public void confirmsWebViewRequestCallbackCoverageIsNotCefCompatible()
            throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        UiAutomation automation = InstrumentationRegistry.getInstrumentation().getUiAutomation();
        automation.grantRuntimePermission(
                context.getPackageName(), "android.permission.ACCESS_LOCAL_NETWORK");

        try (NetworkTestServer server = new NetworkTestServer()) {
            String pageUrl = server.url("/probe");
            Intent intent = new Intent(
                    context, MuonNetworkCapabilityProbeActivity.class);
            intent.putExtra(MuonNetworkCapabilityProbeActivity.EXTRA_PAGE_URL, pageUrl);
            try (ActivityScenario<MuonNetworkCapabilityProbeActivity> scenario =
                         ActivityScenario.launch(intent)) {
                AtomicReference<MuonNetworkCapabilityProbeActivity> activityReference =
                        new AtomicReference<>();
                scenario.onActivity(activityReference::set);
                MuonNetworkCapabilityProbeActivity activity = activityReference.get();
                assertNotNull(activity);
                assertTrue(activity.awaitPageReadyForTest(30, TimeUnit.SECONDS));
                activity.clearRequestObservationsForTest();

                String origin = server.loopbackOrigin();
                String script = """
                        void (async () => {
                          const loadFrame = (url) => new Promise((resolve) => {
                            const frame = document.createElement('iframe');
                            frame.addEventListener('load', () => resolve(frame), { once: true });
                            frame.addEventListener('error', () => resolve(frame), { once: true });
                            frame.src = url;
                            document.body.append(frame);
                          });
                          const loadXhr = (url) => new Promise((resolve, reject) => {
                            const request = new XMLHttpRequest();
                            request.addEventListener('load', resolve, { once: true });
                            request.addEventListener('error', reject, { once: true });
                            request.open('GET', url);
                            request.send();
                          });
                          await loadFrame(%1$s + '/iframe');
                          await fetch(%1$s + '/fetch');
                          await loadXhr(%1$s + '/xhr');
                          await fetch(%1$s + '/redirect');
                          await new Promise((resolve) => {
                            const socket = new WebSocket(%2$s + '/websocket');
                            socket.addEventListener('open', () => {
                              socket.close();
                              resolve();
                            }, { once: true });
                            socket.addEventListener('error', resolve, { once: true });
                          });
                          const blobUrl = URL.createObjectURL(new Blob([
                            '<body data-muon-network-probe="loaded">'
                          ], { type: 'text/html' }));
                          const blobFrame = await loadFrame(blobUrl);
                          const blob = blobFrame.contentDocument?.body?.dataset
                            .muonNetworkProbe === 'loaded' ? 'loaded' : 'failed';
                          blobFrame.remove();
                          URL.revokeObjectURL(blobUrl);

                          const registration = await navigator.serviceWorker.register(
                            %1$s + '/service-worker.js'
                          );
                          await navigator.serviceWorker.ready;
                          await new Promise((resolve) => {
                            const channel = new MessageChannel();
                            channel.port1.addEventListener('message', resolve, { once: true });
                            channel.port1.start();
                            registration.active.postMessage('fetch', [channel.port2]);
                          });
                          muonNetworkProbe.postMessage(JSON.stringify({ blob }));
                        })();
                        """.formatted(
                        JSONObject.quote(origin),
                        JSONObject.quote(server.loopbackWebSocketOrigin()));
                scenario.onActivity(current -> current.getWebViewForTest()
                        .evaluateJavascript(script, null));

                String message = activity.awaitMessageForTest(30, TimeUnit.SECONDS);
                assertNotNull(message);
                assertEquals("loaded", new JSONObject(message).getString("blob"));

                assertTrue(server.wasRequested("/iframe"));
                assertTrue(server.wasRequested("/fetch"));
                assertTrue(server.wasRequested("/xhr"));
                assertTrue(server.wasRequested("/websocket"));
                assertTrue(server.wasRequested("/redirect"));
                assertTrue(server.wasRequested("/redirect-target"));
                assertTrue(server.wasRequested("/service-worker.js"));
                assertTrue(server.wasRequested("/service-worker-fetch"));

                List<MuonNetworkCapabilityProbeActivity.RequestObservation> webViewRequests =
                        activity.getWebViewRequestsForTest();
                assertTrue(webViewRequests.stream().anyMatch(request ->
                        request.getUrl().equals(server.url("/iframe"))
                                && !request.isMainFrame()));
                assertTrue(webViewRequests.stream().anyMatch(request ->
                        request.getUrl().equals(server.url("/fetch"))));
                assertTrue(webViewRequests.stream().anyMatch(request ->
                        request.getUrl().equals(server.url("/xhr"))));
                assertTrue(webViewRequests.stream().anyMatch(request ->
                        request.getUrl().equals(server.url("/redirect"))
                                && !request.isRedirect()));
                assertFalse(webViewRequests.stream().anyMatch(request ->
                        request.getUrl().equals(server.url("/redirect-target"))));
                assertFalse(webViewRequests.stream().anyMatch(request ->
                        request.getUrl().equals(server.url("/websocket"))));
                assertFalse(webViewRequests.stream().anyMatch(request ->
                        request.getUrl().startsWith("blob:")));
                assertFalse(webViewRequests.stream().anyMatch(request ->
                        request.getUrl().equals(server.url("/service-worker-fetch"))));

                List<MuonNetworkCapabilityProbeActivity.RequestObservation>
                        serviceWorkerRequests = activity.getServiceWorkerRequestsForTest();
                assertTrue(serviceWorkerRequests.stream().anyMatch(request ->
                        request.getUrl().equals(server.url("/service-worker-fetch"))));
            }
        }
    }

    private static final class NetworkTestServer implements AutoCloseable {
        private final ServerSocket serverSocket;
        private final List<String> requestedPaths =
                Collections.synchronizedList(new ArrayList<>());
        private final AtomicBoolean closed = new AtomicBoolean(false);
        private final CountDownLatch started = new CountDownLatch(1);
        private final Thread acceptThread;
        private final String localNetworkHost;

        private NetworkTestServer() throws Exception {
            localNetworkHost = findLocalNetworkHost();
            serverSocket = new ServerSocket(0, 16);
            acceptThread = new Thread(this::acceptRequests, "muon-network-test-server");
            acceptThread.start();
            assertTrue(started.await(30, TimeUnit.SECONDS));
        }

        private String url(String path) {
            return loopbackOrigin() + path;
        }

        private String loopbackOrigin() {
            return "http://localhost:" + serverSocket.getLocalPort();
        }

        private String loopbackWebSocketOrigin() {
            return "ws://localhost:" + serverSocket.getLocalPort();
        }

        private String localNetworkOrigin() {
            return "http://" + localNetworkHost + ":" + serverSocket.getLocalPort();
        }

        private boolean wasRequested(String path) {
            return requestedPaths.contains(path);
        }

        private static String findLocalNetworkHost() throws Exception {
            Enumeration<NetworkInterface> interfaces = NetworkInterface.getNetworkInterfaces();
            while (interfaces.hasMoreElements()) {
                NetworkInterface networkInterface = interfaces.nextElement();
                if (!networkInterface.isUp() || networkInterface.isLoopback()) {
                    continue;
                }
                Enumeration<InetAddress> addresses = networkInterface.getInetAddresses();
                while (addresses.hasMoreElements()) {
                    InetAddress address = addresses.nextElement();
                    if (address instanceof Inet4Address && address.isSiteLocalAddress()) {
                        return address.getHostAddress();
                    }
                }
            }
            throw new IllegalStateException("No local IPv4 address is available");
        }

        private void acceptRequests() {
            started.countDown();
            while (!closed.get()) {
                try {
                    Socket socket = serverSocket.accept();
                    handleRequest(socket);
                } catch (IOException error) {
                    if (!closed.get()) {
                        throw new IllegalStateException("Network test server failed", error);
                    }
                }
            }
        }

        private void handleRequest(Socket socket) throws IOException {
            try (socket;
                 BufferedReader reader = new BufferedReader(new InputStreamReader(
                         socket.getInputStream(), StandardCharsets.US_ASCII));
                 BufferedWriter writer = new BufferedWriter(new OutputStreamWriter(
                         socket.getOutputStream(), StandardCharsets.US_ASCII))) {
                String requestLine = reader.readLine();
                if (requestLine == null) {
                    return;
                }
                String[] parts = requestLine.split(" ");
                if (parts.length >= 2) {
                    requestedPaths.add(parts[1]);
                }
                String line;
                do {
                    line = reader.readLine();
                } while (line != null && !line.isEmpty());

                String path = parts.length >= 2 ? parts[1] : "/";
                if (path.equals("/redirect")) {
                    writer.write("HTTP/1.1 302 Found\r\n");
                    writer.write("Location: /redirect-target\r\n");
                    writer.write("Content-Length: 0\r\n");
                    writer.write("Connection: close\r\n\r\n");
                    writer.flush();
                    return;
                }

                String contentType = "text/html; charset=utf-8";
                String body = "<!doctype html><title>network reached</title>";
                if (path.equals("/service-worker.js")) {
                    contentType = "application/javascript; charset=utf-8";
                    body = """
                            self.addEventListener('install', (event) => {
                              event.waitUntil(self.skipWaiting());
                            });
                            self.addEventListener('activate', (event) => {
                              event.waitUntil(self.clients.claim());
                            });
                            self.addEventListener('message', (event) => {
                              event.waitUntil((async () => {
                                await fetch('/service-worker-fetch');
                                event.ports[0].postMessage('completed');
                              })());
                            });
                            """;
                }
                writer.write("HTTP/1.1 200 OK\r\n");
                writer.write("Content-Type: " + contentType + "\r\n");
                writer.write("Cache-Control: no-store\r\n");
                writer.write("Service-Worker-Allowed: /\r\n");
                writer.write("Content-Length: "
                        + body.getBytes(StandardCharsets.UTF_8).length + "\r\n");
                writer.write("Connection: close\r\n\r\n");
                writer.write(body);
                writer.flush();
            }
        }

        @Override
        public void close() throws Exception {
            closed.set(true);
            serverSocket.close();
            acceptThread.join(TimeUnit.SECONDS.toMillis(30));
            assertFalse(acceptThread.isAlive());
        }
    }
}

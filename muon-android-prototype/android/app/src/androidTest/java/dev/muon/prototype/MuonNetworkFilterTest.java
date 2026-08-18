/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

package dev.muon.prototype;

import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;

import android.app.UiAutomation;
import android.content.Context;

import androidx.test.core.app.ActivityScenario;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;

import org.junit.Test;
import org.junit.runner.RunWith;

import java.io.BufferedReader;
import java.io.BufferedWriter;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;

@RunWith(AndroidJUnit4.class)
public final class MuonNetworkFilterTest {
    @Test
    public void blocksUnconfiguredMainFrameBeforeItReachesTheNetwork() throws Exception {
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

            String url = server.url("/blocked-main-frame");
            scenario.onActivity(current -> current.getWebViewForTest().loadUrl(url));

            assertNotNull(activity.awaitFinishedPageUrlForTest(30, TimeUnit.SECONDS));
            assertFalse(server.wasRequested("/blocked-main-frame"));
        }
    }

    private static final class NetworkTestServer implements AutoCloseable {
        private final ServerSocket serverSocket;
        private final List<String> requestedPaths =
                Collections.synchronizedList(new ArrayList<>());
        private final AtomicBoolean closed = new AtomicBoolean(false);
        private final CountDownLatch started = new CountDownLatch(1);
        private final Thread acceptThread;

        private NetworkTestServer() throws Exception {
            serverSocket = new ServerSocket(0, 16, InetAddress.getLoopbackAddress());
            acceptThread = new Thread(this::acceptRequests, "muon-network-test-server");
            acceptThread.start();
            assertTrue(started.await(30, TimeUnit.SECONDS));
        }

        private String url(String path) {
            return "http://localhost:" + serverSocket.getLocalPort() + path;
        }

        private boolean wasRequested(String path) {
            return requestedPaths.contains(path);
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

                String body = "<!doctype html><title>network reached</title>";
                writer.write("HTTP/1.1 200 OK\r\n");
                writer.write("Content-Type: text/html; charset=utf-8\r\n");
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

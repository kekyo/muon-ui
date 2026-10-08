/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

package dev.muon.runtime;

import android.app.Activity;
import android.content.pm.ApplicationInfo;
import android.net.Uri;
import android.os.Bundle;
import android.webkit.WebView;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.webkit.JavaScriptReplyProxy;
import androidx.webkit.WebMessageCompat;
import androidx.webkit.WebViewCompat;

import java.util.Collections;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;

/** Hosts the Android WebView backend prototype. */
public final class MuonActivity extends Activity {
    static final String TRUSTED_ORIGIN = "https://main.asset.muon.invalid";
    private static final String JAVASCRIPT_RUNTIME_OBJECT_NAME =
            "muonAndroidJavaScriptRuntime";
    private static final String TEST_OBJECT_NAME = "muonAndroidTest";

    private final CountDownLatch pageReady = new CountDownLatch(1);
    private final CountDownLatch startupFailureReady = new CountDownLatch(1);
    private final CountDownLatch destroyed = new CountDownLatch(1);
    private final LinkedBlockingQueue<String> testMessages = new LinkedBlockingQueue<>();
    private final LinkedBlockingQueue<String> finishedPageUrls = new LinkedBlockingQueue<>();
    private final LinkedBlockingQueue<Integer> mainFrameHttpStatuses =
            new LinkedBlockingQueue<>();
    private WebView webView;
    private MuonRpcBridge rpcBridge;
    private MuonJavaScriptRuntimeBridge javaScriptRuntimeBridge;
    private MuonWebViewHost host;
    private boolean testBridgeInstalled;
    @Nullable private String startupFailure;

    @Override
    protected void onCreate(@Nullable Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        host = new MuonWebViewHost(this, new MuonWebViewHost.Listener() {
            @Override public void beforePageLoad(@NonNull WebView view, @NonNull MuonRpcBridge bridge) {
                webView = view;
                rpcBridge = bridge;
                javaScriptRuntimeBridge = new MuonJavaScriptRuntimeBridge(MuonActivity.this);
                WebViewCompat.addWebMessageListener(view, JAVASCRIPT_RUNTIME_OBJECT_NAME,
                        Collections.singleton(TRUSTED_ORIGIN), javaScriptRuntimeBridge);
                if ((getApplicationInfo().flags & ApplicationInfo.FLAG_DEBUGGABLE) != 0) {
                    WebViewCompat.addWebMessageListener(view, TEST_OBJECT_NAME,
                            Collections.singleton(TRUSTED_ORIGIN), MuonActivity.this::receiveTestMessage);
                    testBridgeInstalled = true;
                }
            }
            @Override public void pageFinished(@NonNull String url) {
                finishedPageUrls.add(url);
                if (MuonAppConfig.load(MuonActivity.this).startPage.equals(url)) {
                    pageReady.countDown();
                }
            }
            @Override public void mainFrameHttpError(int status) { mainFrameHttpStatuses.add(status); }
            @Override public void startupFailed(@NonNull String diagnostic) {
                startupFailure = diagnostic;
                startupFailureReady.countDown();
            }
        });
        host.start("");
    }

    private void receiveTestMessage(
            @NonNull WebView view,
            @NonNull WebMessageCompat message,
            @NonNull Uri sourceOrigin,
            boolean isMainFrame,
            @NonNull JavaScriptReplyProxy replyProxy) {
        if (!isMainFrame
                || !TRUSTED_ORIGIN.equals(sourceOrigin.toString())
                || message.getType() != WebMessageCompat.TYPE_STRING) {
            return;
        }
        String data = message.getData();
        if (data != null) {
            testMessages.add(data);
        }
    }

    WebView getWebViewForTest() {
        return webView;
    }

    boolean awaitPageReadyForTest(long timeout, @NonNull TimeUnit unit)
            throws InterruptedException {
        return pageReady.await(timeout, unit);
    }

    boolean awaitStartupFailureForTest(long timeout, @NonNull TimeUnit unit)
            throws InterruptedException {
        return startupFailureReady.await(timeout, unit);
    }

    @NonNull String getStartupFailureForTest() {
        return startupFailure == null ? "" : startupFailure;
    }

    boolean wasPageLoadStartedForTest() {
        return host.wasPageLoadStarted();
    }

    @Nullable String awaitTestMessage(long timeout, @NonNull TimeUnit unit)
            throws InterruptedException {
        return testMessages.poll(timeout, unit);
    }

    void clearTestMessages() {
        testMessages.clear();
    }

    @Nullable String awaitFinishedPageUrlForTest(long timeout, @NonNull TimeUnit unit)
            throws InterruptedException {
        return finishedPageUrls.poll(timeout, unit);
    }

    void clearFinishedPageUrlsForTest() {
        finishedPageUrls.clear();
    }

    @Nullable Integer awaitMainFrameHttpStatusForTest(
            long timeout,
            @NonNull TimeUnit unit) throws InterruptedException {
        return mainFrameHttpStatuses.poll(timeout, unit);
    }

    void clearMainFrameHttpStatusesForTest() {
        mainFrameHttpStatuses.clear();
    }

    int getNativePendingCallCountForTest() {
        return rpcBridge == null ? 0 : rpcBridge.getNativePendingCallCount();
    }

    @NonNull MuonRpcBridge getRpcBridgeForTest() {
        return rpcBridge;
    }

    boolean isFullscreenForTest() {
        return rpcBridge != null && rpcBridge.isFullscreenForTest();
    }

    float getManagedZoomFactorForTest() {
        return rpcBridge == null ? 1.0f : rpcBridge.getManagedZoomFactorForTest();
    }

    int getActiveFilesystemWatchCountForTest() {
        return rpcBridge == null
                ? 0
                : rpcBridge.getActiveFilesystemWatchCountForTest();
    }

    void startNativeRuntimeProbeForTest() {
        if (rpcBridge == null) {
            throw new IllegalStateException("The native runtime session is closed");
        }
        rpcBridge.startNativeRuntimeProbeForTest();
    }

    void releaseNativeContextForTest() {
        if (rpcBridge != null) {
            rpcBridge.releaseNativeContextForTest();
        }
    }

    void clearNativeRuntimeProbeEventsForTest() {
        if (rpcBridge != null) {
            rpcBridge.clearNativeRuntimeProbeEventsForTest();
        }
    }

    @Nullable String awaitNativeRuntimeProbeResultForTest(
            long timeout,
            @NonNull TimeUnit unit) throws InterruptedException {
        return rpcBridge == null
                ? null
                : rpcBridge.awaitNativeRuntimeProbeResultForTest(timeout, unit);
    }

    @Nullable String awaitNativeRuntimeProbeSettlementForTest(
            long timeout,
            @NonNull TimeUnit unit) throws InterruptedException {
        return rpcBridge == null
                ? null
                : rpcBridge.awaitNativeRuntimeProbeSettlementForTest(timeout, unit);
    }

    int getNativeRuntimeProbeResultCountForTest() {
        return rpcBridge == null ? 0 : rpcBridge.getNativeRuntimeProbeResultCountForTest();
    }

    @NonNull String getNativeRuntimeDiagnosticsForTest() {
        if (rpcBridge == null) {
            throw new IllegalStateException("The native runtime session is closed");
        }
        return rpcBridge.getNativeRuntimeDiagnosticsForTest();
    }

    boolean awaitDestroyedForTest(long timeout, @NonNull TimeUnit unit)
            throws InterruptedException {
        return destroyed.await(timeout, unit);
    }

    @Override
    protected void onDestroy() {
        if (webView != null) {
            WebViewCompat.removeWebMessageListener(
                    webView,
                    JAVASCRIPT_RUNTIME_OBJECT_NAME);
            if (testBridgeInstalled) {
                WebViewCompat.removeWebMessageListener(webView, TEST_OBJECT_NAME);
                testBridgeInstalled = false;
            }
        }
        if (javaScriptRuntimeBridge != null) {
            javaScriptRuntimeBridge.close();
            javaScriptRuntimeBridge = null;
        }
        host.close(isChangingConfigurations());
        host = null;
        rpcBridge = null;
        webView = null;
        super.onDestroy();
        destroyed.countDown();
    }
}

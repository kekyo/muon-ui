/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

package dev.muon.prototype;

import android.app.Activity;
import android.content.pm.ApplicationInfo;
import android.net.Uri;
import android.os.Bundle;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.webkit.JavaScriptReplyProxy;
import androidx.webkit.WebMessageCompat;
import androidx.webkit.WebViewCompat;
import androidx.webkit.WebViewFeature;

import java.util.Collections;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;

/** Hosts the Android WebView backend prototype. */
public final class MuonActivity extends Activity {
    static final String TRUSTED_ORIGIN = "https://main.asset.muon.invalid";
    private static final String APP_URL = TRUSTED_ORIGIN + "/index.html";
    private static final String RPC_OBJECT_NAME = "muonAndroidRpc";
    private static final String TEST_OBJECT_NAME = "muonAndroidTest";

    static {
        System.loadLibrary("muon_android_rpc");
    }

    private final CountDownLatch pageReady = new CountDownLatch(1);
    private final CountDownLatch destroyed = new CountDownLatch(1);
    private final LinkedBlockingQueue<String> testMessages = new LinkedBlockingQueue<>();
    private final LinkedBlockingQueue<String> finishedPageUrls = new LinkedBlockingQueue<>();
    private final LinkedBlockingQueue<Integer> mainFrameHttpStatuses =
            new LinkedBlockingQueue<>();
    private WebView webView;
    private MuonRpcBridge rpcBridge;
    private MuonAssetRequestHandler assetRequestHandler;
    private boolean testBridgeInstalled;

    @Override
    protected void onCreate(@Nullable Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        if (!WebViewFeature.isFeatureSupported(WebViewFeature.WEB_MESSAGE_LISTENER)) {
            throw new IllegalStateException("WebView message listeners are unavailable");
        }
        if (!WebViewFeature.isFeatureSupported(WebViewFeature.WEB_MESSAGE_ARRAY_BUFFER)) {
            throw new IllegalStateException("WebView ArrayBuffer messages are unavailable");
        }

        assetRequestHandler = new MuonAssetRequestHandler(this);
        assetRequestHandler.configureServiceWorkers();
        webView = new WebView(this);
        WebSettings settings = webView.getSettings();
        assetRequestHandler.configureWebViewSettings(settings);
        webView.setWebViewClient(new WebViewClient() {
            @Override
            public WebResourceResponse shouldInterceptRequest(
                    WebView view,
                    WebResourceRequest request) {
                return assetRequestHandler.shouldInterceptRequest(request);
            }

            @Override
            public void onReceivedHttpError(
                    WebView view,
                    WebResourceRequest request,
                    WebResourceResponse errorResponse) {
                if (request.isForMainFrame()) {
                    mainFrameHttpStatuses.add(errorResponse.getStatusCode());
                }
            }

            @Override
            public void onPageFinished(WebView view, String url) {
                finishedPageUrls.add(url);
                if (APP_URL.equals(url)) {
                    pageReady.countDown();
                }
            }
        });

        rpcBridge = new MuonRpcBridge(this, webView);
        WebViewCompat.addWebMessageListener(
                webView,
                RPC_OBJECT_NAME,
                Collections.singleton(TRUSTED_ORIGIN),
                rpcBridge);
        if ((getApplicationInfo().flags & ApplicationInfo.FLAG_DEBUGGABLE) != 0) {
            WebViewCompat.addWebMessageListener(
                    webView,
                    TEST_OBJECT_NAME,
                    Collections.singleton(TRUSTED_ORIGIN),
                    this::receiveTestMessage);
            testBridgeInstalled = true;
        }

        setContentView(webView);
        webView.loadUrl(APP_URL);
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
            WebViewCompat.removeWebMessageListener(webView, RPC_OBJECT_NAME);
            if (testBridgeInstalled) {
                WebViewCompat.removeWebMessageListener(webView, TEST_OBJECT_NAME);
                testBridgeInstalled = false;
            }
        }
        if (rpcBridge != null) {
            rpcBridge.close(isChangingConfigurations());
            rpcBridge = null;
        }
        if (assetRequestHandler != null) {
            assetRequestHandler.close();
            assetRequestHandler = null;
        }
        if (webView != null) {
            webView.destroy();
            webView = null;
        }
        super.onDestroy();
        destroyed.countDown();
    }
}

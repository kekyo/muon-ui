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
import android.util.Log;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.TextView;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.webkit.JavaScriptReplyProxy;
import androidx.webkit.ScriptHandler;
import androidx.webkit.WebMessageCompat;
import androidx.webkit.WebViewCompat;
import androidx.webkit.WebViewFeature;

import java.util.Collections;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;

/** Hosts the Android WebView backend prototype. */
public final class MuonActivity extends Activity implements MuonRpcBridge.Listener {
    static final String TRUSTED_ORIGIN = "https://main.asset.muon.invalid";
    private String appUrl;
    private static final String RPC_OBJECT_NAME = "muonAndroidRpc";
    private static final String JAVASCRIPT_RUNTIME_OBJECT_NAME =
            "muonAndroidJavaScriptRuntime";
    private static final String TEST_OBJECT_NAME = "muonAndroidTest";
    private static final String LOG_TAG = "MuonActivity";

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
    private ScriptHandler pluginMetadataScriptHandler;
    private MuonAssetRequestHandler assetRequestHandler;
    private boolean testBridgeInstalled;
    private boolean pageLoadStarted;
    @Nullable private String startupFailure;

    @Override
    protected void onCreate(@Nullable Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        if (!WebViewFeature.isFeatureSupported(WebViewFeature.WEB_MESSAGE_LISTENER)) {
            throw new IllegalStateException("WebView message listeners are unavailable");
        }
        if (!WebViewFeature.isFeatureSupported(WebViewFeature.WEB_MESSAGE_ARRAY_BUFFER)) {
            throw new IllegalStateException("WebView ArrayBuffer messages are unavailable");
        }
        if (!WebViewFeature.isFeatureSupported(WebViewFeature.DOCUMENT_START_SCRIPT)) {
            throw new IllegalStateException("WebView document-start scripts are unavailable");
        }

        appUrl = MuonAppConfig.load(this).startPage;
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
                if (appUrl.equals(url)) {
                    Log.i(LOG_TAG, "Muon page ready: " + url);
                    pageReady.countDown();
                }
            }
        });

        try {
            rpcBridge = new MuonRpcBridge(this, webView, this);
            javaScriptRuntimeBridge = new MuonJavaScriptRuntimeBridge(this);
        } catch (RuntimeException error) {
            showStartupFailure(error.getMessage() == null
                    ? error.getClass().getSimpleName()
                    : error.getMessage());
            return;
        }
        if (rpcBridge.isNativeHostReady()) {
            startPage(rpcBridge);
        }
    }

    public void onNativeHostReady(@NonNull MuonRpcBridge source) {
        if (rpcBridge == source) {
            startPage(source);
        }
    }

    public void onNativeHostStartupFailed(
            @NonNull MuonRpcBridge source,
            @NonNull String diagnostic) {
        if (rpcBridge == source) {
            showStartupFailure(diagnostic);
        }
    }

    private void startPage(@NonNull MuonRpcBridge source) {
        if (rpcBridge != source || pageLoadStarted || startupFailure != null ||
                webView == null) {
            return;
        }
        WebViewCompat.addWebMessageListener(
                webView,
                RPC_OBJECT_NAME,
                Collections.singleton(TRUSTED_ORIGIN),
                rpcBridge);
        WebViewCompat.addWebMessageListener(
                webView,
                JAVASCRIPT_RUNTIME_OBJECT_NAME,
                Collections.singleton(TRUSTED_ORIGIN),
                javaScriptRuntimeBridge);
        pluginMetadataScriptHandler = WebViewCompat.addDocumentStartJavaScript(
                webView,
                rpcBridge.getDocumentStartScript(),
                Collections.singleton(TRUSTED_ORIGIN));
        if ((getApplicationInfo().flags & ApplicationInfo.FLAG_DEBUGGABLE) != 0) {
            WebViewCompat.addWebMessageListener(
                    webView,
                    TEST_OBJECT_NAME,
                    Collections.singleton(TRUSTED_ORIGIN),
                    this::receiveTestMessage);
            testBridgeInstalled = true;
        }

        setContentView(webView);
        pageLoadStarted = true;
        webView.loadUrl(appUrl);
    }

    private void showStartupFailure(@NonNull String diagnosticText) {
        if (startupFailure != null || pageLoadStarted) {
            return;
        }
        startupFailure = diagnosticText;
        Log.e(LOG_TAG, "Native plugin startup failed: " + startupFailure);
        TextView diagnostic = new TextView(this);
        diagnostic.setText("muon startup failed: " + startupFailure);
        setContentView(diagnostic);
        startupFailureReady.countDown();
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
        return pageLoadStarted;
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
        if (pluginMetadataScriptHandler != null) {
            pluginMetadataScriptHandler.remove();
            pluginMetadataScriptHandler = null;
        }
        if (webView != null) {
            WebViewCompat.removeWebMessageListener(webView, RPC_OBJECT_NAME);
            WebViewCompat.removeWebMessageListener(
                    webView,
                    JAVASCRIPT_RUNTIME_OBJECT_NAME);
            if (testBridgeInstalled) {
                WebViewCompat.removeWebMessageListener(webView, TEST_OBJECT_NAME);
                testBridgeInstalled = false;
            }
        }
        if (rpcBridge != null) {
            rpcBridge.close(isChangingConfigurations());
            rpcBridge = null;
        }
        if (javaScriptRuntimeBridge != null) {
            javaScriptRuntimeBridge.close();
            javaScriptRuntimeBridge = null;
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

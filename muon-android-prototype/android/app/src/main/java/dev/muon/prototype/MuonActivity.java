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
    static final String TRUSTED_ORIGIN = "https://appassets.androidplatform.net";
    private static final String APP_URL = TRUSTED_ORIGIN + "/assets/index.html";
    private static final String RPC_OBJECT_NAME = "muonAndroidRpc";
    private static final String TEST_OBJECT_NAME = "muonAndroidTest";

    static {
        System.loadLibrary("muon_android_rpc");
    }

    private final CountDownLatch pageReady = new CountDownLatch(1);
    private final LinkedBlockingQueue<String> testMessages = new LinkedBlockingQueue<>();
    private final LinkedBlockingQueue<String> finishedPageUrls = new LinkedBlockingQueue<>();
    private WebView webView;
    private MuonRpcBridge rpcBridge;
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

        MuonWebViewNetworkFilter networkFilter = new MuonWebViewNetworkFilter(this);
        networkFilter.configureServiceWorkers();
        webView = new WebView(this);
        WebSettings settings = webView.getSettings();
        networkFilter.configureWebViewSettings(settings);
        webView.setWebViewClient(new WebViewClient() {
            @Override
            public boolean shouldOverrideUrlLoading(
                    WebView view,
                    WebResourceRequest request) {
                return networkFilter.shouldBlockNavigation(request);
            }

            @Override
            public WebResourceResponse shouldInterceptRequest(
                    WebView view,
                    WebResourceRequest request) {
                return networkFilter.shouldInterceptRequest(request);
            }

            @Override
            public void onPageFinished(WebView view, String url) {
                finishedPageUrls.add(url);
                if (APP_URL.equals(url)) {
                    pageReady.countDown();
                }
            }
        });

        rpcBridge = new MuonRpcBridge();
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

    int getNativePendingCallCountForTest() {
        return rpcBridge == null ? 0 : rpcBridge.getNativePendingCallCount();
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
            rpcBridge.close();
            rpcBridge = null;
        }
        if (webView != null) {
            webView.destroy();
            webView = null;
        }
        super.onDestroy();
    }
}

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
    private final LinkedBlockingQueue<String> testMessages = new LinkedBlockingQueue<>();
    private WebView webView;
    private MuonJavaScriptRuntimeBridge javaScriptRuntimeBridge;
    private MuonWebViewHost host;
    private boolean testBridgeInstalled;

    @Override
    protected void onCreate(@Nullable Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        host = new MuonWebViewHost(this, new MuonWebViewHost.Listener() {
            @Override public void beforePageLoad(@NonNull WebView view, @NonNull MuonRpcBridge bridge) {
                webView = view;
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
                if (MuonAppConfig.load(MuonActivity.this).startPage.equals(url)) {
                    pageReady.countDown();
                }
            }
            @Override public void mainFrameHttpError(int status) {}
            @Override public void startupFailed(@NonNull String diagnostic) {}
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

    @Nullable String awaitTestMessage(long timeout, @NonNull TimeUnit unit)
            throws InterruptedException {
        return testMessages.poll(timeout, unit);
    }

    void clearTestMessages() {
        testMessages.clear();
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
        webView = null;
        super.onDestroy();
    }
}

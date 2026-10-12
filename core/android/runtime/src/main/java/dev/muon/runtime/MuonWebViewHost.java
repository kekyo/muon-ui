/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

package dev.muon.runtime;

import android.app.Activity;
import android.util.Log;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.TextView;
import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.webkit.ScriptHandler;
import androidx.webkit.WebViewCompat;
import androidx.webkit.WebViewFeature;
import java.util.Collections;

/** Owns the common WebView, assets and RPC lifecycle for an Android Activity. */
final class MuonWebViewHost implements MuonRpcBridge.Listener {
    interface Listener {
        void beforePageLoad(@NonNull WebView view, @NonNull MuonRpcBridge bridge);
        void pageFinished(@NonNull String url);
        void mainFrameHttpError(int status);
        void startupFailed(@NonNull String diagnostic);
    }

    private static final String RPC_OBJECT_NAME = "muonAndroidRpc";
    private final Activity activity;
    private final Listener listener;
    private WebView webView;
    private MuonRpcBridge bridge;
    private MuonAssetRequestHandler assets;
    private ScriptHandler script;
    private String bootstrap;
    private String startPage;
    private boolean pageLoadStarted;
    private boolean pluginEnabled;
    @Nullable private String startupFailure;

    MuonWebViewHost(@NonNull Activity activity, @NonNull Listener listener) {
        this.activity = activity;
        this.listener = listener;
    }

    void start(@NonNull String bootstrap) {
        this.bootstrap = bootstrap;
        try {
            for (String feature : new String[] {
                    WebViewFeature.WEB_MESSAGE_LISTENER,
                    WebViewFeature.WEB_MESSAGE_ARRAY_BUFFER,
                    WebViewFeature.DOCUMENT_START_SCRIPT }) {
                if (!WebViewFeature.isFeatureSupported(feature)) {
                    throw new IllegalStateException("Required WebView feature is unavailable: " + feature);
                }
            }
            MuonAppConfig config = MuonAppConfig.load(activity);
            startPage = config.startPage;
            pluginEnabled = config.pluginEnabled;
            assets = new MuonAssetRequestHandler(activity);
            assets.configureServiceWorkers();
            webView = new WebView(activity);
            assets.configureWebViewSettings(webView.getSettings());
            webView.setWebViewClient(new WebViewClient() {
                @Override
                public WebResourceResponse shouldInterceptRequest(
                        WebView view, WebResourceRequest request) {
                    return assets.shouldInterceptRequest(request);
                }

                @Override
                public void onReceivedHttpError(WebView view, WebResourceRequest request,
                        WebResourceResponse response) {
                    if (request.isForMainFrame()) {
                        listener.mainFrameHttpError(response.getStatusCode());
                    }
                }

                @Override
                public void onPageFinished(WebView view, String url) {
                    if (startPage.equals(url)) {
                        Log.i("MuonActivity", "Muon page ready: " + url);
                    }
                    listener.pageFinished(url);
                }
            });
            bridge = new MuonRpcBridge(activity, webView, this);
            if (bridge.isNativeHostReady()) {
                onNativeHostReady(bridge);
            }
        } catch (RuntimeException | LinkageError error) {
            showFailure(error.getMessage() == null
                    ? error.getClass().getSimpleName() : error.getMessage());
        }
    }

    @Override
    public void onNativeHostReady(@NonNull MuonRpcBridge source) {
        if (source != bridge || pageLoadStarted || startupFailure != null) {
            return;
        }
        if (pluginEnabled) {
            WebViewCompat.addWebMessageListener(webView, RPC_OBJECT_NAME,
                    Collections.singleton(MuonAppConfig.TRUSTED_ORIGIN), bridge);
            script = WebViewCompat.addDocumentStartJavaScript(webView,
                    bridge.getDocumentStartScript() + "\n" + bootstrap,
                    Collections.singleton(MuonAppConfig.TRUSTED_ORIGIN));
        }
        listener.beforePageLoad(webView, bridge);
        activity.setContentView(webView);
        pageLoadStarted = true;
        webView.loadUrl(startPage);
    }

    @Override
    public void onNativeHostStartupFailed(@NonNull MuonRpcBridge source,
            @NonNull String diagnostic) {
        if (source == bridge) {
            showFailure(diagnostic);
        }
    }

    void showFailure(@NonNull String diagnostic) {
        if (startupFailure != null || pageLoadStarted) {
            return;
        }
        startupFailure = diagnostic;
        Log.e("MuonActivity", "Muon startup failed: " + diagnostic);
        TextView view = new TextView(activity);
        view.setText("muon startup failed: " + diagnostic);
        activity.setContentView(view);
        listener.startupFailed(diagnostic);
    }

    boolean wasPageLoadStarted() { return pageLoadStarted; }

    void close(boolean changingConfigurations) {
        if (script != null) { script.remove(); script = null; }
        if (webView != null) {
            WebViewCompat.removeWebMessageListener(webView, RPC_OBJECT_NAME);
        }
        if (bridge != null) { bridge.close(changingConfigurations); bridge = null; }
        if (assets != null) { assets.close(); assets = null; }
        if (webView != null) { webView.destroy(); webView = null; }
    }
}

/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

package dev.muon.runtime;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Bitmap;
import android.net.Uri;
import android.util.Log;
import android.view.View;
import android.view.WindowInsets;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.FrameLayout;
import android.widget.TextView;
import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.core.view.ViewCompat;
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
    private ScriptHandler cssScript;
    private FrameLayout content;
    private String bootstrap;
    private String startPage;
    private boolean pageLoadStarted;
    private boolean pluginEnabled;
    private boolean nativeReady;
    private boolean documentStarted;
    @Nullable private String startupFailure;

    MuonWebViewHost(@NonNull Activity activity, @NonNull Listener listener) {
        this.activity = activity;
        this.listener = listener;
    }

    void start(@NonNull String bootstrap, @NonNull MuonAppConfig config) {
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
            startPage = config.startPage;
            pluginEnabled = config.pluginEnabled;
            assets = new MuonAssetRequestHandler(activity);
            assets.configureServiceWorkers();
            webView = new WebView(activity);
            content = new FrameLayout(activity);
            content.addView(webView, new FrameLayout.LayoutParams(
                    FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));
            assets.configureWebViewSettings(webView.getSettings());
            // Interception runs off the UI thread and can finish after close().
            // Keep its immutable loader alive for this WebViewClient's lifetime.
            final MuonAssetRequestHandler requestAssets = assets;
            webView.setWebViewClient(new WebViewClient() {
                @Override
                public void onPageStarted(WebView view, String url, Bitmap favicon) {
                    if (documentStarted) { bridge.releasePageContext(); }
                    documentStarted = true;
                }

                @Override
                public void onScaleChanged(WebView view, float oldScale, float newScale) {
                    // getScale() is deprecated because its UI/render-thread values can race.
                    if (bridge != null) { bridge.getSystemBarsService().onScaleChanged(newScale); }
                }

                @Override
                public WebResourceResponse shouldInterceptRequest(
                        WebView view, WebResourceRequest request) {
                    return requestAssets.shouldInterceptRequest(request);
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
            bridge = new MuonRpcBridge(activity, webView, this, config);
            bridge.onNotificationIntent(activity.getIntent());
            bridge.getSystemBarsService().attach(content, this::updateInsets);
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
        if (source != bridge || nativeReady || startupFailure != null) {
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
        nativeReady = true;
        // A recreated Activity can reuse its Window without redispatching unchanged
        // insets on API 35. Wait for its new bounds before reading the root snapshot;
        // attachment can still expose the previous full-screen window's geometry.
        content.addOnLayoutChangeListener(new View.OnLayoutChangeListener() {
            @Override public void onLayoutChange(View view, int left, int top, int right,
                    int bottom, int oldLeft, int oldTop, int oldRight, int oldBottom) {
                WindowInsets insets = view.getRootWindowInsets();
                if (insets != null) {
                    view.removeOnLayoutChangeListener(this);
                    if (bridge.getSystemBarsService().getCssScript() != null) { return; }
                    // Preserve the decor's consumption in multi-window mode.
                    WindowInsets remaining = activity.getWindow().getDecorView()
                            .dispatchApplyWindowInsets(insets);
                    // If the reused hierarchy did not deliver to this new subtree,
                    // seed it with the remainder after decor consumption.
                    if (bridge.getSystemBarsService().getCssScript() == null) {
                        view.dispatchApplyWindowInsets(remaining);
                    }
                }
            }
        });
        activity.setContentView(content);
        ViewCompat.requestApplyInsets(content);
        updateInsets();
    }

    private void updateInsets() {
        if (bridge == null || startupFailure != null) { return; }
        String source = bridge.getSystemBarsService().getCssScript();
        if (source == null) { return; }
        if (cssScript != null) { cssScript.remove(); }
        cssScript = WebViewCompat.addDocumentStartJavaScript(webView, source,
                Collections.singleton(MuonAppConfig.TRUSTED_ORIGIN));
        if (nativeReady && !pageLoadStarted) {
            // A real first snapshot is installed before the application's first document.
            pageLoadStarted = true;
            webView.loadUrl(startPage);
        } else if (pageLoadStarted && isTrustedPage(webView.getUrl())) {
            webView.evaluateJavascript(source, null);
        }
    }

    private static boolean isTrustedPage(@Nullable String url) {
        if (url == null) { return false; }
        Uri page = Uri.parse(url);
        Uri origin = Uri.parse(MuonAppConfig.TRUSTED_ORIGIN);
        return origin.getScheme().equals(page.getScheme())
                && origin.getHost().equals(page.getHost()) && page.getPort() == origin.getPort();
    }

    @Nullable String retainRequestedState() {
        return bridge == null ? null : bridge.getSystemBarsService().retainRequestedState();
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

    void setResumed(boolean value) {
        if (bridge != null) { bridge.setResumed(value); }
    }

    void onNotificationIntent(Intent intent) {
        if (bridge != null) { bridge.onNotificationIntent(intent); }
    }

    boolean onRequestPermissionsResult(int code, @NonNull String[] permissions, @NonNull int[] results) {
        return bridge != null && bridge.onRequestPermissionsResult(code, permissions, results);
    }

    void close(boolean changingConfigurations) {
        if (script != null) { script.remove(); script = null; }
        if (cssScript != null) { cssScript.remove(); cssScript = null; }
        if (webView != null) {
            WebViewCompat.removeWebMessageListener(webView, RPC_OBJECT_NAME);
        }
        if (bridge != null) { bridge.close(changingConfigurations); bridge = null; }
        if (assets != null) { assets.close(); assets = null; }
        if (webView != null) { webView.destroy(); webView = null; }
        content = null;
    }
}

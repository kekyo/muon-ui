/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

package dev.muon.runtime;

import android.app.Activity;
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
import androidx.webkit.ServiceWorkerClientCompat;
import androidx.webkit.ServiceWorkerControllerCompat;
import androidx.webkit.ServiceWorkerWebSettingsCompat;
import androidx.webkit.WebMessageCompat;
import androidx.webkit.WebViewCompat;
import androidx.webkit.WebViewFeature;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;

/** Records the request callbacks exposed by WebView for debug capability tests. */
public final class MuonNetworkCapabilityProbeActivity extends Activity {
    static final String EXTRA_PAGE_URL = "dev.muon.prototype.extra.PAGE_URL";
    private static final String MESSAGE_OBJECT_NAME = "muonNetworkProbe";

    private final CountDownLatch pageReady = new CountDownLatch(1);
    private final LinkedBlockingQueue<String> messages = new LinkedBlockingQueue<>();
    private final List<RequestObservation> webViewRequests =
            Collections.synchronizedList(new ArrayList<>());
    private final List<RequestObservation> serviceWorkerRequests =
            Collections.synchronizedList(new ArrayList<>());
    private WebView webView;
    private String pageUrl;
    private String pageOrigin;

    @Override
    protected void onCreate(@Nullable Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        pageUrl = getIntent().getStringExtra(EXTRA_PAGE_URL);
        Uri parsedPageUrl = pageUrl == null ? Uri.EMPTY : Uri.parse(pageUrl);
        String scheme = parsedPageUrl.getScheme();
        String authority = parsedPageUrl.getEncodedAuthority();
        if (scheme == null
                || authority == null
                || !(scheme.toLowerCase(Locale.ROOT).equals("http")
                || scheme.toLowerCase(Locale.ROOT).equals("https"))) {
            throw new IllegalArgumentException("A valid HTTP probe page URL is required");
        }
        pageOrigin = scheme.toLowerCase(Locale.ROOT) + "://" + authority;

        requireFeature(WebViewFeature.WEB_MESSAGE_LISTENER);
        configureServiceWorkerObservation();

        webView = new WebView(this);
        WebSettings settings = webView.getSettings();
        settings.setJavaScriptEnabled(true);
        settings.setAllowFileAccess(false);
        settings.setAllowContentAccess(false);
        settings.setBlockNetworkLoads(false);
        settings.setMixedContentMode(WebSettings.MIXED_CONTENT_NEVER_ALLOW);
        webView.setWebViewClient(new WebViewClient() {
            @Override
            public WebResourceResponse shouldInterceptRequest(
                    WebView view,
                    WebResourceRequest request) {
                webViewRequests.add(RequestObservation.from(request));
                return null;
            }

            @Override
            public void onPageFinished(WebView view, String url) {
                if (pageUrl.equals(url)) {
                    pageReady.countDown();
                }
            }
        });
        WebViewCompat.addWebMessageListener(
                webView,
                MESSAGE_OBJECT_NAME,
                Collections.singleton(pageOrigin),
                this::receiveMessage);

        setContentView(webView);
        webView.loadUrl(pageUrl);
    }

    private void configureServiceWorkerObservation() {
        requireFeature(WebViewFeature.SERVICE_WORKER_BASIC_USAGE);
        requireFeature(WebViewFeature.SERVICE_WORKER_BLOCK_NETWORK_LOADS);
        requireFeature(WebViewFeature.SERVICE_WORKER_CONTENT_ACCESS);
        requireFeature(WebViewFeature.SERVICE_WORKER_FILE_ACCESS);
        requireFeature(WebViewFeature.SERVICE_WORKER_SHOULD_INTERCEPT_REQUEST);

        ServiceWorkerControllerCompat controller =
                ServiceWorkerControllerCompat.getInstance();
        ServiceWorkerWebSettingsCompat settings =
                controller.getServiceWorkerWebSettings();
        settings.setAllowContentAccess(false);
        settings.setAllowFileAccess(false);
        settings.setBlockNetworkLoads(false);
        controller.setServiceWorkerClient(new ServiceWorkerClientCompat() {
            @Nullable
            @Override
            public WebResourceResponse shouldInterceptRequest(
                    @NonNull WebResourceRequest request) {
                serviceWorkerRequests.add(RequestObservation.from(request));
                return null;
            }
        });
    }

    private void receiveMessage(
            @NonNull WebView view,
            @NonNull WebMessageCompat message,
            @NonNull Uri sourceOrigin,
            boolean isMainFrame,
            @NonNull JavaScriptReplyProxy replyProxy) {
        if (!isMainFrame
                || !pageOrigin.equals(sourceOrigin.toString())
                || message.getType() != WebMessageCompat.TYPE_STRING) {
            return;
        }
        String data = message.getData();
        if (data != null) {
            messages.add(data);
        }
    }

    WebView getWebViewForTest() {
        return webView;
    }

    boolean awaitPageReadyForTest(long timeout, @NonNull TimeUnit unit)
            throws InterruptedException {
        return pageReady.await(timeout, unit);
    }

    @Nullable String awaitMessageForTest(long timeout, @NonNull TimeUnit unit)
            throws InterruptedException {
        return messages.poll(timeout, unit);
    }

    void clearRequestObservationsForTest() {
        webViewRequests.clear();
        serviceWorkerRequests.clear();
    }

    List<RequestObservation> getWebViewRequestsForTest() {
        synchronized (webViewRequests) {
            return new ArrayList<>(webViewRequests);
        }
    }

    List<RequestObservation> getServiceWorkerRequestsForTest() {
        synchronized (serviceWorkerRequests) {
            return new ArrayList<>(serviceWorkerRequests);
        }
    }

    @Override
    protected void onDestroy() {
        if (webView != null) {
            WebViewCompat.removeWebMessageListener(webView, MESSAGE_OBJECT_NAME);
            webView.destroy();
            webView = null;
        }
        ServiceWorkerControllerCompat.getInstance().setServiceWorkerClient(null);
        super.onDestroy();
    }

    private static void requireFeature(@NonNull String feature) {
        if (!WebViewFeature.isFeatureSupported(feature)) {
            throw new IllegalStateException(
                    "Required WebView probe feature is unavailable: " + feature);
        }
    }

    static final class RequestObservation {
        private final String url;
        private final boolean mainFrame;
        private final boolean redirect;

        private RequestObservation(String url, boolean mainFrame, boolean redirect) {
            this.url = url;
            this.mainFrame = mainFrame;
            this.redirect = redirect;
        }

        private static RequestObservation from(@NonNull WebResourceRequest request) {
            return new RequestObservation(
                    request.getUrl().toString(),
                    request.isForMainFrame(),
                    request.isRedirect());
        }

        String getUrl() {
            return url;
        }

        boolean isMainFrame() {
            return mainFrame;
        }

        boolean isRedirect() {
            return redirect;
        }
    }
}

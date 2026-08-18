/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

package dev.muon.prototype;

import android.Manifest;
import android.content.Context;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
import android.webkit.WebSettings;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.webkit.ServiceWorkerClientCompat;
import androidx.webkit.ServiceWorkerControllerCompat;
import androidx.webkit.ServiceWorkerWebSettingsCompat;
import androidx.webkit.WebViewAssetLoader;
import androidx.webkit.WebViewFeature;

import java.io.ByteArrayInputStream;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;
import java.util.Locale;
import java.util.Map;

/** Serves the trusted asset origin without filtering ordinary WebView network traffic. */
final class MuonAssetRequestHandler implements AutoCloseable {
    private static final String TRUSTED_SCHEME = "https";
    private static final String TRUSTED_HOST = "main.asset.muon.invalid";
    private static final String TRUSTED_PATH_PREFIX = "/";
    private static final byte[] NOT_FOUND_BODY =
            "Not Found".getBytes(StandardCharsets.UTF_8);
    private static final byte[] METHOD_NOT_ALLOWED_BODY =
            "Method Not Allowed".getBytes(StandardCharsets.UTF_8);

    private final WebViewAssetLoader assetLoader;
    private final boolean networkLoadsAllowed;
    private ServiceWorkerControllerCompat serviceWorkerController;

    MuonAssetRequestHandler(@NonNull Context context) {
        assetLoader = new WebViewAssetLoader.Builder()
                .setDomain(TRUSTED_HOST)
                .addPathHandler(TRUSTED_PATH_PREFIX,
                        new WebViewAssetLoader.AssetsPathHandler(context))
                .build();
        networkLoadsAllowed = context.checkSelfPermission(Manifest.permission.INTERNET)
                == PackageManager.PERMISSION_GRANTED;
    }

    void configureWebViewSettings(@NonNull WebSettings settings) {
        settings.setJavaScriptEnabled(true);
        settings.setAllowFileAccess(false);
        settings.setAllowContentAccess(false);
        settings.setBlockNetworkLoads(!networkLoadsAllowed);
        settings.setMixedContentMode(WebSettings.MIXED_CONTENT_NEVER_ALLOW);
        settings.setSupportZoom(true);
        settings.setBuiltInZoomControls(false);
        settings.setDisplayZoomControls(false);
    }

    void configureServiceWorkers() {
        requireFeature(WebViewFeature.SERVICE_WORKER_BASIC_USAGE);
        requireFeature(WebViewFeature.SERVICE_WORKER_BLOCK_NETWORK_LOADS);
        requireFeature(WebViewFeature.SERVICE_WORKER_CONTENT_ACCESS);
        requireFeature(WebViewFeature.SERVICE_WORKER_FILE_ACCESS);
        requireFeature(WebViewFeature.SERVICE_WORKER_SHOULD_INTERCEPT_REQUEST);

        serviceWorkerController = ServiceWorkerControllerCompat.getInstance();
        ServiceWorkerWebSettingsCompat settings =
                serviceWorkerController.getServiceWorkerWebSettings();
        settings.setAllowContentAccess(false);
        settings.setAllowFileAccess(false);
        settings.setBlockNetworkLoads(!networkLoadsAllowed);
        serviceWorkerController.setServiceWorkerClient(new ServiceWorkerClientCompat() {
            @Nullable
            @Override
            public WebResourceResponse shouldInterceptRequest(
                    @NonNull WebResourceRequest request) {
                return MuonAssetRequestHandler.this.shouldInterceptRequest(request);
            }
        });
    }

    @Nullable WebResourceResponse shouldInterceptRequest(
            @NonNull WebResourceRequest request) {
        Uri url = request.getUrl();
        if (!isTrustedAssetUrl(url)) {
            return null;
        }

        String method = request.getMethod();
        if (!"GET".equals(method) && !"HEAD".equals(method)) {
            return createErrorResponse(
                    405,
                    "Method Not Allowed",
                    METHOD_NOT_ALLOWED_BODY,
                    true);
        }

        WebResourceResponse response = assetLoader.shouldInterceptRequest(url);
        if (response == null || response.getData() == null) {
            return createErrorResponse(404, "Not Found", NOT_FOUND_BODY, false);
        }
        applySecurityHeaders(response);
        return response;
    }

    @Override
    public void close() {
        if (serviceWorkerController != null) {
            serviceWorkerController.setServiceWorkerClient(null);
            serviceWorkerController = null;
        }
    }

    private static boolean isTrustedAssetUrl(@NonNull Uri url) {
        String scheme = url.getScheme();
        String host = url.getHost();
        String path = url.getPath();
        int port = url.getPort();
        return scheme != null
                && host != null
                && path != null
                && TRUSTED_SCHEME.equals(scheme.toLowerCase(Locale.ROOT))
                && TRUSTED_HOST.equals(host.toLowerCase(Locale.ROOT))
                && (port == -1 || port == 443)
                && path.startsWith(TRUSTED_PATH_PREFIX);
    }

    private static void applySecurityHeaders(@NonNull WebResourceResponse response) {
        Map<String, String> existingHeaders = response.getResponseHeaders();
        Map<String, String> headers = existingHeaders == null
                ? new HashMap<>()
                : new HashMap<>(existingHeaders);
        headers.put("X-Content-Type-Options", "nosniff");
        response.setResponseHeaders(headers);
    }

    @NonNull private static WebResourceResponse createErrorResponse(
            int statusCode,
            @NonNull String reasonPhrase,
            @NonNull byte[] body,
            boolean includeAllowHeader) {
        Map<String, String> headers = new HashMap<>();
        headers.put("Cache-Control", "no-store");
        headers.put("Content-Security-Policy", "default-src 'none'");
        headers.put("X-Content-Type-Options", "nosniff");
        if (includeAllowHeader) {
            headers.put("Allow", "GET, HEAD");
        }
        return new WebResourceResponse(
                "text/plain",
                StandardCharsets.UTF_8.name(),
                statusCode,
                reasonPhrase,
                headers,
                new ByteArrayInputStream(body));
    }

    private static void requireFeature(@NonNull String feature) {
        if (!WebViewFeature.isFeatureSupported(feature)) {
            throw new IllegalStateException(
                    "Required WebView network feature is unavailable: " + feature);
        }
    }
}

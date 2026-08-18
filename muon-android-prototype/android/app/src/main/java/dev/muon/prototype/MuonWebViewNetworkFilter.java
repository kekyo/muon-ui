/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

package dev.muon.prototype;

import android.content.Context;
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

/** Enforces the fail-closed network boundary used by the Android WebView backend. */
final class MuonWebViewNetworkFilter {
    private static final String TRUSTED_SCHEME = "https";
    private static final String TRUSTED_HOST = "appassets.androidplatform.net";
    private static final String TRUSTED_PATH_PREFIX = "/assets/";
    private static final String DATA_IMAGE_PREFIX = "data:image/";
    private static final String CONTENT_SECURITY_POLICY = String.join("; ",
            "default-src 'none'",
            "script-src 'self'",
            "style-src 'self'",
            "img-src 'self' data:",
            "connect-src 'none'",
            "frame-src 'none'",
            "worker-src 'none'",
            "object-src 'none'",
            "base-uri 'none'",
            "form-action 'none'");
    private static final byte[] FORBIDDEN_BODY =
            "Forbidden".getBytes(StandardCharsets.UTF_8);

    private final WebViewAssetLoader assetLoader;

    MuonWebViewNetworkFilter(@NonNull Context context) {
        assetLoader = new WebViewAssetLoader.Builder()
                .addPathHandler(TRUSTED_PATH_PREFIX,
                        new WebViewAssetLoader.AssetsPathHandler(context))
                .build();
    }

    void configureWebViewSettings(@NonNull WebSettings settings) {
        settings.setJavaScriptEnabled(true);
        settings.setAllowFileAccess(false);
        settings.setAllowContentAccess(false);
        settings.setBlockNetworkLoads(true);
        settings.setMixedContentMode(WebSettings.MIXED_CONTENT_NEVER_ALLOW);
    }

    void configureServiceWorkers() {
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
        settings.setBlockNetworkLoads(true);
        controller.setServiceWorkerClient(new ServiceWorkerClientCompat() {
            @Nullable
            @Override
            public WebResourceResponse shouldInterceptRequest(
                    @NonNull WebResourceRequest request) {
                return createForbiddenResponse(false);
            }
        });
    }

    boolean shouldBlockNavigation(@NonNull WebResourceRequest request) {
        return !isAllowedUrl(request.getUrl());
    }

    @Nullable WebResourceResponse shouldInterceptRequest(
            @NonNull WebResourceRequest request) {
        Uri url = request.getUrl();
        if (isTrustedAssetUrl(url)) {
            WebResourceResponse response = assetLoader.shouldInterceptRequest(url);
            if (response != null) {
                applySecurityHeaders(response);
                return response;
            }
        } else if (isDataImageUrl(url)) {
            return null;
        }
        return createForbiddenResponse(request.isForMainFrame());
    }

    private static boolean isAllowedUrl(@NonNull Uri url) {
        return isTrustedAssetUrl(url) || isDataImageUrl(url);
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

    private static boolean isDataImageUrl(@NonNull Uri url) {
        return url.toString().startsWith(DATA_IMAGE_PREFIX);
    }

    private static void applySecurityHeaders(@NonNull WebResourceResponse response) {
        Map<String, String> existingHeaders = response.getResponseHeaders();
        Map<String, String> headers = existingHeaders == null
                ? new HashMap<>()
                : new HashMap<>(existingHeaders);
        headers.put("Content-Security-Policy", CONTENT_SECURITY_POLICY);
        headers.put("X-Content-Type-Options", "nosniff");
        response.setResponseHeaders(headers);
    }

    @NonNull private static WebResourceResponse createForbiddenResponse(
            boolean isMainFrame) {
        Map<String, String> headers = new HashMap<>();
        headers.put("Cache-Control", "no-store");
        headers.put("Content-Security-Policy", "default-src 'none'");
        headers.put("X-Content-Type-Options", "nosniff");
        return new WebResourceResponse(
                isMainFrame ? "text/html" : "text/plain",
                StandardCharsets.UTF_8.name(),
                403,
                "Forbidden",
                headers,
                new ByteArrayInputStream(FORBIDDEN_BODY));
    }

    private static void requireFeature(@NonNull String feature) {
        if (!WebViewFeature.isFeatureSupported(feature)) {
            throw new IllegalStateException(
                    "Required WebView network feature is unavailable: " + feature);
        }
    }
}

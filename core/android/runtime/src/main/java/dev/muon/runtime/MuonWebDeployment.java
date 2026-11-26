/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
package dev.muon.runtime;

import android.net.Uri;
import androidx.annotation.Nullable;
import java.net.URI;
import java.net.URISyntaxException;
import java.util.HashSet;
import java.util.Locale;
import java.util.Set;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

/** Immutable origins and local media paths selected by the application when it was packaged. */
final class MuonWebDeployment {
    static final String BUNDLED_ORIGIN = "https://main.asset.muon.invalid";
    final String origin;
    final boolean restricted;
    final boolean hasDocumentScript;
    private final Set<String> resources = new HashSet<>();
    private final Set<String> loopbackPaths = new HashSet<>();

    MuonWebDeployment(String entry, @Nullable JSONObject web) throws JSONException {
        origin = httpsOrigin(entry);
        if (origin == null) throw new JSONException("Invalid Android HTTPS start page");
        JSONArray extra = web == null ? null : web.optJSONArray("resourceOrigins");
        restricted = !BUNDLED_ORIGIN.equals(origin) || extra != null;
        hasDocumentScript = web != null && web.optBoolean("documentStartScript");
        resources.add(origin);
        if (extra != null) {
            for (int i = 0; i < extra.length(); ++i) {
                String value = extra.getString(i);
                String scope = httpsOrigin(value);
                if (scope == null || !scope.equals(value)) throw new JSONException("Invalid Android resource origin");
                resources.add(scope);
            }
        }
        JSONArray paths = web == null ? null : web.optJSONArray("loopbackResourcePaths");
        if (paths != null) {
            for (int i = 0; i < paths.length(); ++i) {
                String path = paths.getString(i);
                if (!path.matches("/(?:[a-zA-Z0-9_~-][a-zA-Z0-9._~-]*/)+"))
                    throw new JSONException("Invalid Android loopback resource directory");
                loopbackPaths.add(path);
            }
        }
    }

    @Nullable private static String httpsOrigin(@Nullable String text) {
        if (text == null || text.indexOf('*') >= 0) return null;
        try {
            URI uri = new URI(text).parseServerAuthority();
            int port = uri.getPort();
            if (!"https".equalsIgnoreCase(uri.getScheme()) || uri.getHost() == null || uri.getRawUserInfo() != null
                    || port == 0 || port > 65535) return null;
            return "https://" + uri.getHost().toLowerCase(Locale.ROOT) + (port < 0 || port == 443 ? "" : ":" + port);
        } catch (URISyntaxException invalid) { return null; }
    }

    boolean trusted(@Nullable String url) { return origin.equals(httpsOrigin(url)); }

    boolean allowsNavigation(String url) { return !restricted || trusted(url); }

    boolean allowsResource(Uri url) {
        if (!restricted || resources.contains(httpsOrigin(url.toString()))) return true;
        if (!"http".equals(url.getScheme()) || !"127.0.0.1".equals(url.getHost())
                || url.getUserInfo() != null || url.getPort() <= 0 || url.getPort() > 65535) return false;
        String path = url.getEncodedPath();
        if (path == null || path.contains("%") || path.contains("/../") || path.contains("/./")) return false;
        for (String prefix : loopbackPaths) if (path.startsWith(prefix)) return true;
        return false;
    }
}

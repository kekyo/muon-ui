/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

package dev.muon.runtime;

import android.content.Context;
import android.net.Uri;
import androidx.annotation.NonNull;
import org.json.JSONException;
import org.json.JSONArray;
import org.json.JSONObject;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;

/** Reads the immutable application configuration packaged by the host build. */
final class MuonAppConfig {
    static final String TRUSTED_ORIGIN = "https://main.asset.muon.invalid";
    final String startPage;
    final JSONObject values;
    final boolean pluginEnabled;
    final String[] internalAllow;
    final boolean validateMode;
    final String[] capabilityIds;
    final String[][] capabilityAllows;

    private MuonAppConfig(@NonNull JSONObject config) throws JSONException {
        startPage = config.getString("startPage");
        Uri url = Uri.parse(startPage);
        if (!"https".equals(url.getScheme())
                || !"main.asset.muon.invalid".equals(url.getHost())
                || url.getPort() != -1 || url.getUserInfo() != null) {
            throw new JSONException("The Android start page must use the trusted asset origin");
        }
        values = config.getJSONObject("values");
        JSONObject plugin = config.getJSONObject("plugin");
        String mode = plugin.getString("mode");
        if (!"simple".equals(mode) && !"validate".equals(mode)) {
            throw new JSONException("Unsupported Android plugin mode");
        }
        validateMode = "validate".equals(mode);
        pluginEnabled = plugin.getBoolean("enabled");
        JSONArray allow = plugin.getJSONArray("internalAllow");
        internalAllow = new String[allow.length()];
        for (int i = 0; i < internalAllow.length; i++) {
            internalAllow[i] = allow.getString(i);
        }
        JSONArray capabilities = validateMode ? plugin.getJSONArray("capabilities") : new JSONArray();
        capabilityIds = new String[capabilities.length()];
        capabilityAllows = new String[capabilities.length()][];
        for (int i = 0; i < capabilities.length(); i++) {
            JSONObject capability = capabilities.getJSONObject(i);
            capabilityIds[i] = capability.getString("id");
            JSONArray paths = capability.getJSONArray("allow");
            capabilityAllows[i] = new String[paths.length()];
            for (int j = 0; j < paths.length(); j++) {
                capabilityAllows[i][j] = paths.getString(j);
            }
        }
    }

    @NonNull static MuonAppConfig load(@NonNull Context context) {
        try {
            return new MuonAppConfig(new JSONObject(readAsset(context, "muon/config.json")));
        } catch (JSONException error) {
            throw new IllegalStateException("Invalid Muon application configuration", error);
        }
    }

    @NonNull static String readAsset(@NonNull Context context, @NonNull String path) {
        try (InputStream input = context.getAssets().open(path);
                ByteArrayOutputStream output = new ByteArrayOutputStream()) {
            byte[] buffer = new byte[8192];
            int count;
            while ((count = input.read(buffer)) != -1) {
                output.write(buffer, 0, count);
            }
            return output.toString(StandardCharsets.UTF_8.name());
        } catch (IOException error) {
            throw new IllegalStateException("Cannot read packaged Muon asset: " + path, error);
        }
    }
}

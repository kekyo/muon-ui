/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

package dev.muon.runtime;

import android.content.Context;
import androidx.annotation.NonNull;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;
import java.util.HashSet;
import java.util.Set;

/** Immutable, package-owned plugin inputs passed to the process runtime. */
final class MuonPackagedPlugin {
    final String name;
    final String soname;
    final String[] allow;
    final String[] configKeys;
    final String[] configValues;

    private MuonPackagedPlugin(@NonNull JSONObject entry) throws JSONException {
        name = entry.getString("name");
        soname = entry.getString("soname");
        if (!name.matches("[A-Za-z_][A-Za-z0-9_]*") || name.equals("internal")
                || !soname.matches("lib[A-Za-z0-9_][A-Za-z0-9_.+-]*\\.so")) {
            throw new JSONException("Invalid packaged plugin name or soname");
        }
        JSONArray patterns = entry.getJSONArray("allow");
        if (patterns.length() == 0) {
            throw new JSONException("A packaged plugin requires an explicit allow policy");
        }
        allow = new String[patterns.length()];
        for (int i = 0; i < allow.length; i++) {
            allow[i] = patterns.getString(i);
        }
        JSONArray config = entry.getJSONArray("config");
        configKeys = new String[config.length()];
        configValues = new String[config.length()];
        for (int i = 0; i < config.length(); i++) {
            configKeys[i] = config.getJSONObject(i).getString("key");
            configValues[i] = config.getJSONObject(i).getString("value");
        }
    }

    @NonNull static MuonPackagedPlugin[] load(@NonNull Context context) {
        try {
            JSONObject registry = new JSONObject(
                    MuonAppConfig.readAsset(context, "muon/plugins.json"));
            if (registry.getInt("schemaVersion") != 1) {
                throw new JSONException("Unsupported packaged plugin registry version");
            }
            JSONArray entries = registry.getJSONArray("plugins");
            MuonPackagedPlugin[] plugins = new MuonPackagedPlugin[entries.length()];
            Set<String> names = new HashSet<>();
            Set<String> sonames = new HashSet<>();
            for (int i = 0; i < plugins.length; i++) {
                plugins[i] = new MuonPackagedPlugin(entries.getJSONObject(i));
                if (!names.add(plugins[i].name) || !sonames.add(plugins[i].soname)) {
                    throw new JSONException("Duplicate packaged plugin name or soname");
                }
            }
            return plugins;
        } catch (JSONException error) {
            throw new IllegalStateException("Invalid packaged Muon plugin registry", error);
        }
    }
}

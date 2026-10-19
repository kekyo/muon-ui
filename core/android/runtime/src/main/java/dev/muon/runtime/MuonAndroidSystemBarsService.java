/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

package dev.muon.runtime;

import android.os.Looper;
import android.view.Window;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.core.view.ViewCompat;
import androidx.core.view.WindowCompat;
import androidx.core.view.WindowInsetsCompat;
import androidx.core.view.WindowInsetsControllerCompat;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;
import org.json.JSONTokener;

import java.util.Iterator;

/** Keeps requested bar visibility separate from the window's observed visibility. */
final class MuonAndroidSystemBarsService implements AutoCloseable {
    private Window window;
    private boolean statusBarVisible = true;
    private boolean navigationBarVisible = true;

    MuonAndroidSystemBarsService(@NonNull Window window) {
        this.window = window;
    }

    void invoke(@NonNull String functionPath, @NonNull JSONArray arguments,
            @NonNull byte[][] attachments,
            @NonNull MuonAndroidPlatformService.Completion completion) {
        try {
            requireWindow();
            if (attachments.length != 0) {
                throw new IllegalArgumentException(functionPath + " does not accept binary arguments");
            }
            switch (functionPath) {
                case "muon.android.systemBars.configure":
                    if (arguments.length() != 1 || !(arguments.get(0) instanceof String)) {
                        throw new IllegalArgumentException(functionPath + " requires one JSON string");
                    }
                    configure(arguments.getString(0));
                    completion.completeVoid();
                    return;
                case "muon.android.systemBars.getState":
                    if (arguments.length() != 0) {
                        throw new IllegalArgumentException(functionPath + " does not accept arguments");
                    }
                    completion.completeString(getState().toString());
                    return;
                default:
                    throw new IllegalArgumentException("Unknown Android system bar function: " + functionPath);
            }
        } catch (JSONException | RuntimeException error) {
            String message = error.getMessage();
            completion.fail(message == null ? error.getClass().getSimpleName() : message);
        }
    }

    boolean isFullscreen() {
        return !statusBarVisible && !navigationBarVisible;
    }

    void setFullscreen(boolean fullscreen) {
        apply(!fullscreen, !fullscreen,
                WindowInsetsCompat.Type.statusBars() | WindowInsetsCompat.Type.navigationBars());
    }

    private void configure(@NonNull String json) throws JSONException {
        JSONTokener parser = new JSONTokener(json);
        Object value = parser.nextValue();
        if (!(value instanceof JSONObject) || parser.nextClean() != 0) {
            throw new IllegalArgumentException("System bar options must be a JSON object");
        }
        JSONObject options = (JSONObject) value;
        for (Iterator<String> keys = options.keys(); keys.hasNext();) {
            String key = keys.next();
            if (!key.equals("statusBar") && !key.equals("navigationBar")) {
                throw new IllegalArgumentException("Unknown system bar option: " + key);
            }
        }

        // Validate both partial updates before changing either bar or its requested state.
        Boolean status = readVisibility(options, "statusBar");
        Boolean navigation = readVisibility(options, "navigationBar");
        int changedTypes = (status == null ? 0 : WindowInsetsCompat.Type.statusBars())
                | (navigation == null ? 0 : WindowInsetsCompat.Type.navigationBars());
        apply(status == null ? statusBarVisible : status,
                navigation == null ? navigationBarVisible : navigation, changedTypes);
    }

    @Nullable private static Boolean readVisibility(@NonNull JSONObject options,
            @NonNull String key) throws JSONException {
        if (!options.has(key)) {
            return null;
        }
        Object value = options.get(key);
        if (!(value instanceof JSONObject)) {
            throw new IllegalArgumentException(key + " must be an object");
        }
        JSONObject bar = (JSONObject) value;
        for (Iterator<String> keys = bar.keys(); keys.hasNext();) {
            String member = keys.next();
            if (!member.equals("visible")) {
                throw new IllegalArgumentException("Unknown " + key + " option: " + member);
            }
        }
        if (!bar.has("visible")) {
            return null;
        }
        Object visible = bar.get("visible");
        if (!(visible instanceof Boolean)) {
            throw new IllegalArgumentException(key + ".visible must be a boolean");
        }
        return (Boolean) visible;
    }

    private void apply(boolean status, boolean navigation, int changedTypes) {
        Window current = requireWindow();
        if (changedTypes == 0) {
            return;
        }
        WindowInsetsControllerCompat controller = WindowCompat.getInsetsController(
                current, current.getDecorView());
        controller.setSystemBarsBehavior(
                WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        int visibleTypes = (status ? WindowInsetsCompat.Type.statusBars() : 0)
                | (navigation ? WindowInsetsCompat.Type.navigationBars() : 0);
        int showTypes = changedTypes & visibleTypes;
        int hideTypes = changedTypes & ~visibleTypes;
        if (showTypes != 0) {
            controller.show(showTypes);
        }
        if (hideTypes != 0) {
            controller.hide(hideTypes);
        }
        statusBarVisible = status;
        navigationBarVisible = navigation;
    }

    @NonNull private JSONObject getState() throws JSONException {
        WindowInsetsCompat insets = ViewCompat.getRootWindowInsets(requireWindow().getDecorView());
        // Insets can be unavailable before attachment. On API 29 and earlier, AndroidX
        // derives visibility from the available inset information rather than a direct OS flag.
        Boolean actualStatus = insets == null ? null
                : insets.isVisible(WindowInsetsCompat.Type.statusBars());
        Boolean actualNavigation = insets == null ? null
                : insets.isVisible(WindowInsetsCompat.Type.navigationBars());
        return new JSONObject()
                .put("requested", visibilityState(statusBarVisible, navigationBarVisible))
                .put("actual", visibilityState(actualStatus, actualNavigation));
    }

    @NonNull private static JSONObject visibilityState(@Nullable Boolean status,
            @Nullable Boolean navigation) throws JSONException {
        return new JSONObject()
                .put("statusBar", new JSONObject().put("visible", status == null ? JSONObject.NULL : status))
                .put("navigationBar", new JSONObject().put("visible", navigation == null ? JSONObject.NULL : navigation));
    }

    @NonNull private Window requireWindow() {
        if (Looper.myLooper() != Looper.getMainLooper()) {
            throw new IllegalStateException("System bar operations require the Android main thread");
        }
        if (window == null) {
            throw new IllegalStateException("Android system bar service was released");
        }
        return window;
    }

    @Override
    public void close() {
        window = null;
    }
}

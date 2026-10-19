/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

package dev.muon.runtime;

import android.os.Build;
import android.os.Looper;
import android.view.Window;
import android.widget.FrameLayout;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.core.graphics.Insets;
import androidx.core.view.ViewCompat;
import androidx.core.view.WindowCompat;
import androidx.core.view.WindowInsetsAnimationCompat;
import androidx.core.view.WindowInsetsCompat;
import androidx.core.view.WindowInsetsControllerCompat;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;
import org.json.JSONTokener;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Iterator;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.UUID;

/** Owns requested bars, observed window insets and document-scoped inset subscriptions. */
final class MuonAndroidSystemBarsService implements AutoCloseable {
    private static final class Pending {
        final String operation;
        @Nullable final String token;
        final MuonAndroidPlatformService.Completion completion;

        Pending(String operation, @Nullable String token,
                MuonAndroidPlatformService.Completion completion) {
            this.operation = operation;
            this.token = token;
            this.completion = completion;
        }
    }

    private Window window;
    private FrameLayout content;
    private Runnable changed;
    private boolean statusBarVisible = true;
    private boolean navigationBarVisible = true;
    @Nullable private String statusBarIcons;
    @Nullable private String navigationBarIcons;
    private String contentLayout = "safe-area";
    private float scale;
    @Nullable private WindowInsetsCompat observedInsets;
    @Nullable private WindowInsetsCompat appliedInsets;
    private final Set<WindowInsetsAnimationCompat> animations = new HashSet<>();
    @Nullable private JSONObject snapshot;
    private String snapshotContents = "";
    private long revision;
    private final Map<Integer, Pending> pending = new HashMap<>();
    private final Set<String> leases = new HashSet<>();

    MuonAndroidSystemBarsService(@NonNull Window window, float initialScale,
            @NonNull JSONObject config, @Nullable String retainedState) {
        this.window = window;
        scale = initialScale;
        // Apply our own safe-area padding so the same window insets can also be exposed
        // in edge-to-edge mode without relying on WebView's viewport-fit behavior.
        WindowCompat.enableEdgeToEdge(window);
        try {
            configure(retainedState == null ? config.toString() : retainedState);
        } catch (JSONException error) {
            throw new IllegalArgumentException("Invalid initial system bar options", error);
        }
    }

    void attach(@NonNull FrameLayout content, @NonNull Runnable changed) {
        requireWindow();
        this.content = content;
        this.changed = changed;
        ViewCompat.setOnApplyWindowInsetsListener(content, (view, insets) -> {
            appliedInsets = insets;
            if (animations.isEmpty()) { observeInsets(insets); }
            Insets padding = nativePadding(insets);
            // Children receive only the remainder; otherwise WebView can apply a second inset.
            return insets.inset(padding.left, padding.top, padding.right, padding.bottom);
        });
        ViewCompat.setWindowInsetsAnimationCallback(content,
                new WindowInsetsAnimationCompat.Callback(
                        WindowInsetsAnimationCompat.Callback.DISPATCH_MODE_CONTINUE_ON_SUBTREE) {
                    @Override
                    public void onPrepare(@NonNull WindowInsetsAnimationCompat animation) {
                        animations.add(animation);
                    }

                    @Override @NonNull
                    public WindowInsetsCompat onProgress(@NonNull WindowInsetsCompat insets,
                            @NonNull List<WindowInsetsAnimationCompat> runningAnimations) {
                        observeInsets(insets);
                        Insets padding = nativePadding(insets);
                        return insets.inset(padding.left, padding.top, padding.right, padding.bottom);
                    }

                    @Override
                    public void onEnd(@NonNull WindowInsetsAnimationCompat animation) {
                        animations.remove(animation);
                        if (animations.isEmpty() && appliedInsets != null) { observeInsets(appliedInsets); }
                    }
                });
    }

    void onScaleChanged(float newScale) {
        requireWindow();
        if (Float.isFinite(newScale) && newScale > 0 && scale != newScale) {
            scale = newScale;
            publishSnapshot();
        }
    }

    @Nullable String getCssScript() {
        if (snapshot == null) {
            return null;
        }
        JSONObject area = snapshot.optJSONObject("contentSafeArea");
        // This script only installs CSS variables and never exposes an RPC or callback.
        return "(() => { const area=" + area + "; const apply=()=>{"
                + "if(!document.documentElement)return false;"
                + "for(const [edge,value] of Object.entries(area))"
                + "document.documentElement.style.setProperty('--muon-safe-area-'+edge,value+'px');"
                + "return true;}; if(!apply()){const observer=new MutationObserver(()=>{"
                + "if(apply())observer.disconnect();});observer.observe(document,{childList:true,subtree:true});} })();";
    }

    @NonNull String retainRequestedState() {
        try {
            JSONObject state = requestedState();
            // Public configure deliberately rejects null. Omission retains the theme default.
            if (statusBarIcons == null) { state.getJSONObject("statusBar").remove("icons"); }
            if (navigationBarIcons == null) { state.getJSONObject("navigationBar").remove("icons"); }
            return state.toString();
        } catch (JSONException error) {
            throw new IllegalStateException(error);
        }
    }

    void invoke(int callId, @NonNull String functionPath, @NonNull JSONArray arguments,
            @NonNull byte[][] attachments,
            @NonNull MuonAndroidPlatformService.Completion completion) {
        try {
            requireWindow();
            if (attachments.length != 0) {
                throw new IllegalArgumentException(functionPath + " does not accept binary arguments");
            }
            switch (functionPath) {
                case "muon.android.systemBars.configure":
                    configure(stringArgument(functionPath, arguments));
                    completion.completeVoid();
                    return;
                case "muon.android.systemBars.getState":
                    noArguments(functionPath, arguments);
                    completion.completeString(getState().toString());
                    return;
                case "muon.android.systemBars.getInsets":
                    noArguments(functionPath, arguments);
                    if (snapshot == null) {
                        pending.put(callId, new Pending("get", null, completion));
                    } else {
                        completion.completeString(snapshot.toString());
                    }
                    return;
                case "muon.android.systemBars.watchInsets":
                    watch(callId, object(stringArgument(functionPath, arguments)), completion);
                    return;
                default:
                    throw new IllegalArgumentException("Unknown Android system bar function: " + functionPath);
            }
        } catch (JSONException | RuntimeException error) {
            String message = error.getMessage();
            completion.fail(message == null ? error.getClass().getSimpleName() : message);
        }
    }

    private void watch(int callId, JSONObject options,
            MuonAndroidPlatformService.Completion completion) throws JSONException {
        Object operationValue = options.opt("operation");
        if (!(operationValue instanceof String)) {
            throw new IllegalArgumentException("Insets operation must be a string");
        }
        String operation = (String) operationValue;
        if (operation.equals("acquire")) {
            keys(options, "operation");
            String token = UUID.randomUUID().toString();
            leases.add(token);
            if (snapshot == null) {
                pending.put(callId, new Pending("acquire", token, completion));
            } else {
                completion.completeString(new JSONObject().put("token", token)
                        .put("insets", snapshot).toString());
            }
            return;
        }
        if (!operation.equals("next") && !operation.equals("release")) {
            throw new IllegalArgumentException("Unknown insets operation: " + operation);
        }
        if (operation.equals("next")) {
            keys(options, "operation", "token", "afterRevision");
        } else {
            keys(options, "operation", "token");
        }
        Object tokenValue = options.opt("token");
        if (!(tokenValue instanceof String) || ((String) tokenValue).isEmpty()) {
            throw new IllegalArgumentException("Insets subscription token must be a nonempty string");
        }
        String token = (String) tokenValue;
        if (operation.equals("release")) {
            leases.remove(token);
            for (Map.Entry<Integer, Pending> entry : new ArrayList<>(pending.entrySet())) {
                if (token.equals(entry.getValue().token)) {
                    pending.remove(entry.getKey());
                    entry.getValue().completion.completeString("{\"insets\":null}");
                }
            }
            completion.completeVoid();
            return;
        }
        if (!leases.contains(token)) {
            throw new IllegalArgumentException("Insets subscription is not active");
        }
        Object after = options.opt("afterRevision");
        if (!(after instanceof Number) || !Double.isFinite(((Number) after).doubleValue())
                || ((Number) after).doubleValue() != ((Number) after).longValue()
                || ((Number) after).longValue() < 0 || ((Number) after).longValue() > revision) {
            throw new IllegalArgumentException("Insets afterRevision must be an observed nonnegative revision");
        }
        for (Pending value : pending.values()) {
            if (token.equals(value.token)) {
                throw new IllegalArgumentException("Insets subscription already has a pending call");
            }
        }
        if (snapshot != null && ((Number) after).longValue() < revision) {
            completion.completeString(new JSONObject().put("insets", snapshot).toString());
        } else {
            pending.put(callId, new Pending("next", token, completion));
        }
    }

    void cancel(int callId) {
        Pending removed = pending.remove(callId);
        if (removed != null && removed.operation.equals("acquire")) {
            leases.remove(removed.token);
        }
    }

    void cancelAll() {
        pending.clear();
        leases.clear();
    }

    int getActiveWatchCount() { return leases.size(); }

    boolean isFullscreen() { return !statusBarVisible && !navigationBarVisible; }

    void setFullscreen(boolean fullscreen) {
        applyVisibility(!fullscreen, !fullscreen,
                WindowInsetsCompat.Type.statusBars() | WindowInsetsCompat.Type.navigationBars());
    }

    private void configure(@NonNull String json) throws JSONException {
        JSONObject options = object(json);
        keys(options, "contentLayout", "statusBar", "navigationBar");
        String layout = enumOption(options, "contentLayout", "safe-area", "edge-to-edge");
        JSONObject status = barOptions(options, "statusBar");
        JSONObject navigation = barOptions(options, "navigationBar");
        Boolean statusVisible = visibility(status);
        Boolean navigationVisible = visibility(navigation);
        String statusIcons = enumOption(status, "icons", "light", "dark");
        String navigationIcons = enumOption(navigation, "icons", "light", "dark");
        if (Build.VERSION.SDK_INT < 26 && "dark".equals(navigationIcons)) {
            throw new IllegalArgumentException("Dark navigation bar icons require Android API 26 or later");
        }

        // Validate every partial update before changing any requested state or Window setting.
        Window current = requireWindow();
        WindowInsetsControllerCompat controller = WindowCompat.getInsetsController(current, current.getDecorView());
        if (statusIcons != null) {
            controller.setAppearanceLightStatusBars(statusIcons.equals("dark"));
            statusBarIcons = statusIcons;
        }
        if (navigationIcons != null) {
            controller.setAppearanceLightNavigationBars(navigationIcons.equals("dark"));
            navigationBarIcons = navigationIcons;
        }
        if (layout != null) { contentLayout = layout; }
        int changedTypes = (statusVisible == null ? 0 : WindowInsetsCompat.Type.statusBars())
                | (navigationVisible == null ? 0 : WindowInsetsCompat.Type.navigationBars());
        applyVisibility(statusVisible == null ? statusBarVisible : statusVisible,
                navigationVisible == null ? navigationBarVisible : navigationVisible, changedTypes);
        if (observedInsets != null) { applyPadding(observedInsets); }
        publishSnapshot();
        if (content != null) { ViewCompat.requestApplyInsets(content); }
    }

    @NonNull private static JSONObject object(String json) throws JSONException {
        JSONTokener parser = new JSONTokener(json);
        Object value = parser.nextValue();
        if (!(value instanceof JSONObject) || parser.nextClean() != 0) {
            throw new IllegalArgumentException("System bar options must be a JSON object");
        }
        return (JSONObject) value;
    }

    private static void keys(JSONObject options, String... allowed) {
        for (Iterator<String> keys = options.keys(); keys.hasNext();) {
            String key = keys.next();
            boolean found = false;
            for (String candidate : allowed) { found |= candidate.equals(key); }
            if (!found) { throw new IllegalArgumentException("Unknown system bar option: " + key); }
        }
    }

    @NonNull private static JSONObject barOptions(JSONObject options, String key) throws JSONException {
        if (!options.has(key)) { return new JSONObject(); }
        Object value = options.get(key);
        if (!(value instanceof JSONObject)) { throw new IllegalArgumentException(key + " must be an object"); }
        JSONObject bar = (JSONObject) value;
        keys(bar, "visible", "icons");
        return bar;
    }

    @Nullable private static Boolean visibility(JSONObject bar) throws JSONException {
        if (!bar.has("visible")) { return null; }
        Object value = bar.get("visible");
        if (!(value instanceof Boolean)) { throw new IllegalArgumentException("visible must be a boolean"); }
        return (Boolean) value;
    }

    @Nullable private static String enumOption(JSONObject options, String key,
            String first, String second) throws JSONException {
        if (!options.has(key)) { return null; }
        Object value = options.get(key);
        if (!(value instanceof String) || (!value.equals(first) && !value.equals(second))) {
            throw new IllegalArgumentException(key + " must be " + first + " or " + second);
        }
        return (String) value;
    }

    @NonNull private static String stringArgument(String path, JSONArray arguments) throws JSONException {
        if (arguments.length() != 1 || !(arguments.get(0) instanceof String)) {
            throw new IllegalArgumentException(path + " requires one JSON string");
        }
        return arguments.getString(0);
    }

    private static void noArguments(String path, JSONArray arguments) {
        if (arguments.length() != 0) { throw new IllegalArgumentException(path + " does not accept arguments"); }
    }

    private void applyVisibility(boolean status, boolean navigation, int changedTypes) {
        Window current = requireWindow();
        if (changedTypes == 0) { return; }
        WindowInsetsControllerCompat controller = WindowCompat.getInsetsController(current, current.getDecorView());
        controller.setSystemBarsBehavior(WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        int visibleTypes = (status ? WindowInsetsCompat.Type.statusBars() : 0)
                | (navigation ? WindowInsetsCompat.Type.navigationBars() : 0);
        int showTypes = changedTypes & visibleTypes;
        int hideTypes = changedTypes & ~visibleTypes;
        if (showTypes != 0) { controller.show(showTypes); }
        if (hideTypes != 0) { controller.hide(hideTypes); }
        statusBarVisible = status;
        navigationBarVisible = navigation;
    }

    @NonNull private JSONObject getState() throws JSONException {
        Window current = requireWindow();
        WindowInsetsCompat insets = ViewCompat.getRootWindowInsets(current.getDecorView());
        WindowInsetsControllerCompat controller = WindowCompat.getInsetsController(current, current.getDecorView());
        // Insets can be unavailable before attachment. On API 29 and earlier, AndroidX
        // derives visibility from the available inset information rather than a direct OS flag.
        // Transient overlays are not always reflected in these public inset observations;
        // they are not a measurement of the system UI's composited screen pixels.
        return new JSONObject().put("requested", requestedState())
                .put("actual", new JSONObject()
                        .put("statusBar", barState(insets == null ? null : insets.isVisible(WindowInsetsCompat.Type.statusBars()),
                                insets == null ? null : controller.isAppearanceLightStatusBars() ? "dark" : "light"))
                        .put("navigationBar", barState(insets == null ? null : insets.isVisible(WindowInsetsCompat.Type.navigationBars()),
                                insets == null ? null : controller.isAppearanceLightNavigationBars() ? "dark" : "light")));
    }

    @NonNull private JSONObject requestedState() throws JSONException {
        return new JSONObject().put("contentLayout", contentLayout)
                .put("statusBar", barState(statusBarVisible, statusBarIcons))
                .put("navigationBar", barState(navigationBarVisible, navigationBarIcons));
    }

    @NonNull private static JSONObject barState(@Nullable Boolean visible, @Nullable String icons) throws JSONException {
        return new JSONObject().put("visible", visible == null ? JSONObject.NULL : visible)
                .put("icons", icons == null ? JSONObject.NULL : icons);
    }

    private void observeInsets(WindowInsetsCompat insets) {
        observedInsets = insets;
        applyPadding(insets);
        publishSnapshot();
    }

    private Insets nativePadding(WindowInsetsCompat insets) {
        return contentLayout.equals("safe-area")
                ? insets.getInsets(WindowInsetsCompat.Type.systemBars() | WindowInsetsCompat.Type.displayCutout())
                : Insets.NONE;
    }

    private void applyPadding(WindowInsetsCompat insets) {
        if (content == null) { return; }
        Insets padding = nativePadding(insets);
        content.setPadding(padding.left, padding.top, padding.right, padding.bottom);
    }

    private void publishSnapshot() {
        if (observedInsets == null || window == null) { return; }
        try {
            Insets safe = observedInsets.getInsets(WindowInsetsCompat.Type.systemBars() | WindowInsetsCompat.Type.displayCutout());
            JSONObject next = new JSONObject().put("unit", "css-px").put("contentLayout", contentLayout)
                    .put("systemBars", edges(observedInsets.getInsets(WindowInsetsCompat.Type.systemBars())))
                    .put("displayCutout", edges(observedInsets.getInsets(WindowInsetsCompat.Type.displayCutout())))
                    .put("systemGestures", edges(observedInsets.getInsets(WindowInsetsCompat.Type.systemGestures())))
                    .put("ime", edges(observedInsets.getInsets(WindowInsetsCompat.Type.ime())))
                    .put("safeArea", edges(safe))
                    .put("contentSafeArea", edges(contentLayout.equals("safe-area") ? Insets.NONE : safe));
            String contents = next.toString();
            if (contents.equals(snapshotContents)) { return; }
            snapshotContents = contents;
            snapshot = next.put("revision", ++revision);
            // Install the CSS before callbacks can observe the new snapshot.
            if (changed != null) { changed.run(); }
            for (Map.Entry<Integer, Pending> entry : new ArrayList<>(pending.entrySet())) {
                pending.remove(entry.getKey());
                Pending call = entry.getValue();
                if (call.operation.equals("get")) {
                    call.completion.completeString(snapshot.toString());
                } else {
                    JSONObject result = new JSONObject().put("insets", snapshot);
                    if (call.operation.equals("acquire")) { result.put("token", call.token); }
                    call.completion.completeString(result.toString());
                }
            }
        } catch (JSONException error) {
            throw new IllegalStateException("Could not serialize Android window insets", error);
        }
    }

    private JSONObject edges(Insets value) throws JSONException {
        return new JSONObject().put("left", value.left / (double) scale).put("top", value.top / (double) scale)
                .put("right", value.right / (double) scale).put("bottom", value.bottom / (double) scale);
    }

    @NonNull private Window requireWindow() {
        if (Looper.myLooper() != Looper.getMainLooper()) {
            throw new IllegalStateException("System bar operations require the Android main thread");
        }
        if (window == null) { throw new IllegalStateException("Android system bar service was released"); }
        return window;
    }

    @Override public void close() {
        cancelAll();
        if (content != null) {
            ViewCompat.setOnApplyWindowInsetsListener(content, null);
            ViewCompat.setWindowInsetsAnimationCallback(content, null);
        }
        content = null;
        animations.clear();
        changed = null;
        window = null;
    }
}

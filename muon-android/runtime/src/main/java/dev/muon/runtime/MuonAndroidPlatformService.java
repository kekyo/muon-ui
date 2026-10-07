/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

package dev.muon.runtime;

import android.app.Activity;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.Process;
import android.webkit.WebView;

import androidx.annotation.NonNull;
import androidx.core.view.WindowCompat;
import androidx.core.view.WindowInsetsCompat;
import androidx.core.view.WindowInsetsControllerCompat;
import androidx.webkit.WebViewCompat;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.util.Map;
import java.util.TreeMap;

/** Performs Android operations selected by the CEF-independent RPC host. */
final class MuonAndroidPlatformService implements AutoCloseable {
    interface Completion {
        void completeVoid();

        void completeString(@NonNull String value);

        void completeUnsignedInteger(long value);

        void completeBoolean(boolean value);

        void completeBinary(@NonNull byte[] value);

        void fail(@NonNull String diagnostic);
    }

    private static final float ZOOM_STEP = 1.2f;
    private static final float MINIMUM_ZOOM = 0.25f;
    private static final float MAXIMUM_ZOOM = 5.0f;

    private final JSONObject configValues;
    private Activity activity;
    private WebView webView;
    private final MuonAndroidFilesystemService filesystemService;
    private boolean fullscreen;
    private float managedZoomFactor = 1.0f;

    MuonAndroidPlatformService(
            @NonNull Activity activity,
            @NonNull WebView webView) {
        this.activity = activity;
        configValues = MuonAppConfig.load(activity).values;
        this.webView = webView;
        filesystemService = new MuonAndroidFilesystemService(
                new Handler(Looper.getMainLooper()));
    }

    void invoke(
            int callId,
            @NonNull String functionPath,
            @NonNull JSONArray arguments,
            @NonNull byte[][] attachments,
            @NonNull Completion completion) {
        if (activity == null || webView == null) {
            completion.fail("Android platform service was released");
            return;
        }
        if (functionPath.startsWith("muon.fs.")) {
            filesystemService.invoke(
                    callId, functionPath, arguments, attachments, completion);
            return;
        }
        if (arguments.length() != 0 || attachments.length != 0) {
            completion.fail(functionPath + " does not accept arguments");
            return;
        }

        try {
            switch (functionPath) {
                case "muon.environments.getVariables":
                    completion.completeString(createEnvironmentVariables().toString());
                    return;
                case "muon.environments.getConfigValues":
                    completion.completeString(createConfigValues().toString());
                    return;
                case "muon.environments.getProcessId":
                    completion.completeUnsignedInteger(Integer.toUnsignedLong(Process.myPid()));
                    return;
                case "muon.environments.getRuntimeInfo":
                    completion.completeString(createRuntimeInfo().toString());
                    return;
                case "muon.browser.reload":
                    webView.reload();
                    completion.completeVoid();
                    return;
                case "muon.browser.toggleFullscreen":
                    setFullscreen(!fullscreen);
                    completion.completeVoid();
                    return;
                case "muon.browser.enterFullscreen":
                    setFullscreen(true);
                    completion.completeVoid();
                    return;
                case "muon.browser.exitFullscreen":
                    setFullscreen(false);
                    completion.completeVoid();
                    return;
                case "muon.browser.zoomIn":
                    setManagedZoom(Math.min(MAXIMUM_ZOOM, managedZoomFactor * ZOOM_STEP));
                    completion.completeVoid();
                    return;
                case "muon.browser.zoomOut":
                    setManagedZoom(Math.max(MINIMUM_ZOOM, managedZoomFactor / ZOOM_STEP));
                    completion.completeVoid();
                    return;
                case "muon.browser.resetZoom":
                    setManagedZoom(1.0f);
                    completion.completeVoid();
                    return;
                case "muon.browser.close":
                    activity.finish();
                    completion.completeVoid();
                    return;
                default:
                    completion.fail("Unknown Android platform function: " + functionPath);
            }
        } catch (JSONException | RuntimeException error) {
            String message = error.getMessage();
            completion.fail(message == null ? error.getClass().getSimpleName() : message);
        }
    }

    void cancel(int callId) {
        filesystemService.cancel(callId);
    }

    void cancelAll() {
        filesystemService.cancelAll();
    }

    boolean isFullscreen() {
        return fullscreen;
    }

    float getManagedZoomFactor() {
        return managedZoomFactor;
    }

    int getActiveFilesystemWatchCount() {
        return filesystemService.getActiveWatchCount();
    }

    @Override
    public void close() {
        filesystemService.close();
        activity = null;
        webView = null;
    }

    @NonNull private JSONObject createEnvironmentVariables() throws JSONException {
        JSONObject result = new JSONObject();
        for (Map.Entry<String, String> entry :
                new TreeMap<>(System.getenv()).entrySet()) {
            result.put(entry.getKey(), entry.getValue());
        }
        return result;
    }

    @NonNull private JSONObject createConfigValues() {
        return configValues;
    }

    @NonNull private JSONObject createRuntimeInfo() throws JSONException {
        PackageInfo webViewPackage = WebViewCompat.getCurrentWebViewPackage(activity);
        String packageName = webViewPackage == null ? "" : webViewPackage.packageName;
        String versionName = webViewPackage == null ? "" : webViewPackage.versionName;
        String abi = Build.SUPPORTED_ABIS.length == 0 ? "" : Build.SUPPORTED_ABIS[0];

        JSONObject result = new JSONObject();
        result.put("backend", "android-webview");
        result.put("os", "android");
        result.put("osVersion", Build.VERSION.RELEASE);
        result.put("apiLevel", Build.VERSION.SDK_INT);
        result.put("abi", abi);
        result.put("applicationId", activity.getPackageName());
        try {
            PackageInfo application = activity.getPackageManager().getPackageInfo(
                    activity.getPackageName(), 0);
            result.put("applicationVersion", application.versionName);
        } catch (PackageManager.NameNotFoundException error) {
            throw new IllegalStateException("Installed application metadata is unavailable", error);
        }
        result.put("webViewPackage", packageName == null ? "" : packageName);
        result.put("webViewVersion", versionName == null ? "" : versionName);
        return result;
    }

    private void setFullscreen(boolean value) {
        WindowInsetsControllerCompat controller = WindowCompat.getInsetsController(
                activity.getWindow(), activity.getWindow().getDecorView());
        controller.setSystemBarsBehavior(
                WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        if (value) {
            controller.hide(WindowInsetsCompat.Type.systemBars());
        } else {
            controller.show(WindowInsetsCompat.Type.systemBars());
        }
        fullscreen = value;
    }

    private void setManagedZoom(float value) {
        if (value == managedZoomFactor) {
            return;
        }
        webView.zoomBy(value / managedZoomFactor);
        managedZoomFactor = value;
    }
}

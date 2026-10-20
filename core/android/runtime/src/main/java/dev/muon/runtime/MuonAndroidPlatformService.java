/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

package dev.muon.runtime;

import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.Process;
import android.webkit.WebView;

import androidx.annotation.NonNull;
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
    private final MuonAndroidSystemBarsService systemBarsService;
    private final MuonAndroidSettingsService settingsService;
    private final MuonAndroidNotificationPermissionService notificationPermissionService;
    private final MuonAndroidNotificationService notificationService;
    private float managedZoomFactor = 1.0f;

    MuonAndroidPlatformService(
            @NonNull Activity activity,
            @NonNull WebView webView,
            @NonNull MuonAppConfig config) {
        this.activity = activity;
        configValues = config.values;
        this.webView = webView;
        filesystemService = new MuonAndroidFilesystemService(
                new Handler(Looper.getMainLooper()), activity.getFilesDir());
        Object retained = activity.getLastNonConfigurationInstance();
        systemBarsService = new MuonAndroidSystemBarsService(activity.getWindow(),
                activity.getResources().getDisplayMetrics().density,
                config.systemBars,
                retained instanceof String ? (String) retained : null);
        notificationService = new MuonAndroidNotificationService(activity, config);
        settingsService = new MuonAndroidSettingsService(activity, notificationService::hasChannel);
        notificationPermissionService = new MuonAndroidNotificationPermissionService(activity, config.notificationsEnabled);
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
        if (functionPath.startsWith("muon.android.systemBars.")) {
            systemBarsService.invoke(callId, functionPath, arguments, attachments, completion);
            return;
        }
        if (functionPath.startsWith("muon.android.settings.")) {
            settingsService.invoke(functionPath, arguments, attachments, completion);
            return;
        }
        if (functionPath.equals("muon.android.notifications.getPermissionState")
                || functionPath.equals("muon.android.notifications.requestPermission")) {
            notificationPermissionService.invoke(callId, functionPath, arguments, attachments, completion);
            return;
        }
        if (functionPath.startsWith("muon.notifications.") || functionPath.startsWith("muon.android.notifications.")) {
            notificationService.invoke(callId, functionPath, arguments, attachments, completion);
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
                    systemBarsService.setFullscreen(!systemBarsService.isFullscreen());
                    completion.completeVoid();
                    return;
                case "muon.browser.enterFullscreen":
                    systemBarsService.setFullscreen(true);
                    completion.completeVoid();
                    return;
                case "muon.browser.exitFullscreen":
                    systemBarsService.setFullscreen(false);
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
        systemBarsService.cancel(callId);
        notificationPermissionService.cancel(callId);
        notificationService.cancel(callId);
    }

    void cancelAll() {
        filesystemService.cancelAll();
        systemBarsService.cancelAll();
        notificationPermissionService.cancelAll();
        notificationService.cancelAll();
    }

    void setResumed(boolean value) {
        settingsService.setResumed(value);
        notificationPermissionService.setResumed(value);
    }

    void onNotificationIntent(Intent intent) { notificationService.onNotificationIntent(intent); }
    int getActiveNotificationWatchCount() { return notificationService.getActiveWatchCount(); }

    boolean onRequestPermissionsResult(int code, @NonNull String[] permissions, @NonNull int[] results) {
        return notificationPermissionService.onRequestPermissionsResult(code, permissions, results);
    }

    MuonAndroidSystemBarsService getSystemBarsService() { return systemBarsService; }

    boolean isFullscreen() {
        return systemBarsService.isFullscreen();
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
        systemBarsService.close();
        settingsService.close();
        notificationPermissionService.close();
        notificationService.close();
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

    private void setManagedZoom(float value) {
        if (value == managedZoomFactor) {
            return;
        }
        webView.zoomBy(value / managedZoomFactor);
        managedZoomFactor = value;
    }
}

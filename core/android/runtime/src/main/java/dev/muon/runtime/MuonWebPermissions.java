/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
package dev.muon.runtime;

import android.Manifest;
import android.app.Activity;
import android.content.pm.PackageManager;
import android.os.Build;
import android.webkit.GeolocationPermissions;
import android.webkit.WebChromeClient;
import android.webkit.WebView;

/** Owns WebView geolocation continuations and retries a hosted page after Android grants LAN access. */
final class MuonWebPermissions extends WebChromeClient {
    private static final int LOCATION = 0x4621, NETWORK = 0x4622;
    private final Activity activity;
    private final WebView view;
    private final MuonWebDeployment deployment;
    private final Runnable retry;
    private String requestingOrigin;
    private GeolocationPermissions.Callback location;
    private boolean locationDialog, networkDialog;
    boolean resumed;

    MuonWebPermissions(Activity activity, WebView view, MuonWebDeployment deployment, Runnable retry) {
        this.activity = activity;
        this.view = view;
        this.deployment = deployment;
        this.retry = retry;
    }

    private boolean granted(String name) {
        return activity.checkSelfPermission(name) == PackageManager.PERMISSION_GRANTED;
    }
    private boolean hasLocation() {
        for (String name : deployment.locationPermissions) if (granted(name)) return true;
        return false;
    }
    @Override public void onGeolocationPermissionsShowPrompt(String origin, GeolocationPermissions.Callback callback) {
        cancelLocation();
        if (!resumed || !deployment.trusted(origin) || !deployment.trusted(view.getUrl()) || deployment.locationPermissions.length == 0) {
            callback.invoke(origin, false, false);
            return;
        }
        if (hasLocation()) { callback.invoke(origin, true, false); return; }
        requestingOrigin = origin;
        location = callback;
        if (locationDialog) return;
        locationDialog = true;
        try { activity.requestPermissions(deployment.locationPermissions, LOCATION); }
        catch (RuntimeException error) { locationDialog = false; cancelLocation(); }
    }
    @Override public void onGeolocationPermissionsHidePrompt() { cancelLocation(); }
    void cancelLocation() { completeLocation(false); }
    private void completeLocation(boolean allow) {
        GeolocationPermissions.Callback reply = location;
        String origin = requestingOrigin;
        location = null;
        requestingOrigin = null;
        if (reply != null) reply.invoke(origin, allow, false);
    }

    boolean needsNetworkPermission() {
        return Build.VERSION.SDK_INT >= 37 && deployment.localNetworkPermission && !granted(Manifest.permission.ACCESS_LOCAL_NETWORK);
    }
    void requestNetwork() {
        if (!resumed || networkDialog || !needsNetworkPermission()) return;
        networkDialog = true;
        try { activity.requestPermissions(new String[]{Manifest.permission.ACCESS_LOCAL_NETWORK}, NETWORK); }
        catch (RuntimeException error) { networkDialog = false; }
    }
    boolean completed(int code) {
        if (code == LOCATION && locationDialog) {
            locationDialog = false;
            completeLocation(hasLocation() && deployment.trusted(view.getUrl()));
            return true;
        }
        if (code == NETWORK && networkDialog) {
            networkDialog = false;
            if (!needsNetworkPermission()) retry.run();
            return true;
        }
        return false;
    }
}

/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

package dev.muon.runtime;

import android.app.Activity;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.Bundle;
import android.util.Log;
import android.webkit.WebView;
import androidx.core.content.ContextCompat;
import org.json.JSONObject;

/** Creates independently owned WebViews for the Playwright MCP service tests. */
public final class MuonServiceRpcActivity extends Activity {
    private MuonWebViewHost host;
    private MuonRpcBridge bridge;
    private final BroadcastReceiver closeForTest = new BroadcastReceiver() {
        @Override public void onReceive(Context context, Intent intent) {
            if (java.util.Objects.equals(getIntent().getStringExtra("owner"), intent.getStringExtra("owner"))) finishAndRemoveTask();
        }
    };
    private final java.util.concurrent.CountDownLatch pageReady = new java.util.concurrent.CountDownLatch(1);

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        ContextCompat.registerReceiver(this, closeForTest, new IntentFilter("dev.muon.servicefixture.CLOSE_PAGE"), ContextCompat.RECEIVER_EXPORTED);
        if (!getIntent().getBooleanExtra("deferHost", false)) startHostForTest();
    }

    void startHostForTest() {
        // Debugging is enabled only by this test fixture, never by the runtime.
        WebView.setWebContentsDebuggingEnabled(true);
        String owner = getIntent().getStringExtra("owner");
        host = new MuonWebViewHost(this, new MuonWebViewHost.Listener() {
            @Override public void beforePageLoad(WebView view, MuonRpcBridge value) { bridge = value; }
            @Override public void pageFinished(String url) { Log.i("MuonServiceRpcReady", owner); pageReady.countDown(); }
            @Override public void mainFrameHttpError(int status) { Log.e("MuonServiceRpcReady", "HTTP " + status); }
            @Override public void startupFailed(String diagnostic) { Log.e("MuonServiceRpcReady", diagnostic); }
        });
        try {
            JSONObject config = new JSONObject(MuonAppConfig.readAsset(this, "muon/config.json"));
            config.put("startPage", MuonAppConfig.TRUSTED_ORIGIN + "/empty.html?owner=" + owner);
            host.start(MuonAppConfig.readAsset(this, "muon/renderer.js"), new MuonAppConfig(config));
            host.setResumed(true);
        } catch (Exception error) { throw new IllegalStateException(error); }
    }

    boolean awaitPageForTest() throws InterruptedException { return pageReady.await(30, java.util.concurrent.TimeUnit.SECONDS); }
    String runtimeDiagnosticsForTest() { return bridge.getNativeRuntimeDiagnosticsForTest(); }
    void closeHostForTest() { if (host != null) { host.close(false); host = null; bridge = null; } }

    @Override protected void onResume() { super.onResume(); if (host != null) host.setResumed(true); }
    @Override protected void onPause() { if (host != null) host.setResumed(false); super.onPause(); }
    @Override protected void onDestroy() {
        unregisterReceiver(closeForTest);
        closeHostForTest();
        Log.i("MuonServiceRpcReady", "closed:" + getIntent().getStringExtra("owner"));
        super.onDestroy();
    }
}

/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

package dev.muon.runtime;

import android.app.Activity;
import android.os.Bundle;
import android.webkit.WebView;
import androidx.annotation.NonNull;
import androidx.annotation.Nullable;

/** Hosts the packaged application using the Muon Android runtime. */
public final class MuonAppActivity extends Activity {
    private MuonWebViewHost host;

    @Override
    protected void onCreate(@Nullable Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        host = new MuonWebViewHost(this, new MuonWebViewHost.Listener() {
            @Override public void beforePageLoad(@NonNull WebView view, @NonNull MuonRpcBridge bridge) {}
            @Override public void pageFinished(@NonNull String url) {}
            @Override public void mainFrameHttpError(int status) {}
            @Override public void startupFailed(@NonNull String diagnostic) {}
        });
        try {
            host.start(MuonAppConfig.readAsset(this, "muon/renderer.js"));
        } catch (RuntimeException error) {
            host.showFailure(error.getMessage() == null
                    ? "Muon renderer is unavailable" : error.getMessage());
        }
    }

    @Override
    protected void onDestroy() {
        if (host != null) { host.close(isChangingConfigurations()); host = null; }
        super.onDestroy();
    }
}

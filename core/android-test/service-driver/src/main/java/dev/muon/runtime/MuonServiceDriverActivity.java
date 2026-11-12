package dev.muon.runtime;

import android.app.Activity;
import android.os.Bundle;
import android.os.Process;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;
import androidx.core.graphics.Insets;
import androidx.core.view.ViewCompat;
import androidx.core.view.WindowInsetsCompat;
import org.json.JSONException;
import org.json.JSONObject;

/** Observable UI owner; the separate observer may kill this entire process. */
public final class MuonServiceDriverActivity extends Activity {
    private MuonServiceClient client;
    private TextView status;

    @Override public void onCreate(Bundle saved) {
        super.onCreate(saved);
        status = new TextView(this);
        status.setContentDescription("service-driver-starting");
        status.setText("Starting independent native services");
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        ViewCompat.setOnApplyWindowInsetsListener(layout, (view, insets) -> {
            Insets bars = insets.getInsets(WindowInsetsCompat.Type.systemBars()
                    | WindowInsetsCompat.Type.displayCutout());
            view.setPadding(bars.left, bars.top, bars.right, bars.bottom);
            return WindowInsetsCompat.CONSUMED;
        });
        layout.addView(status);
        setContentView(layout);
        client = new MuonServiceClient(this);
        if ("reject".equals(getIntent().getStringExtra("mode"))) {
            rejectForegroundStart();
            return;
        }
        request("acquire", "alpha", alpha -> request("acquire", "beta", beta ->
                request("getState", "alpha", alphaState -> request("getState", "beta", betaState -> {
                    try {
                        JSONObject result = new JSONObject().put("uiPid", Process.myPid())
                                .put("alphaGeneration", alpha.getString("generation"))
                                .put("betaGeneration", beta.getString("generation"))
                                .put("alphaClientReferences", alphaState.getInt("clientReferences"))
                                .put("betaClientReferences", betaState.getInt("clientReferences"));
                        status.setText(result.toString());
                        status.setContentDescription("service-driver-ready");
                        Button release = new Button(this);
                        release.setText("Release beta");
                        release.setContentDescription("release-beta");
                        release.setOnClickListener(view -> releaseBeta(beta));
                        layout.addView(release);
                    } catch (JSONException error) { fail(error.toString()); }
                }))));
    }

    private interface Result { void complete(JSONObject value); }

    private void releaseBeta(JSONObject reference) {
        request("release", reference, first -> request("release", reference, second -> {
            try {
                status.setText(new JSONObject().put("firstRelease", first.getBoolean("released"))
                        .put("secondRelease", second.getBoolean("released")).toString());
                status.setContentDescription("service-driver-released");
            } catch (JSONException error) { fail(error.toString()); }
        }));
    }

    private void rejectForegroundStart() {
        try {
            client.invoke("acquire", new JSONObject().put("serviceId", "rejected"),
                    new MuonServiceClient.Completion() {
                @Override public void complete(JSONObject value) { fail("Android unexpectedly allowed camera FGS"); }
                @Override public void fail(String acquisitionError) {
                    request("getState", "rejected", snapshot -> {
                        try {
                            status.setText(snapshot.put("acquireError", acquisitionError).toString());
                            status.setContentDescription("service-driver-rejected");
                        } catch (JSONException error) { MuonServiceDriverActivity.this.fail(error.toString()); }
                    });
                }
            });
        } catch (JSONException error) { fail(error.toString()); }
    }

    private void request(String operation, String id, Result result) {
        try {
            request(operation, new JSONObject().put("serviceId", id), result);
        } catch (JSONException error) { fail(error.toString()); }
    }

    private void request(String operation, JSONObject arguments, Result result) {
            client.invoke(operation, arguments, new MuonServiceClient.Completion() {
                @Override public void complete(JSONObject value) { result.complete(value); }
                @Override public void fail(String error) { MuonServiceDriverActivity.this.fail(error); }
            });
    }

    private void fail(String error) {
        status.setText(error);
        status.setContentDescription("service-driver-error");
    }

    @Override public void onDestroy() {
        if (client != null) client.close();
        super.onDestroy();
    }
}

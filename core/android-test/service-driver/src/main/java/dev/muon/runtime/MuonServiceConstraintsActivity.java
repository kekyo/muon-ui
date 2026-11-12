package dev.muon.runtime;

import android.app.Activity;
import android.os.Bundle;
import android.widget.LinearLayout;
import android.widget.TextView;
import androidx.core.graphics.Insets;
import androidx.core.view.ViewCompat;
import androidx.core.view.WindowInsetsCompat;
import org.json.JSONObject;

/** Starts a selected fixture and displays coalesced state without acquiring it. */
public final class MuonServiceConstraintsActivity extends Activity {
    private MuonServiceClient client;
    private TextView status;
    private String id;
    private String token;
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        status = new TextView(this);
        status.setContentDescription("constraint-pending");
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
        id = getIntent().getStringExtra("service");
        try {
            client.invoke("setAutostart", new JSONObject().put("serviceId", id).put("enabled", true),
                    new MuonServiceClient.Completion() {
                @Override public void complete(JSONObject value) { watch(); }
                @Override public void fail(String error) { failure(error); }
            });
        } catch (Exception error) { failure(error.toString()); }
    }
    private void failure(String error) {
        status.setText(error);
        status.setContentDescription("constraint-error");
    }
    private void watch() {
        try {
            JSONObject arguments = new JSONObject().put("serviceId", id)
                    .put("operation", token == null ? "acquire" : "next");
            if (token != null) arguments.put("token", token);
            client.invoke("watchState", arguments, new MuonServiceClient.Completion() {
                @Override public void complete(JSONObject value) {
                    if (isDestroyed()) return;
                    try {
                        if (value.has("token")) token = value.getString("token");
                        JSONObject snapshot = value.getJSONObject("state");
                        status.setText(snapshot.toString());
                        status.setContentDescription("constraint-" + snapshot.getString("state"));
                        watch();
                    } catch (Exception error) { failure(error.toString()); }
                }
                @Override public void fail(String error) { if (!isDestroyed()) failure(error); }
            });
        } catch (Exception error) { failure(error.toString()); }
    }
    @Override public void onDestroy() {
        client.close();
        super.onDestroy();
    }
}

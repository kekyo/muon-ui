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
    private MuonServiceClient secondClient;
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
        String mode = getIntent().getStringExtra("mode");
        if ("preferences".equals(mode) || "inspect-preferences".equals(mode)) {
            preferences(layout, "inspect-preferences".equals(mode));
            return;
        }
        if ("reject".equals(getIntent().getStringExtra("mode"))) {
            rejectForegroundStart();
            return;
        }
        request("acquire", "alpha.v1-test", alpha -> request("acquire", "beta", beta ->
                request("getState", "alpha.v1-test", alphaState -> request("getState", "beta", betaState -> {
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

    private void preferences(LinearLayout layout, boolean inspect) {
        secondClient = new MuonServiceClient(this);
        TextView observation = new TextView(this);
        observation.setContentDescription("service-watch-starting");
        layout.addView(observation);
        Runnable watch = () -> {
            try {
                request("watchState", new JSONObject().put("serviceId", "alpha.v1-test")
                        .put("operation", "acquire"), result -> {
                    try {
                        String token = result.getString("token");
                        observe(result.getJSONObject("state"), observation);
                        nextState(token, observation);
                        reportPreference("preference-ready");
                        Button foreign = new Button(this);
                        foreign.setText("Foreign watch");
                        foreign.setContentDescription("foreign-watch");
                        foreign.setOnClickListener(view -> {
                            try {
                                secondClient.invoke("watchState", new JSONObject().put("serviceId", "alpha.v1-test")
                                        .put("operation", "next").put("token", token), new MuonServiceClient.Completion() {
                                    @Override public void complete(JSONObject value) { fail("Foreign owner used a watch token"); }
                                    @Override public void fail(String error) { reportPreference("foreign-rejected"); }
                                });
                            } catch (JSONException error) { fail(error.toString()); }
                        });
                        layout.addView(foreign);
                    } catch (JSONException error) { fail(error.toString()); }
                });
            } catch (JSONException error) { fail(error.toString()); }
        };
        for (boolean enabled : new boolean[]{false, true}) {
            Button toggle = new Button(this);
            toggle.setText(enabled ? "Enable autostart" : "Disable autostart");
            toggle.setContentDescription(enabled ? "autostart-on" : "autostart-off");
            toggle.setOnClickListener(view -> {
                try {
                    client.invoke("setAutostart", new JSONObject().put("serviceId", "alpha.v1-test")
                            .put("enabled", enabled), new MuonServiceClient.Completion() {
                        @Override public void complete(JSONObject value) {
                            reportPreference(enabled ? "autostart-on" : "autostart-off");
                        }
                        @Override public void fail(String error) {
                            request("getState", "alpha.v1-test", state -> {
                                try {
                                    status.setText(state.put("saveError", error).toString());
                                    status.setContentDescription("service-driver-save-failed");
                                } catch (JSONException invalid) { MuonServiceDriverActivity.this.fail(invalid.toString()); }
                            });
                        }
                    });
                } catch (JSONException error) { fail(error.toString()); }
            });
            layout.addView(toggle);
        }
        Button stop = new Button(this);
        stop.setText("Stop Android component");
        stop.setContentDescription("os-stop");
        stop.setOnClickListener(view -> stopService(new android.content.Intent(this, MuonFixtureAlphaService.class)));
        layout.addView(stop);
        if (inspect) { watch.run(); return; }
        request("acquire", "alpha.v1-test", first -> {
            try {
                secondClient.invoke("acquire", new JSONObject().put("serviceId", "alpha.v1-test"), new MuonServiceClient.Completion() {
                    @Override public void complete(JSONObject second) {
                        addRelease(layout, "release-first", client, first);
                        addRelease(layout, "release-second", secondClient, second);
                        watch.run();
                    }
                    @Override public void fail(String error) { MuonServiceDriverActivity.this.fail(error); }
                });
            } catch (JSONException error) { fail(error.toString()); }
        });
    }

    private void addRelease(LinearLayout layout, String name, MuonServiceClient owner, JSONObject reference) {
        Button button = new Button(this);
        button.setText(name);
        button.setContentDescription(name);
        button.setOnClickListener(view -> owner.invoke("release", reference, new MuonServiceClient.Completion() {
            @Override public void complete(JSONObject value) { reportPreference(name); }
            @Override public void fail(String error) { MuonServiceDriverActivity.this.fail(error); }
        }));
        layout.addView(button);
    }

    private void reportPreference(String name) {
        request("getState", "alpha.v1-test", state -> {
            try {
                status.setText(state.put("uiPid", Process.myPid()).toString());
                status.setContentDescription("service-driver-" + name);
            } catch (JSONException error) { fail(error.toString()); }
        });
    }

    private void observe(JSONObject state, TextView target) throws JSONException {
        target.setText(state.toString());
        target.setContentDescription("service-watch-" + state.getString("state"));
    }

    private void nextState(String token, TextView target) {
        try {
            request("watchState", new JSONObject().put("serviceId", "alpha.v1-test")
                    .put("operation", "next").put("token", token), result -> {
                if (result.isNull("state")) return;
                try {
                    observe(result.getJSONObject("state"), target);
                    nextState(token, target);
                } catch (JSONException error) { fail(error.toString()); }
            });
        } catch (JSONException error) { fail(error.toString()); }
    }

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
        if (secondClient != null) secondClient.close();
        super.onDestroy();
    }
}

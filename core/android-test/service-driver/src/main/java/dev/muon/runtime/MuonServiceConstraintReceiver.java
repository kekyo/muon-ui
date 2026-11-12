package dev.muon.runtime;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import org.json.JSONObject;

/** Debug-fixture entry without an Activity or an instrumented target process. */
public final class MuonServiceConstraintReceiver extends BroadcastReceiver {
    @Override public void onReceive(Context context, Intent intent) {
        if ("notifications".equals(intent.getStringExtra("operation"))) {
            try {
                org.json.JSONArray result = new org.json.JSONArray();
                for (android.service.notification.StatusBarNotification entry : context
                        .getSystemService(android.app.NotificationManager.class).getActiveNotifications()) {
                    android.app.Notification notification = entry.getNotification();
                    result.put(new JSONObject()
                            .put("title", notification.extras.getCharSequence(android.app.Notification.EXTRA_TITLE))
                            .put("foreground", (notification.flags & android.app.Notification.FLAG_FOREGROUND_SERVICE) != 0));
                }
                try (java.io.FileOutputStream output = context.openFileOutput("constraint-notifications.json", Context.MODE_PRIVATE)) {
                    output.write(result.toString().getBytes(java.nio.charset.StandardCharsets.UTF_8));
                }
                setResultCode(0);
            } catch (Exception error) { setResultCode(2); }
            return;
        }
        if ("release-start".equals(intent.getStringExtra("operation"))) {
            // The native gate is already open for reading and writing. A single
            // byte fits in the empty FIFO without blocking the receiver thread.
            java.io.File gate = new java.io.File(context.getFilesDir(), "services/held/start-gate");
            try {
                if (!gate.exists()) throw new java.io.IOException("Native gate is missing");
                try (java.io.FileOutputStream output = new java.io.FileOutputStream(gate)) { output.write(1); }
                setResultCode(0);
            } catch (Exception error) { setResultCode(2); }
            return;
        }
        PendingResult pending = goAsync();
        MuonServiceClient client = new MuonServiceClient(context);
        String id = intent.getStringExtra("service");
        boolean start = "start".equals(intent.getStringExtra("operation"));
        try {
            JSONObject arguments = new JSONObject().put("serviceId", id);
            if (start) arguments.put("enabled", true);
            client.invoke(start ? "setAutostart" : "getState", arguments, new MuonServiceClient.Completion() {
                private boolean finished;
                private void finish(JSONObject state, String error) {
                    if (finished) return;
                    finished = true;
                    try {
                        JSONObject result = new JSONObject().put("state", state).put("error", error);
                        try (java.io.FileOutputStream output = context.openFileOutput("constraint-result.json", Context.MODE_PRIVATE)) {
                            output.write(result.toString().getBytes(java.nio.charset.StandardCharsets.UTF_8));
                        }
                        pending.setResultCode(error == null ? 0 : 1);
                    } catch (Exception failure) { pending.setResultCode(2); }
                    client.close();
                    pending.finish();
                }
                @Override public void complete(JSONObject result) {
                    if (!start || result.optString("state").equals("failed")) { finish(result, null); return; }
                    try {
                        // A non-owning watch observes failure without retrying a
                        // failed generation or keeping the native instance alive.
                        if (result.has("token")) {
                            JSONObject state = result.getJSONObject("state");
                            if (state.getString("state").equals("failed")) { finish(state, null); return; }
                            token = result.getString("token");
                        } else if (result.has("state") && result.get("state") instanceof JSONObject) {
                            JSONObject state = result.getJSONObject("state");
                            if (state.getString("state").equals("failed")) { finish(state, null); return; }
                        }
                        JSONObject watch = new JSONObject().put("serviceId", id)
                                .put("operation", token == null ? "acquire" : "next");
                        if (token != null) watch.put("token", token);
                        client.invoke("watchState", watch, this);
                    } catch (Exception error) { fail(error.toString()); }
                }
                private String token;
                @Override public void fail(String error) { finish(null, error); }
            });
        } catch (Exception error) { client.close(); pending.setResultCode(2); pending.finish(); }
    }
}

/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
package dev.muon.runtime;

import android.content.Context;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;
import java.util.HashMap;
import java.util.Map;

/** One document's gravity-inclusive measurements, serialized on the Activity main thread. */
final class MuonAndroidAccelerometer {
    private static final int INTERVAL_US = 100_000;
    private final SensorManager manager;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Map<Integer, MuonAndroidPlatformService.Completion> pending = new HashMap<>();
    private SensorEventListener listener;
    private boolean requested, resumed;
    private long revision;
    private String state = "stopped", error;
    private JSONObject sample;

    MuonAndroidAccelerometer(Context context) {
        manager = context.getSystemService(SensorManager.class);
    }

    void invoke(int callId, String path, JSONArray args, byte[][] attachments,
            MuonAndroidPlatformService.Completion reply) {
        try {
            if (attachments.length != 0) throw new IllegalArgumentException("Unexpected accelerometer attachment");
            if (path.equals("muon.android.accelerometer.waitForChange")) {
                if (args.length() != 1 || !(args.get(0) instanceof Number))
                    throw new IllegalArgumentException("Expected an accelerometer revision");
                long previous = args.getLong(0);
                if (previous < 0 || previous > revision || previous != args.getDouble(0))
                    throw new IllegalArgumentException("Invalid accelerometer revision");
                if (previous == revision) {
                    if (pending.size() >= 32) throw new IllegalStateException("Too many pending accelerometer observers");
                    pending.put(callId, reply);
                    return;
                }
            } else {
                if (args.length() != 0) throw new IllegalArgumentException("Unexpected accelerometer arguments");
                switch (path) {
                    case "muon.android.accelerometer.start": requested = true; reconcile(); break;
                    case "muon.android.accelerometer.stop": requested = false; reconcile(); break;
                    case "muon.android.accelerometer.getState": break;
                    default: throw new IllegalArgumentException("Unknown accelerometer operation");
                }
            }
            reply.completeString(snapshot());
        } catch (JSONException | RuntimeException invalid) { reply.fail(invalid.toString()); }
    }

    private void reconcile() {
        if (!requested || !resumed) {
            if (releaseListener()) change(requested ? "suspended" : "stopped", null);
            return;
        }
        if (listener != null && state.equals("active")) return;
        if (!releaseListener()) return;
        try {
            Sensor sensor = manager == null ? null : manager.getDefaultSensor(Sensor.TYPE_ACCELEROMETER);
            if (sensor == null) { change("unavailable", "sensor_unavailable"); return; }
            listener = new SensorEventListener() {
                @Override public void onAccuracyChanged(Sensor sensor, int accuracy) {}
                @Override public void onSensorChanged(SensorEvent event) {
                    if (listener != this || !state.equals("active") || !requested || !resumed) return;
                    long receivedUs = SystemClock.elapsedRealtimeNanos() / 1000;
                    long elapsedUs = event.timestamp / 1000;
                    if (elapsedUs < 0 || receivedUs < elapsedUs || event.values.length < 3) return;
                    try {
                        sample = new JSONObject().put("elapsedUs", elapsedUs).put("receivedElapsedUs", receivedUs)
                                .put("accuracy", event.accuracy).put("x", (double)event.values[0])
                                .put("y", (double)event.values[1]).put("z", (double)event.values[2]);
                        publish();
                    } catch (JSONException nonFiniteReading) { /* Ignore this measurement. */ }
                }
            };
            if (manager.registerListener(listener, sensor, INTERVAL_US, 0, handler)) change("active", null);
            else if (releaseListener()) change("failed", "sensor_registration_failed");
        } catch (RuntimeException failed) {
            if (releaseListener()) change("failed", failed instanceof SecurityException
                    ? "sensor_permission_denied" : "acceleration_start_failed");
        }
    }

    private boolean releaseListener() {
        if (listener == null) return true;
        try {
            manager.unregisterListener(listener);
            listener = null;
            return true;
        } catch (RuntimeException failed) {
            change("failed", "acceleration_stop_failed");
            return false;
        }
    }

    private void change(String next, String failure) {
        if (state.equals(next) && java.util.Objects.equals(error, failure)) return;
        state = next;
        error = failure;
        sample = null;
        publish();
    }

    private String snapshot() throws JSONException {
        JSONObject status = new JSONObject().put("state", state).put("intervalUs", INTERVAL_US)
                .put("error", error == null ? JSONObject.NULL : error);
        return new JSONObject().put("revision", revision).put("status", status)
                .put("sample", sample == null ? JSONObject.NULL : sample).toString();
    }

    private void publish() {
        ++revision;
        if (pending.isEmpty()) return;
        var waiting = new java.util.ArrayList<>(pending.values());
        pending.clear();
        try {
            String snapshot = snapshot();
            for (var reply : waiting) reply.completeString(snapshot);
        } catch (JSONException invalid) {
            for (var reply : waiting) reply.fail(invalid.toString());
        }
    }

    void cancel(int callId) { pending.remove(callId); }
    void setResumed(boolean value) { resumed = value; reconcile(); }
    void releasePage() {
        pending.clear();
        requested = false;
        reconcile();
    }
}

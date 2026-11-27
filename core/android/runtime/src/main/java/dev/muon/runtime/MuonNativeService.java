/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
package dev.muon.runtime;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Binder;
import android.os.Build;
import android.os.IBinder;
import androidx.core.app.NotificationCompat;
import java.util.Arrays;
import java.util.HashSet;
import java.util.Set;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

/** Per-logical-service Android component, delegated to the shared execution host. */
public abstract class MuonNativeService extends Service {
    private MuonServiceProcess process;
    private long generation;
    private int startId;
    private int foregroundTypes;
    private boolean stopped;

    /** @return Build-time logical service identity of this generated component. */
    protected abstract String serviceId();

    /** Local binding between the controller and the generated component in its private process. */
    final class LocalBinder extends Binder {
        MuonNativeService component() { return MuonNativeService.this; }
    }

    @Override public IBinder onBind(Intent intent) { return new LocalBinder(); }

    void startBound(MuonServiceProcess owner, long nextGeneration) {
        generation = nextGeneration;
        process = owner;
        stopped = false;
        startId = 0;
        foregroundTypes = 0;
        process.foregroundReady(this, serviceId(), generation);
    }

    private void promote(int types) throws JSONException {
        JSONObject notification = process.definition(serviceId()).getJSONObject("android").getJSONObject("notification");
        String channel = notification.getString("channelId");
        String title = notification.getString("title");
        if (Build.VERSION.SDK_INT >= 26) {
            getSystemService(NotificationManager.class).createNotificationChannel(
                    new NotificationChannel(channel, title, NotificationManager.IMPORTANCE_LOW));
        }
        Notification status = new NotificationCompat.Builder(this, channel)
                .setSmallIcon(android.R.drawable.ic_dialog_info)
                .setContentTitle(title).setContentText(notification.getString("text"))
                .setOngoing(true).build();
        if (Build.VERSION.SDK_INT >= 29) startForeground(process.notificationId(serviceId()), status, types);
        else startForeground(process.notificationId(serviceId()), status);
        foregroundTypes = types;
    }

    private static int typeFlag(String name) {
        return switch (name) {
            case "camera" -> ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA;
            case "connectedDevice" -> ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE;
            case "dataSync" -> ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC;
            case "health" -> ServiceInfo.FOREGROUND_SERVICE_TYPE_HEALTH;
            case "location" -> ServiceInfo.FOREGROUND_SERVICE_TYPE_LOCATION;
            case "mediaPlayback" -> ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK;
            case "mediaProcessing" -> ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROCESSING;
            case "mediaProjection" -> ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION;
            case "microphone" -> ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE;
            case "phoneCall" -> ServiceInfo.FOREGROUND_SERVICE_TYPE_PHONE_CALL;
            case "remoteMessaging" -> ServiceInfo.FOREGROUND_SERVICE_TYPE_REMOTE_MESSAGING;
            case "shortService" -> ServiceInfo.FOREGROUND_SERVICE_TYPE_SHORT_SERVICE;
            case "specialUse" -> ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE;
            case "systemExempted" -> ServiceInfo.FOREGROUND_SERVICE_TYPE_SYSTEM_EXEMPTED;
            default -> throw new IllegalArgumentException("Unknown foreground service type");
        };
    }

    void setForegroundTypes(long expected, String declared, String parameters) throws JSONException {
        if (expected != generation || stopped) throw new IllegalStateException("Service generation has stopped");
        JSONObject request = new JSONObject(parameters);
        JSONArray types = request.getJSONArray("types");
        if (request.length() != 1) throw new IllegalArgumentException("Unexpected foreground service parameters");
        Set<String> allowed = new HashSet<>(Arrays.asList(declared.split("\\|")));
        int mask = 0;
        for (int i = 0; i < types.length(); i++) {
            Object type = types.get(i);
            if (!(type instanceof String)) throw new IllegalArgumentException("Expected foreground service type name");
            int flag = typeFlag((String) type);
            if ((mask & flag) != 0) throw new IllegalArgumentException("Duplicate foreground service type");
            if (!allowed.contains(type)) throw new SecurityException("Foreground service type is not declared: " + type);
            mask |= flag;
        }
        if (mask == foregroundTypes) return;
        if (mask == 0) {
            stopForeground(STOP_FOREGROUND_REMOVE);
            stopSelf();
            foregroundTypes = 0;
            return;
        }
        boolean first = foregroundTypes == 0;
        // The binding keeps the generation alive. A started component owns the
        // notification while work runs, even after the last page has gone.
        if (first) startService(new Intent(this, getClass()).putExtra("onDemandStart", true)
                .putExtra("generation", generation));
        try { promote(mask); }
        catch (JSONException | RuntimeException error) {
            if (first) stopSelf();
            throw error;
        }
    }

    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent == null) { stopSelf(); return START_NOT_STICKY; }
        long requestedGeneration = intent.getLongExtra("generation", 0);
        if (intent.getBooleanExtra("onDemandStart", false)) {
            if (requestedGeneration == generation && !stopped && foregroundTypes != 0) this.startId = startId;
            else stopSelfResult(startId);
            return START_NOT_STICKY;
        }
        if (process != null && generation == requestedGeneration && !stopped) {
            this.startId = startId;
            return START_NOT_STICKY;
        }
        this.startId = startId;
        generation = requestedGeneration;
        stopped = false;
        process = MuonServiceProcess.get(this);
        try {
            // Permissions and eligibility are checked before calling developer code.
            promote(ServiceInfo.FOREGROUND_SERVICE_TYPE_MANIFEST);
            process.foregroundReady(this, serviceId(), generation);
        } catch (JSONException | RuntimeException error) {
            process.startFailed(serviceId(), generation, intent.getLongExtra("bootAttemptEpoch", -1), error);
            stopGeneration(generation);
        }
        return START_NOT_STICKY;
    }

    void stopGeneration(long expected) {
        if (generation != expected || stopped) return;
        stopped = true;
        stopForeground(STOP_FOREGROUND_REMOVE);
        if (startId != 0) stopSelfResult(startId);
        else stopSelf();
        foregroundTypes = 0;
    }

    @Override public void onTimeout(int startId) { if (startId == this.startId) timeout("Android short-service deadline"); }
    @Override public void onTimeout(int startId, int type) { if (startId == this.startId) timeout("Android foreground-service deadline: " + type); }

    private void timeout(String reason) {
        if (process != null) process.runtimeStop(serviceId(), generation, reason, true);
        // Android's grace deadline is independent of asynchronous native cleanup.
        stopGeneration(generation);
    }

    @Override public void onDestroy() {
        if (process != null) process.componentDestroyed(this, serviceId(), generation, stopped);
        super.onDestroy();
    }
}

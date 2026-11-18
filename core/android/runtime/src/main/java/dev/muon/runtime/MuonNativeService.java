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
import android.os.Build;
import android.os.IBinder;
import androidx.core.app.NotificationCompat;
import org.json.JSONException;
import org.json.JSONObject;

/** Per-logical-service Android component, delegated to the shared execution host. */
public abstract class MuonNativeService extends Service {
    private MuonServiceProcess process;
    private long generation;
    private int startId;
    private boolean stopped;

    /** @return Build-time logical service identity of this generated component. */
    protected abstract String serviceId();

    @Override public IBinder onBind(Intent intent) { return null; }

    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent == null) { stopSelf(); return START_NOT_STICKY; }
        long requestedGeneration = intent.getLongExtra("generation", 0);
        if (process != null && generation == requestedGeneration && !stopped) {
            this.startId = startId;
            return START_NOT_STICKY;
        }
        this.startId = startId;
        generation = requestedGeneration;
        stopped = false;
        process = MuonServiceProcess.get(this);
        try {
            JSONObject options = process.definition(serviceId()).getJSONObject("android");
            JSONObject notification = options.getJSONObject("notification");
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
            // Permissions and eligibility are checked before calling developer code.
            if (Build.VERSION.SDK_INT >= 29)
                startForeground(process.notificationId(serviceId()), status,
                        ServiceInfo.FOREGROUND_SERVICE_TYPE_MANIFEST);
            else startForeground(process.notificationId(serviceId()), status);
            process.foregroundReady(this, serviceId(), generation);
        } catch (JSONException | RuntimeException error) {
            process.startFailed(serviceId(), generation, error.toString());
            stopGeneration(generation);
        }
        return START_NOT_STICKY;
    }

    void stopGeneration(long expected) {
        if (generation != expected || stopped) return;
        stopped = true;
        stopForeground(STOP_FOREGROUND_REMOVE);
        stopSelfResult(startId);
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

/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
package dev.muon.runtime;

import android.annotation.SuppressLint;
import android.annotation.TargetApi;
import android.app.ActivityManager;
import android.app.ApplicationExitInfo;
import android.content.Context;
import android.os.Build;
import android.os.Handler;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.ForkJoinPool;
import org.json.JSONException;
import org.json.JSONObject;

/** Reads retained OS history independently of current service state and policy. */
final class MuonServiceExitHistory {
    private MuonServiceExitHistory() {}

    static final class Entry {
        final String processName;
        final int pid;
        final long timestamp;
        final int reason;
        final int status;
        final String description;
        Entry(String processName, int pid, long timestamp, int reason, int status, String description) {
            this.processName = processName; this.pid = pid; this.timestamp = timestamp;
            this.reason = reason; this.status = status; this.description = description;
        }
    }

    interface Source { List<Entry> read() throws Exception; }

    static void load(Context context, Handler owner, MuonServicePreferences.Completion<JSONObject> complete) {
        load(Build.VERSION.SDK_INT, context.getPackageName() + ":muon_services",
                () -> Api30.read(context), owner, complete);
    }

    static void load(int api, String processName, Source source, Handler owner,
            MuonServicePreferences.Completion<JSONObject> complete) {
        if (api < 30) { owner.post(() -> complete.complete(null, null)); return; }
        try {
            // Binder may wait for system-server storage. Reuse the existing JVM
            // pool instead of blocking the service owner or creating a worker.
            ForkJoinPool.commonPool().execute(() -> {
                try {
                    JSONObject result = latest(processName, source.read());
                    owner.post(() -> complete.complete(result, null));
                } catch (Exception error) {
                    owner.post(() -> complete.complete(null, error.toString()));
                }
            });
        } catch (RuntimeException error) {
            owner.post(() -> complete.complete(null, error.toString()));
        }
    }

    static JSONObject latest(String processName, Iterable<Entry> records) throws JSONException {
        Entry latest = null;
        for (Entry entry : records) {
            // The OS also includes UI and external bound-service processes.
            // Never attribute their exit to the independent service host.
            if (processName.equals(entry.processName) && (latest == null || entry.timestamp > latest.timestamp))
                latest = entry;
        }
        if (latest == null) return null;
        return new JSONObject().put("platform", "android").put("processName", latest.processName)
                .put("pid", latest.pid).put("timestampMs", Long.toString(latest.timestamp))
                .put("reason", reason(latest.reason)).put("reasonCode", latest.reason)
                .put("status", latest.status)
                .put("description", latest.description == null ? JSONObject.NULL : latest.description);
    }

    // These public integer constants are inlined; newer reason codes do not
    // access newer OS methods or require those Android versions at runtime.
    @SuppressLint("InlinedApi")
    private static String reason(int code) {
        switch (code) {
            case ApplicationExitInfo.REASON_EXIT_SELF: return "exit-self";
            case ApplicationExitInfo.REASON_SIGNALED: return "signaled";
            case ApplicationExitInfo.REASON_LOW_MEMORY: return "low-memory";
            case ApplicationExitInfo.REASON_CRASH: return "crash";
            case ApplicationExitInfo.REASON_CRASH_NATIVE: return "crash-native";
            case ApplicationExitInfo.REASON_ANR: return "anr";
            case ApplicationExitInfo.REASON_INITIALIZATION_FAILURE: return "initialization-failure";
            case ApplicationExitInfo.REASON_PERMISSION_CHANGE: return "permission-change";
            case ApplicationExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE: return "excessive-resource-usage";
            case ApplicationExitInfo.REASON_USER_REQUESTED: return "user-requested";
            case ApplicationExitInfo.REASON_USER_STOPPED: return "user-stopped";
            case ApplicationExitInfo.REASON_DEPENDENCY_DIED: return "dependency-died";
            case ApplicationExitInfo.REASON_OTHER: return "other";
            case ApplicationExitInfo.REASON_FREEZER: return "freezer";
            case ApplicationExitInfo.REASON_PACKAGE_STATE_CHANGE: return "package-state-change";
            case ApplicationExitInfo.REASON_PACKAGE_UPDATED: return "package-updated";
            default: return "unknown";
        }
    }

    /** Isolated so pre-30 devices never resolve ApplicationExitInfo methods. */
    @TargetApi(30)
    private static final class Api30 {
        static List<Entry> read(Context context) {
            ActivityManager manager = context.getSystemService(ActivityManager.class);
            if (manager == null) throw new IllegalStateException("Android activity manager is unavailable");
            ArrayList<Entry> records = new ArrayList<>();
            // Zero requests all entries retained in Android's bounded ring. A
            // small global limit could hide the host behind unrelated UI exits.
            // This API only returns dead processes, not the current live host;
            // do not discard a historical PID merely because Linux reused it.
            for (ApplicationExitInfo record : manager.getHistoricalProcessExitReasons(context.getPackageName(), 0, 0))
                records.add(new Entry(record.getProcessName(), record.getPid(), record.getTimestamp(),
                        record.getReason(), record.getStatus(), record.getDescription()));
            return records;
        }
    }
}

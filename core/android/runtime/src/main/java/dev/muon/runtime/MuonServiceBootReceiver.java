/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
package dev.muon.runtime;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.util.Log;

/** Restores resident services in their private process after the first user unlock. */
public final class MuonServiceBootReceiver extends BroadcastReceiver {
    @Override public void onReceive(Context context, Intent intent) {
        if (!Intent.ACTION_BOOT_COMPLETED.equals(intent.getAction())) return;
        PendingResult pending = goAsync();
        try {
            MuonServiceProcess.get(context).activateOnBoot(pending::finish);
        } catch (RuntimeException | LinkageError error) {
            Log.e("MuonNativeService", "Cannot initialize services after boot", error);
            pending.finish();
        }
    }
}

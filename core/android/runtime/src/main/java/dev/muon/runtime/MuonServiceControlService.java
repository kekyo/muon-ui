/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
package dev.muon.runtime;

import android.app.Service;
import android.content.Intent;
import android.os.IBinder;

/** Private connection endpoint; binding alone owns no logical service reference. */
public final class MuonServiceControlService extends Service {
    @Override public IBinder onBind(Intent intent) { return MuonServiceProcess.get(this).binder(); }
}

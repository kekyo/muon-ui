/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

package dev.muon.prototype;

import android.os.ParcelFileDescriptor;

/** Controls independent JavaScript runtimes in the private Service process. */
interface IMuonJavaScriptRuntimeService {
    /** Creates one QuickJS runtime and returns its framed protocol socket. */
    ParcelFileDescriptor createRuntime(String runtimeId);

    /** Returns the embedded QuickJS release version. */
    String getEngineVersion();

    /** Returns the PID of the private Service process. */
    int getProcessId();

    /** Returns the number of currently live logical runtimes. */
    int getRuntimeCount();

    /** Interrupts one logical runtime. */
    void shutdownRuntime(String runtimeId);

    /** Interrupts every logical runtime hosted by this Service generation. */
    void shutdownAll();

    /** Terminates the private process in debug builds for recovery testing. */
    void terminateForTest();
}

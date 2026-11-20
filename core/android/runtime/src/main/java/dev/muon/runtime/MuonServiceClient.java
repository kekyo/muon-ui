/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
package dev.muon.runtime;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.Message;
import android.os.Messenger;
import android.os.ParcelFileDescriptor;
import android.os.RemoteException;
import androidx.core.os.BundleCompat;
import java.io.IOException;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Map;
import org.json.JSONException;
import org.json.JSONObject;

/** One page/connection owner; all operations run on the main Looper. */
final class MuonServiceClient implements AutoCloseable {
    interface Completion {
        void complete(JSONObject value);
        void fail(String error);
        default void completeJson(String value) {
            try { complete(new JSONObject(value)); }
            catch (JSONException error) { fail(error.toString()); }
        }
    }

    private final Context context;
    private final Map<Integer, Completion> pending = new HashMap<>();
    private final ArrayList<Message> queued = new ArrayList<>();
    private final Messenger reply = new Messenger(new Handler(Looper.getMainLooper(), this::receive));
    private Messenger remote;
    private boolean bound;
    private boolean resumed;
    private boolean closed;
    private String terminalError = "Service page session was released";
    private int nextRequest = 1;
    private long rpcHandle;
    private boolean rpcCreated;
    private final java.util.concurrent.CountDownLatch rpcClosed = new java.util.concurrent.CountDownLatch(1);
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final ServiceConnection connection = new ServiceConnection() {
        @Override public void onServiceConnected(ComponentName name, IBinder binder) {
            if (closed) return;
            remote = new Messenger(binder);
            try {
                Message open = message(0, new JSONObject().put("operation", "open").put("resumed", resumed));
                remote.send(open);
                for (Message command : queued) remote.send(command);
                queued.clear();
            } catch (JSONException | RemoteException error) { disconnected(error.toString()); }
        }
        @Override public void onServiceDisconnected(ComponentName name) { disconnected("Service process exited"); }
        @Override public void onBindingDied(ComponentName name) { disconnected("Service binding died"); }
        @Override public void onNullBinding(ComponentName name) { disconnected("Service binding rejected"); }
    };

    MuonServiceClient(Context context) { this.context = context.getApplicationContext(); }

    void setResumed(boolean value) {
        if (resumed == value) return;
        resumed = value;
        if (!value || closed || remote == null) return;
        try { remote.send(message(0, new JSONObject().put("operation", "resume"))); }
        catch (JSONException | RemoteException error) { disconnected(error.toString()); }
    }

    void activate() {
        if (closed || bound) return;
        try {
            bound = context.bindService(new Intent(context, MuonServiceControlService.class),
                    connection, Context.BIND_AUTO_CREATE);
            if (!bound) disconnected("Unable to bind the private service host");
        } catch (RuntimeException error) { disconnected(error.toString()); }
    }

    void invoke(String operation, JSONObject arguments, Completion completion) {
        if (closed) { completion.fail(terminalError); return; }
        int id = nextRequest++;
        pending.put(id, completion);
        try {
            if (operation.equals("rpc")) {
                boolean opening = rpcHandle == 0;
                if (opening) {
                    System.loadLibrary("muon_android_rpc");
                    rpcHandle = nativeCreateRpc(this);
                    rpcCreated = rpcHandle != 0;
                    if (rpcHandle == 0) throw new IllegalStateException("Unable to create service RPC client");
                }
                // The common native client bounds its queue while Binder is
                // establishing the stream. Java never copies RPC into Binder.
                nativeRpc(rpcHandle, id, arguments.toString());
                if (opening) invoke("rpcOpen", new JSONObject(), new Completion() {
                    @Override public void complete(JSONObject value) { /* Descriptor reply is handled below. */ }
                    @Override public void fail(String error) { disconnected(error); }
                });
                return;
            }
            JSONObject command = new JSONObject().put("operation", operation)
                    .put("arguments", new JSONObject(arguments.toString()));
            Message message = message(id, command);
            if (remote != null) remote.send(message);
            else {
                queued.add(message);
                activate();
            }
        } catch (JSONException | RuntimeException | RemoteException error) { disconnected(error.toString()); }
    }

    private Message message(int request, JSONObject json) {
        Message message = Message.obtain();
        message.arg1 = request;
        message.replyTo = reply;
        Bundle data = new Bundle();
        data.putString("json", json.toString());
        message.setData(data);
        return message;
    }

    private boolean receive(Message message) {
        if (message.what == 2) { disconnected("Service RPC stream ended"); return true; }
        if (message.what == 1) {
            try (ParcelFileDescriptor descriptor = BundleCompat.getParcelable(
                    message.getData(), "descriptor", ParcelFileDescriptor.class)) {
                pending.remove(message.arg1);
                if (closed) return true;
                if (descriptor == null || rpcHandle == 0) throw new IllegalStateException("Missing service RPC descriptor");
                nativeAttachRpc(rpcHandle, descriptor.detachFd());
            } catch (IOException | RuntimeException error) { disconnected(error.toString()); }
            return true;
        }
        Completion completion = pending.remove(message.arg1);
        if (completion == null) return true;
        String error = message.getData().getString("error");
        if (error != null) completion.fail(error);
        else {
            try { completion.complete(new JSONObject(message.getData().getString("json", "{}"))); }
            catch (JSONException failure) { completion.fail(failure.toString()); }
        }
        return true;
    }

    private void disconnected(String error) {
        // A replacement process can reuse reference/generation counters. Never
        // reconnect this owner or forward its old handles into that process.
        closed = true;
        terminalError = error;
        long nativeClient = rpcHandle;
        rpcHandle = 0;
        if (nativeClient != 0) nativeCloseRpc(nativeClient);
        else if (!rpcCreated) rpcClosed.countDown();
        if (remote != null) {
            try { remote.send(message(0, new JSONObject().put("operation", "close"))); }
            catch (JSONException | RemoteException ignored) { /* Binder death also releases this owner. */ }
        }
        remote = null;
        queued.clear();
        if (bound) { context.unbindService(connection); bound = false; }
        ArrayList<Completion> callbacks = new ArrayList<>(pending.values());
        pending.clear();
        for (Completion completion : callbacks) completion.fail(error);
    }

    // Native disposal can finish after close. Each posted task carries its
    // retained native holder rather than reading the cleared page field.
    private void postRpcTask(long handle, long task) { handler.post(() -> nativeRunRpcTask(handle, task)); }
    private void onRpcResult(int request, String json, String error) {
        Completion completion = pending.remove(request);
        if (completion == null) return;
        if (error != null) completion.fail(error);
        else completion.completeJson(json);
    }
    private void onRpcClosed() { rpcClosed.countDown(); }
    boolean awaitRpcClosedForTest(long timeout, java.util.concurrent.TimeUnit unit) throws InterruptedException {
        return rpcClosed.await(timeout, unit);
    }
    private void onRpcDisconnected() { disconnected("Service RPC stream ended"); }

    private static native long nativeCreateRpc(MuonServiceClient owner);
    private static native void nativeAttachRpc(long handle, int descriptor);
    private static native void nativeRpc(long handle, int request, String json);
    private static native void nativeRunRpcTask(long handle, long task);
    private static native void nativeCloseRpc(long handle);

    @Override public void close() {
        if (closed) return;
        closed = true;
        disconnected("Service page session was released");
    }
}

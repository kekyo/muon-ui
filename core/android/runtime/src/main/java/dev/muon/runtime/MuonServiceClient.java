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
import android.os.RemoteException;
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
    }

    private final Context context;
    private final Map<Integer, Completion> pending = new HashMap<>();
    private final ArrayList<Message> queued = new ArrayList<>();
    private final Messenger reply = new Messenger(new Handler(Looper.getMainLooper(), this::receive));
    private Messenger remote;
    private boolean bound;
    private boolean closed;
    private int nextRequest = 1;
    private final ServiceConnection connection = new ServiceConnection() {
        @Override public void onServiceConnected(ComponentName name, IBinder binder) {
            if (closed) return;
            remote = new Messenger(binder);
            try {
                Message open = message(0, new JSONObject().put("operation", "open"));
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

    void activate() {
        if (closed || bound) return;
        try {
            bound = context.bindService(new Intent(context, MuonServiceControlService.class),
                    connection, Context.BIND_AUTO_CREATE);
            if (!bound) disconnected("Unable to bind the private service host");
        } catch (RuntimeException error) { disconnected(error.toString()); }
    }

    void invoke(String operation, JSONObject arguments, Completion completion) {
        if (closed) { completion.fail("Service page session was released"); return; }
        int id = nextRequest++;
        pending.put(id, completion);
        try {
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

    @Override public void close() {
        if (closed) return;
        closed = true;
        disconnected("Service page session was released");
    }
}

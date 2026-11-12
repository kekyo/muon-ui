/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

package dev.muon.runtime;

import android.content.Context;
import android.content.Intent;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.Message;
import android.os.Messenger;
import android.os.Process;
import android.os.RemoteException;
import android.util.Log;
import dalvik.system.BaseDexClassLoader;
import java.io.File;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Iterator;
import java.util.Map;
import java.util.TreeMap;
import java.util.UUID;
import org.json.JSONException;
import org.json.JSONObject;

/** One native service controller on the private process's main Looper. */
final class MuonServiceProcess {
    static { System.loadLibrary("muon_android_services"); }
    private static MuonServiceProcess instance;

    private static final class Client {
        final String owner = UUID.randomUUID().toString();
        final Messenger reply;
        final Map<String, Integer> acquisitions = new HashMap<>();
        IBinder.DeathRecipient death;
        Client(Messenger reply) { this.reply = reply; }
    }

    private final Context context;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Map<String, JSONObject> definitions = new TreeMap<>();
    private final Map<String, MuonNativeService> components = new HashMap<>();
    private final Map<IBinder, Client> clients = new HashMap<>();
    private final Map<String, Client> owners = new HashMap<>();
    private final Messenger messenger = new Messenger(new Handler(Looper.getMainLooper(), this::receive));
    private final long nativeHandle;

    static MuonServiceProcess get(Context context) {
        if (Looper.myLooper() != Looper.getMainLooper())
            throw new IllegalStateException("Service operations require the main Looper");
        if (instance == null) instance = new MuonServiceProcess(context.getApplicationContext());
        return instance;
    }

    private MuonServiceProcess(Context context) {
        this.context = context;
        try {
            JSONObject config = new JSONObject(MuonAppConfig.readAsset(context, "muon/config.json"));
            JSONObject services = config.optJSONObject("services");
            if (services != null) {
                Iterator<String> ids = services.keys();
                while (ids.hasNext()) {
                    String id = ids.next();
                    if (!id.matches("[A-Za-z_][A-Za-z0-9_]*"))
                        throw new JSONException("Invalid service identity");
                    definitions.put(id, services.getJSONObject(id));
                }
            }
            int count = definitions.size();
            String[] ids = new String[count];
            String[] libraries = new String[count];
            String[] directories = new String[count];
            boolean[] autostarts = new boolean[count];
            String[][] configurations = new String[count][];
            int index = 0;
            for (Map.Entry<String, JSONObject> entry : definitions.entrySet()) {
                String id = entry.getKey();
                JSONObject definition = entry.getValue();
                if (!"native".equals(definition.getString("runtime")))
                    throw new JSONException("Unsupported service runtime");
                String library = definition.getString("library");
                if (!library.matches("lib[A-Za-z0-9_][A-Za-z0-9_.+-]*\\.so"))
                    throw new JSONException("Invalid service library name");
                String path = ((BaseDexClassLoader)context.getClassLoader()).findLibrary(
                        library.substring(3, library.length() - 3));
                if (path == null) throw new IllegalStateException("Service library is not installed: " + library);
                File directory = new File(new File(context.getFilesDir(), "services"), id);
                if (!directory.isDirectory() && !directory.mkdirs())
                    throw new IllegalStateException("Cannot create service data directory: " + id);
                ids[index] = id;
                libraries[index] = path;
                directories[index] = directory.getAbsolutePath();
                autostarts[index] = definition.optBoolean("autostart", false);
                JSONObject values = definition.optJSONObject("config");
                ArrayList<String> pairs = new ArrayList<>();
                if (values != null) {
                    Iterator<String> keys = values.keys();
                    while (keys.hasNext()) {
                        String key = keys.next();
                        pairs.add(key);
                        pairs.add(values.getString(key));
                    }
                }
                configurations[index++] = pairs.toArray(new String[0]);
            }
            nativeHandle = nativeCreate(this, ids, libraries, directories, autostarts, configurations);
        } catch (JSONException error) {
            throw new IllegalStateException("Invalid packaged services", error);
        }
    }

    IBinder binder() { return messenger.getBinder(); }

    JSONObject definition(String id) {
        JSONObject result = definitions.get(id);
        if (result == null) throw new IllegalArgumentException("Unknown service: " + id);
        return result;
    }

    int notificationId(String id) {
        int index = new ArrayList<>(definitions.keySet()).indexOf(id);
        if (index < 0) throw new IllegalArgumentException("Unknown service: " + id);
        return 31000 + index;
    }

    private Client client(Messenger reply) throws RemoteException {
        IBinder binder = reply.getBinder();
        Client client = clients.get(binder);
        if (client == null) {
            client = new Client(reply);
            client.death = () -> handler.post(() -> releaseClient(binder));
            binder.linkToDeath(client.death, 0);
            clients.put(binder, client);
            owners.put(client.owner, client);
        }
        return client;
    }

    private void releaseClient(IBinder binder) {
        Client client = clients.remove(binder);
        if (client == null) return;
        owners.remove(client.owner);
        binder.unlinkToDeath(client.death, 0);
        nativeReleaseOwner(nativeHandle, client.owner);
    }

    private boolean receive(Message message) {
        if (message.sendingUid != Process.myUid() || message.replyTo == null) return true;
        int request = message.arg1;
        Client client = null;
        try {
            JSONObject command = new JSONObject(message.getData().getString("json", "{}"));
            String operation = command.getString("operation");
            if ("close".equals(operation)) {
                releaseClient(message.replyTo.getBinder());
                return true;
            }
            client = client(message.replyTo);
            if ("open".equals(operation)) {
                nativeActivate(nativeHandle);
                return true;
            }
            String id = command.getString("serviceId");
            definition(id);
            if ("acquire".equals(operation)) {
                String reference = nativeAcquire(nativeHandle, id, client.owner);
                if (reference == null) throw new IllegalStateException("Service acquisition was rejected");
                client.acquisitions.put(new JSONObject(reference).getString("refId"), request);
            } else if ("release".equals(operation)) {
                boolean released = nativeRelease(nativeHandle, id, client.owner,
                        Long.parseUnsignedLong(command.getString("refId")),
                        Long.parseUnsignedLong(command.getString("generation")));
                reply(client, request, new JSONObject().put("released", released), null);
            } else if ("getState".equals(operation)) {
                String state = nativeSnapshot(nativeHandle, id);
                if (state == null) throw new IllegalStateException("Unknown service");
                reply(client, request, new JSONObject(state), null);
            } else {
                throw new IllegalArgumentException("Unsupported service operation: " + operation);
            }
        } catch (JSONException | RuntimeException | RemoteException error) {
            if (client != null) reply(client, request, null, error.toString());
        }
        return true;
    }

    private void reply(Client client, int request, JSONObject value, String error) {
        Message reply = Message.obtain();
        reply.arg1 = request;
        Bundle data = new Bundle();
        if (error == null) data.putString("json", value.toString());
        else data.putString("error", error);
        reply.setData(data);
        try { client.reply.send(reply); }
        catch (RemoteException disconnected) { releaseClient(client.reply.getBinder()); }
    }

    // Called by JNI on the owner Looper. No native callback is invoked inline.
    private void postTask(long task) { handler.post(() -> nativeRunTask(nativeHandle, task)); }

    private void startComponent(String id, long generation) {
        try {
            String component = definition(id).getJSONObject("android").getString("component");
            Intent intent = new Intent().setClassName(context, component)
                    .putExtra("generation", generation);
            if (Build.VERSION.SDK_INT >= 26) context.startForegroundService(intent);
            else context.startService(intent);
        } catch (JSONException | RuntimeException error) {
            nativeStartFailed(nativeHandle, id, generation, error.toString());
        }
    }

    private void stopComponent(String id, long generation) {
        MuonNativeService component = components.get(id);
        if (component != null) component.stopGeneration(generation);
    }

    void foregroundReady(MuonNativeService component, String id, long generation) {
        components.put(id, component);
        if (!nativeForegroundReady(nativeHandle, id, generation)) component.stopGeneration(generation);
    }

    void startFailed(String id, long generation, String error) {
        nativeStartFailed(nativeHandle, id, generation, error);
    }

    void runtimeStop(String id, long generation, String reason) {
        nativeRuntimeStop(nativeHandle, id, generation, reason);
    }

    void componentDestroyed(MuonNativeService component, String id, long generation, boolean stopped) {
        if (components.get(id) == component) components.remove(id);
        if (!stopped) runtimeStop(id, generation, "Android destroyed the service component");
    }

    private void onEvent(String json) {
        try {
            JSONObject event = new JSONObject(json);
            String kind = event.getString("kind");
            if (!"acquired".equals(kind) && !"acquireFailed".equals(kind)) return;
            Client client = owners.get(event.getString("owner"));
            if (client == null) return;
            JSONObject reference = event.getJSONObject("reference");
            Integer request = client.acquisitions.remove(reference.getString("refId"));
            if (request != null) reply(client, request, reference,
                    "acquired".equals(kind) ? null : event.getString("error"));
        } catch (JSONException error) { Log.e("MuonNativeService", "Invalid native service event", error); }
    }

    private static native long nativeCreate(MuonServiceProcess owner, String[] ids,
            String[] libraries, String[] directories, boolean[] autostarts, String[][] configurations);
    private static native void nativeRunTask(long handle, long task);
    private static native void nativeActivate(long handle);
    private static native String nativeAcquire(long handle, String id, String owner);
    private static native boolean nativeRelease(long handle, String id, String owner, long reference, long generation);
    private static native void nativeReleaseOwner(long handle, String owner);
    private static native String nativeSnapshot(long handle, String id);
    private static native boolean nativeForegroundReady(long handle, String id, long generation);
    private static native void nativeStartFailed(long handle, String id, long generation, String error);
    private static native void nativeRuntimeStop(long handle, String id, long generation, String error);
}

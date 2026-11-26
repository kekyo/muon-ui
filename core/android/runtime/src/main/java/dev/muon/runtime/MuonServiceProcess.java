/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

package dev.muon.runtime;

import android.annotation.SuppressLint;
import android.content.Context;
import android.content.BroadcastReceiver;
import android.content.ComponentName;
import android.content.ServiceConnection;
import android.content.IntentFilter;
import androidx.core.content.ContextCompat;
import android.content.Intent;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.Message;
import android.os.Messenger;
import android.os.ParcelFileDescriptor;
import android.os.Process;
import android.os.RemoteException;
import android.os.SystemClock;
import android.util.Log;
import dalvik.system.BaseDexClassLoader;
import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Iterator;
import java.util.Map;
import java.util.TreeMap;
import java.util.UUID;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

/** One native service controller on the private process's main Looper. */
final class MuonServiceProcess {
    static { System.loadLibrary("muon_android_services"); }
    // This process-lifetime host retains only getApplicationContext(), never an Activity.
    @SuppressLint("StaticFieldLeak")
    private static MuonServiceProcess instance;

    private static final class Client {
        final String owner = UUID.randomUUID().toString();
        final Messenger reply;
        final Map<String, Integer> acquisitions = new HashMap<>();
        IBinder.DeathRecipient death;
        boolean rpcOpened;
        Client(Messenger reply) { this.reply = reply; }
    }

    private static final class SaveRequest {
        final Client client;
        final int request;
        SaveRequest(Client client, int request) { this.client = client; this.request = request; }
    }

    private static final class Watch {
        final Client client;
        final String serviceId;
        String deliveredState;
        JSONObject latest;
        Integer pending;
        Watch(Client client, String serviceId, JSONObject initial) throws JSONException {
            this.client = client;
            this.serviceId = serviceId;
            latest = initial;
            deliveredState = initial.toString();
        }
    }

    private final Context context;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Map<String, JSONObject> definitions = new TreeMap<>();
    private final Map<String, MuonNativeService> components = new HashMap<>();
    private final Map<String, GenerationBinding> bindings = new HashMap<>();
    private final Map<IBinder, Client> clients = new HashMap<>();
    private final Map<String, Client> owners = new HashMap<>();
    private final Messenger messenger = new Messenger(new Handler(Looper.getMainLooper(), this::receive));
    private final MuonServicePreferences preferences;
    private final ArrayList<Runnable> initializing = new ArrayList<>();
    private final Map<String, SaveRequest> saves = new HashMap<>();
    private final Map<String, Watch> watches = new HashMap<>();
    private final Map<Long, Runnable> delayedTasks = new HashMap<>();
    private final Map<String, SourceStart> sourceStarts = new HashMap<>();
    private final Map<String, Long> deferredBootStarts = new HashMap<>();
    private boolean activated;
    private boolean dispatchingBootStarts;
    private long resumeEpoch;
    private long nextWatch = 1;
    private long nativeHandle;
    private boolean sessionEnding;
    private boolean initialized;
    private String initializationError;
    private JSONObject lastHostExit;

    private static final class SourceStart {
        final long generation;
        final long deadline;
        Runnable timeout;
        SourceStart(long generation) {
            this.generation = generation;
            deadline = SystemClock.elapsedRealtime() + 10000;
        }
    }

    /** Holds an on-demand component across promotion, demotion and page replacement. */
    private final class GenerationBinding implements ServiceConnection {
        final String id;
        final long generation;
        GenerationBinding(String id, long generation) { this.id = id; this.generation = generation; }
        @Override public void onServiceConnected(ComponentName name, IBinder binder) {
            if (bindings.get(id) == this)
                ((MuonNativeService.LocalBinder) binder).component().startBound(MuonServiceProcess.this, generation);
        }
        @Override public void onNullBinding(ComponentName name) { lost(); }
        @Override public void onBindingDied(ComponentName name) { lost(); }
        @Override public void onServiceDisconnected(ComponentName name) { lost(); }
        private void lost() {
            if (bindings.get(id) != this) return;
            String reason = "Android disconnected the service component";
            if (!nativeStartFailed(nativeHandle, id, generation, reason)) runtimeStop(id, generation, reason, false);
        }
    }

    static MuonServiceProcess get(Context context) {
        if (Looper.myLooper() != Looper.getMainLooper())
            throw new IllegalStateException("Service operations require the main Looper");
        if (instance == null) instance = new MuonServiceProcess(context.getApplicationContext());
        return instance;
    }

    private MuonServiceProcess(Context context) {
        this.context = context;
        preferences = new MuonServicePreferences(new File(context.getFilesDir(), "service-autostart"));
        try {
            JSONObject config = new JSONObject(MuonAppConfig.readAsset(context, "muon/config.json"));
            JSONObject services = config.optJSONObject("services");
            if (services != null) {
                Iterator<String> ids = services.keys();
                while (ids.hasNext()) {
                    String id = ids.next();
                    if (!id.matches("[A-Za-z][A-Za-z0-9_.-]{0,63}"))
                        throw new JSONException("Invalid service identity");
                    definitions.put(id, services.getJSONObject(id));
                }
            }
            int count = definitions.size();
            String[] ids = new String[count];
            String[] libraries = new String[count];
            String[] runtimes = new String[count];
            String[] directories = new String[count];
            String[] ioPolicies = new String[count];
            boolean[] autostarts = new boolean[count];
            String[][] configurations = new String[count][];
            String[][] pluginGrants = new String[count][];
            int index = 0;
            for (Map.Entry<String, JSONObject> entry : definitions.entrySet()) {
                String id = entry.getKey();
                JSONObject definition = entry.getValue();
                String runtime = definition.getString("runtime");
                ioPolicies[index] = definition.has("io") ? definition.getJSONObject("io").toString() : "{}";
                String path;
                if ("native".equals(runtime)) {
                    String library = definition.getString("library");
                    if (!library.matches("lib[A-Za-z0-9_][A-Za-z0-9_.+-]*\\.so"))
                        throw new JSONException("Invalid service library name");
                    path = ((BaseDexClassLoader)context.getClassLoader()).findLibrary(
                            library.substring(3, library.length() - 3));
                    if (path == null) throw new IllegalStateException("Service library is not installed: " + library);
                } else if ("quickjs".equals(runtime)) {
                    path = definition.getString("asset");
                    if (!path.equals("muon/services/" + id + ".js"))
                        throw new JSONException("Invalid service bundle asset");
                } else throw new JSONException("Unsupported service runtime");
                File directory = new File(new File(context.getFilesDir(), "services"), id);
                if (!directory.isDirectory() && !directory.mkdirs())
                    throw new IllegalStateException("Cannot create service data directory: " + id);
                ids[index] = id;
                libraries[index] = path;
                runtimes[index] = runtime;
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
                configurations[index] = pairs.toArray(new String[0]);
                JSONArray grants = definition.optJSONArray("plugins");
                pluginGrants[index] = new String[grants == null ? 0 : grants.length()];
                for (int grant = 0; grant < pluginGrants[index].length; ++grant)
                    pluginGrants[index][grant] = grants.getString(grant);
                if (!"quickjs".equals(runtime) && pluginGrants[index].length != 0)
                    throw new JSONException("Native module grants require QuickJS services");
                ++index;
            }
            JSONArray plugins = config.optJSONArray("servicePlugins");
            if (plugins == null) plugins = new JSONArray();
            for (int plugin = 0; plugin < plugins.length(); ++plugin) {
                JSONObject entry = plugins.getJSONObject(plugin);
                String soname = entry.getString("soname");
                if (!soname.matches("lib[A-Za-z0-9_][A-Za-z0-9_.+-]*\\.so"))
                    throw new JSONException("Invalid service plugin library name");
                String path = ((BaseDexClassLoader)context.getClassLoader()).findLibrary(
                        soname.substring(3, soname.length() - 3));
                if (path == null) throw new IllegalStateException("Service plugin is not installed: " + soname);
                entry.put("library", path);
            }
            // Keep the receiver on the service process Looper, including while an Activity is detached.
            ContextCompat.registerReceiver(context, new BroadcastReceiver() {
                @Override public void onReceive(Context receiverContext, Intent intent) {
                    if (Intent.ACTION_SHUTDOWN.equals(intent.getAction())) {
                        sessionEnding = true;
                        if (nativeHandle != 0) nativeSessionEnding(nativeHandle);
                    }
                }
            }, new IntentFilter(Intent.ACTION_SHUTDOWN), ContextCompat.RECEIVER_NOT_EXPORTED);
            String pluginRegistry = plugins.toString();
            MuonServiceExitHistory.load(context, handler, (history, historyError) -> {
                lastHostExit = history;
                if (historyError != null) Log.w("MuonNativeService", "Cannot read previous host exit: " + historyError);
                preferences.load(ids, (overrides, error) -> {
                    initializationError = sessionEnding ? "Android device shutdown" : error;
                    if (initializationError == null) {
                        try {
                            nativeHandle = nativeCreate(this, ids, runtimes, libraries, directories, ioPolicies,
                                    autostarts, overrides, configurations, pluginGrants, pluginRegistry);
                        } catch (RuntimeException failure) {
                            initializationError = failure.getMessage();
                        }
                    }
                    initialized = true;
                    ArrayList<Runnable> pending = new ArrayList<>(initializing);
                    initializing.clear();
                    for (Runnable action : pending) action.run();
                });
            });
        } catch (JSONException error) {
            throw new IllegalStateException("Invalid packaged services", error);
        }
    }

    IBinder binder() { return messenger.getBinder(); }

    private void activate(boolean boot) {
        if (activated) return;
        activated = true;
        dispatchingBootStarts = boot;
        nativeActivate(nativeHandle);
    }

    void activateOnBoot(Runnable finished) {
        Runnable activate = () -> {
            try {
                if (initializationError != null) Log.e("MuonNativeService", initializationError);
                else activate(true);
            } catch (RuntimeException | LinkageError error) {
                Log.e("MuonNativeService", "Cannot restore services after boot", error);
            } finally {
                // Activate queues the component start requests on this same Looper.
                // Keep the broadcast alive until those requests have reached Android.
                handler.post(() -> {
                    dispatchingBootStarts = false;
                    finished.run();
                });
            }
        };
        if (initialized) activate.run();
        else initializing.add(activate);
    }

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
        watches.values().removeIf(watch -> watch.client == client);
        saves.values().removeIf(save -> save.client == client);
        if (nativeHandle != 0) nativeReleaseOwner(nativeHandle, client.owner);
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
            Client owner = client;
            Runnable action = () -> {
                // Initialization cannot turn accepted startup/configuration
                // intent into a page-owned action when that page disappears.
                if (operation.equals("open") || operation.equals("setAutostart")
                        || owners.get(owner.owner) == owner)
                    handle(owner, request, operation, command);
            };
            if (initialized) action.run();
            else initializing.add(action);
        } catch (JSONException | RuntimeException | RemoteException error) {
            if (client != null) reply(client, request, null, error.toString());
        }
        return true;
    }

    private void handle(Client client, int request, String operation, JSONObject envelope) {
        try {
            if (initializationError != null) throw new IllegalStateException(initializationError);
            if ("open".equals(operation) || "resume".equals(operation)) {
                if (owners.get(client.owner) == client && ("resume".equals(operation) || envelope.optBoolean("resumed"))) {
                    ++resumeEpoch;
                    for (Map.Entry<String, Long> entry : deferredBootStarts.entrySet())
                        nativeRetryResidentStart(nativeHandle, entry.getKey(), entry.getValue());
                }
                activate(false);
                return;
            }
            if ("rpcOpen".equals(operation)) { openRpc(client, request); return; }
            JSONObject command = envelope.getJSONObject("arguments");
            String id = command.getString("serviceId");
            definition(id);
            if ("acquire".equals(operation)) {
                String reference = nativeAcquire(nativeHandle, id, client.owner);
                if (reference == null) throw new IllegalStateException("Service acquisition was rejected");
                client.acquisitions.put(new JSONObject(reference).getString("refId"), request);
            } else if ("release".equals(operation)) {
                boolean released = nativeRelease(nativeHandle, id, client.owner,
                        command.getString("refId"), command.getString("generation"));
                reply(client, request, new JSONObject().put("released", released), null);
            } else if ("getState".equals(operation)) {
                reply(client, request, snapshot(id), null);
            } else if ("setAutostart".equals(operation)) {
                Object enabled = command.get("enabled");
                if (!(enabled instanceof Boolean)) throw new IllegalArgumentException("Autostart must be boolean");
                String setting = nativeSetAutostart(nativeHandle, id, (Boolean)enabled);
                if (setting == null) throw new IllegalStateException("Autostart change was rejected");
                if (owners.get(client.owner) == client)
                    saves.put(setting, new SaveRequest(client, request));
            } else if ("watchState".equals(operation)) {
                watch(client, request, id, command);
            } else {
                throw new IllegalArgumentException("Unsupported service operation: " + operation);
            }
        } catch (JSONException | IOException | RemoteException | RuntimeException error) {
            reply(client, request, null, error.toString());
        }
    }

    private void openRpc(Client client, int request) throws IOException, RemoteException {
        if (client.rpcOpened) throw new IllegalStateException("A page RPC stream cannot reconnect");
        client.rpcOpened = true;
        ParcelFileDescriptor[] sockets = ParcelFileDescriptor.createSocketPair();
        try (ParcelFileDescriptor server = sockets[0]; ParcelFileDescriptor page = sockets[1]) {
            nativeOpenRpc(nativeHandle, client.owner, server.detachFd());
            Message response = Message.obtain();
            response.what = 1;
            response.arg1 = request;
            Bundle data = new Bundle();
            data.putParcelable("descriptor", page);
            response.setData(data);
            client.reply.send(response);
        }
    }

    private void onRpcDisconnected(String owner) {
        Client client = owners.get(owner);
        if (client == null) return;
        Message ended = Message.obtain();
        ended.what = 2;
        try { client.reply.send(ended); }
        catch (RemoteException ignored) { /* Binder death reclaims the same owner. */ }
        releaseClient(client.reply.getBinder());
    }

    private void watch(Client client, int request, String id, JSONObject command) throws JSONException {
        Object action = command.get("operation");
        if (!(action instanceof String)) throw new IllegalArgumentException("Service watch operation must be a string");
        String operation = (String)action;
        if (operation.equals("acquire")) {
            if (command.has("token")) throw new IllegalArgumentException("New watches cannot supply a token");
            if (nextWatch == 0) throw new IllegalStateException("Service watch identities exhausted");
            // Long.toUnsignedString is unavailable on the API 24 baseline.
            String token = new java.math.BigInteger(Long.toHexString(nextWatch++), 16).toString();
            JSONObject state = snapshot(id);
            watches.put(token, new Watch(client, id, state));
            reply(client, request, new JSONObject().put("token", token).put("state", state), null);
            return;
        }
        Object identity = command.get("token");
        if (!(identity instanceof String)) throw new IllegalArgumentException("Service watch token must be a string");
        String token = (String)identity;
        Watch watch = watches.get(token);
        if (watch == null || watch.client != client || !watch.serviceId.equals(id))
            throw new IllegalArgumentException("Unknown service watch for this owner");
        if (operation.equals("release")) {
            watches.remove(token);
            if (watch.pending != null) reply(client, watch.pending, new JSONObject().put("state", JSONObject.NULL), null);
            reply(client, request, new JSONObject(), null);
        } else if (operation.equals("next")) {
            if (watch.pending != null) throw new IllegalStateException("A service watch read is already pending");
            watch.pending = request;
            // A Changed event may still be queued; read the authoritative state.
            watch.latest = snapshot(id);
            deliverWatch(watch);
        } else throw new IllegalArgumentException("Unknown service watch operation");
    }

    private void deliverWatch(Watch watch) throws JSONException {
        String state = watch.latest.toString();
        // The public revision tracks committed settings, not runtime/C changes.
        if (watch.pending == null || state.equals(watch.deliveredState)) return;
        int request = watch.pending;
        watch.pending = null;
        watch.deliveredState = state;
        reply(watch.client, request, new JSONObject().put("state", watch.latest), null);
    }

    private JSONObject snapshot(String id) throws JSONException {
        String state = nativeSnapshot(nativeHandle, id);
        if (state == null) throw new IllegalStateException("Unknown service");
        return new JSONObject(state).put("lastHostExit", lastHostExit == null ? JSONObject.NULL : lastHostExit);
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
    private void submitFileWork(long work) {
        java.util.concurrent.ForkJoinPool.commonPool().execute(() -> nativeFileWork(work));
    }

    private void postTask(long task) { handler.post(() -> nativeRunTask(nativeHandle, task)); }

    private void delayTask(long task, long milliseconds) {
        Runnable action = () -> {
            delayedTasks.remove(task);
            nativeRunTask(nativeHandle, task);
        };
        delayedTasks.put(task, action);
        handler.postDelayed(action, milliseconds);
    }

    private void cancelTask(long task) {
        Runnable action = delayedTasks.remove(task);
        if (action != null) handler.removeCallbacks(action);
    }

    private void saveAutostart(String id, boolean enabled, long task) {
        preferences.save(id, enabled, (ignored, error) -> nativeSaved(nativeHandle, task, error));
    }

    private String restrictedBootType(String types) {
        int api = Math.min(Build.VERSION.SDK_INT, context.getApplicationInfo().targetSdkVersion);
        for (String type : types.split("\\|")) {
            switch (type) {
                case "microphone":
                    if (api >= 34) return type;
                    break;
                case "camera": case "dataSync": case "mediaPlayback": case "mediaProjection": case "phoneCall":
                    if (api >= 35) return type;
                    break;
                default: break;
            }
        }
        return null;
    }

    private void deferStart(String id, long generation, long attemptEpoch, String reason) {
        if (!nativeStartFailed(nativeHandle, id, generation, reason)) return;
        deferredBootStarts.put(id, generation);
        Log.w("MuonNativeService", id + ": " + reason);
        // The Activity may have resumed after startForegroundService but before
        // Android rejected the promotion in onStartCommand.
        if (resumeEpoch > attemptEpoch) nativeRetryResidentStart(nativeHandle, id, generation);
    }

    private void startComponent(String id, long generation) {
        boolean boot = dispatchingBootStarts && resumeEpoch == 0;
        boolean retry = deferredBootStarts.remove(id) != null;
        long attemptEpoch = boot || retry ? resumeEpoch : -1;
        try {
            JSONObject options = definition(id).getJSONObject("android");
            boolean onDemand = "on-demand".equals(options.optString("foregroundMode"));
            if (boot && !onDemand) {
                String type = restrictedBootType(options.getString("foregroundServiceType"));
                if (type != null) {
                    deferStart(id, generation, attemptEpoch, "BOOT_COMPLETED cannot start foreground service type " + type);
                    return;
                }
            }
            if ("quickjs".equals(definition(id).getString("runtime"))) {
                SourceStart start = new SourceStart(generation);
                start.timeout = () -> {
                    if (sourceStarts.get(id) == start)
                        nativeStartFailed(nativeHandle, id, generation, "Service source startup deadline exceeded");
                };
                sourceStarts.put(id, start);
                handler.postDelayed(start.timeout, 10000);
            }
            String component = options.getString("component");
            Intent intent = new Intent().setClassName(context, component)
                    .putExtra("generation", generation).putExtra("bootAttemptEpoch", attemptEpoch);
            if (onDemand) {
                GenerationBinding binding = new GenerationBinding(id, generation);
                if (!context.bindService(intent, binding, Context.BIND_AUTO_CREATE))
                    throw new IllegalStateException("Android refused the service binding");
                bindings.put(id, binding);
            } else if (Build.VERSION.SDK_INT >= 26) context.startForegroundService(intent);
            else context.startService(intent);
        } catch (JSONException | RuntimeException error) {
            startFailed(id, generation, attemptEpoch, error);
        }
    }

    private void stopComponent(String id, long generation) {
        SourceStart start = sourceStarts.get(id);
        if (start != null && start.generation == generation) {
            sourceStarts.remove(id);
            handler.removeCallbacks(start.timeout);
        }
        MuonNativeService component = components.get(id);
        if (component != null) component.stopGeneration(generation);
        GenerationBinding binding = bindings.get(id);
        if (binding != null && binding.generation == generation) {
            bindings.remove(id);
            context.unbindService(binding);
        }
    }

    // JNI dispatcher operation. A null result means Android accepted the new types.
    private String[] setForegroundServiceTypes(String id, long generation, String parameters) {
        GenerationBinding binding = bindings.get(id);
        if (binding == null) return new String[]{"permission-denied", "On-demand foreground execution is not configured"};
        MuonNativeService component = components.get(id);
        if (component == null || generation != binding.generation) return new String[]{"cancelled", "Service generation has stopped"};
        try {
            String declared = definition(id).getJSONObject("android").getString("foregroundServiceType");
            component.setForegroundTypes(generation, declared, parameters);
            return null;
        } catch (JSONException | IllegalArgumentException error) {
            return new String[]{"invalid-input", error.toString()};
        } catch (SecurityException | IllegalStateException error) {
            return new String[]{"permission-denied", error.toString()};
        }
    }

    void foregroundReady(MuonNativeService component, String id, long generation) {
        components.put(id, component);
        SourceStart start = sourceStarts.get(id);
        if (start == null) {
            if (!nativeForegroundReady(nativeHandle, id, generation, null, 0)) component.stopGeneration(generation);
            return;
        }
        if (start.generation != generation) { component.stopGeneration(generation); return; }
        MuonServiceSource.read(context, definition(id).optString("asset"), handler, (source, error) -> {
            if (sourceStarts.get(id) != start) return;
            sourceStarts.remove(id);
            handler.removeCallbacks(start.timeout);
            long remaining = start.deadline - SystemClock.elapsedRealtime();
            if (error != null || remaining <= 0) {
                nativeStartFailed(nativeHandle, id, generation,
                        error == null ? "Service source startup deadline exceeded" : error);
            } else if (!nativeForegroundReady(nativeHandle, id, generation, source, remaining)) {
                component.stopGeneration(generation);
            }
        });
    }

    void startFailed(String id, long generation, long attemptEpoch, Exception error) {
        boolean denied = error instanceof SecurityException || (Build.VERSION.SDK_INT >= 31
                ? error instanceof android.app.ForegroundServiceStartNotAllowedException
                : error instanceof IllegalStateException);
        if (attemptEpoch >= 0 && denied) deferStart(id, generation, attemptEpoch, error.toString());
        else nativeStartFailed(nativeHandle, id, generation, error.toString());
    }

    void runtimeStop(String id, long generation, String reason, boolean timeout) {
        nativeRuntimeStop(nativeHandle, id, generation, reason, timeout);
    }

    void componentDestroyed(MuonNativeService component, String id, long generation, boolean stopped) {
        if (components.get(id) == component) components.remove(id);
        if (!stopped) runtimeStop(id, generation, "Android destroyed the service component", false);
    }

    private void onEvent(String json) {
        try {
            JSONObject event = new JSONObject(json);
            String kind = event.getString("kind");
            if ("changed".equals(kind)) {
                String id = event.getJSONObject("snapshot").getString("id");
                // Coalesce on the current snapshot, never a stale queued event.
                JSONObject state = snapshot(id);
                if (!state.getBoolean("autostart")) deferredBootStarts.remove(id);
                for (Watch watch : new ArrayList<>(watches.values())) {
                    if (!watch.serviceId.equals(id)) continue;
                    watch.latest = state;
                    deliverWatch(watch);
                }
                return;
            }
            if ("autostartResult".equals(kind)) {
                SaveRequest save = saves.remove(event.getString("operationId"));
                if (save != null) reply(save.client, save.request, new JSONObject(),
                        event.getBoolean("success") ? null : event.getString("error"));
                return;
            }
            if (!"acquired".equals(kind) && !"acquireFailed".equals(kind)) return;
            Client client = owners.get(event.getString("owner"));
            if (client == null) return;
            JSONObject reference = event.getJSONObject("reference");
            Integer request = client.acquisitions.remove(reference.getString("refId"));
            if (request != null) reply(client, request, reference,
                    "acquired".equals(kind) ? null : event.getString("error"));
        } catch (JSONException error) { Log.e("MuonNativeService", "Invalid native service event", error); }
    }

    private static native long nativeCreate(MuonServiceProcess owner, String[] ids, String[] runtimes,
            String[] libraries, String[] directories, String[] ioPolicies, boolean[] autostarts, int[] overrides,
            String[][] configurations, String[][] pluginGrants, String pluginRegistry);
    private static native void nativeFileWork(long work);
    private static native void nativeRunTask(long handle, long task);
    private static native void nativeActivate(long handle);
    private static native void nativeRetryResidentStart(long handle, String id, long generation);
    private static native String nativeSetAutostart(long handle, String id, boolean enabled);
    private static native void nativeSaved(long handle, long task, String error);
    private static native String nativeAcquire(long handle, String id, String owner);
    private static native boolean nativeRelease(long handle, String id, String owner, String reference, String generation);
    private static native void nativeReleaseOwner(long handle, String owner);
    private static native void nativeOpenRpc(long handle, String owner, int descriptor);
    private static native String nativeSnapshot(long handle, String id);
    private static native boolean nativeForegroundReady(long handle, String id, long generation, String source, long remaining);
    private static native boolean nativeStartFailed(long handle, String id, long generation, String error);
    private static native void nativeSessionEnding(long handle);
    private static native void nativeRuntimeStop(long handle, String id, long generation, String error, boolean timeout);
}

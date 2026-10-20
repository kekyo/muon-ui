/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
package dev.muon.runtime;

import android.content.Context;
import android.os.Handler;
import android.os.Looper;
import androidx.annotation.Nullable;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.Set;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.function.Consumer;

/** Serializes application-owned notification I/O without retaining an Activity or WebView. */
final class MuonNotificationRuntime {
    interface Operation { Object run(MuonNotificationStore store) throws Exception; }
    interface Result { void complete(Object value); void fail(String diagnostic); }
    private static MuonNotificationRuntime instance;
    private final MuonNotificationStore store;
    private final ExecutorService executor = Executors.newSingleThreadExecutor();
    private final Handler main = new Handler(Looper.getMainLooper());
    private final Set<Runnable> listeners = new HashSet<>();

    private MuonNotificationRuntime(Context context) {
        store = new MuonNotificationStore(context, "muon_notifications.db", System::currentTimeMillis);
    }

    static synchronized MuonNotificationRuntime get(Context context) {
        if (instance == null) { instance = new MuonNotificationRuntime(context.getApplicationContext()); }
        return instance;
    }

    void execute(Operation operation, Result result) {
        executor.execute(() -> {
            try {
                Object value = operation.run(store);
                main.post(() -> result.complete(value));
            } catch (Exception error) {
                String diagnostic = error.getMessage() == null ? error.getClass().getSimpleName() : error.getMessage();
                main.post(() -> result.fail(diagnostic));
            }
        });
    }

    void activate(String token, String type, @Nullable String actionId, Consumer<String> cancel, Result result) {
        execute(store -> store.activate(token, type, actionId, cancel), new Result() {
            @Override public void complete(Object value) {
                if (Boolean.TRUE.equals(value)) { eventsChanged(); }
                result.complete(value);
            }
            @Override public void fail(String diagnostic) {
                // OS removal may fail after the event commits. Readers verify the durable
                // snapshot, so waking them also remains safe when persistence itself failed.
                eventsChanged();
                result.fail(diagnostic);
            }
        });
    }

    void addListener(Runnable listener) { listeners.add(listener); }
    void removeListener(Runnable listener) { listeners.remove(listener); }
    void eventsChanged() {
        for (Runnable listener : new ArrayList<>(listeners)) { listener.run(); }
    }
}

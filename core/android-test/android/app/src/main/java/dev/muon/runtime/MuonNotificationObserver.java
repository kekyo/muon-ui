/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
package dev.muon.runtime;

import android.app.NotificationManager;
import android.service.notification.NotificationListenerService;
import android.service.notification.StatusBarNotification;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Semaphore;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.function.Predicate;

/** Observes only test-host notification changes; production applications do not include this service. */
public final class MuonNotificationObserver extends NotificationListenerService {
    private static final CountDownLatch connected = new CountDownLatch(1);
    private static final Semaphore changes = new Semaphore(0);

    @Override public void onListenerConnected() { connected.countDown(); changes.release(); }
    @Override public void onNotificationPosted(StatusBarNotification notification) { changed(notification); }
    @Override public void onNotificationRemoved(StatusBarNotification notification) { changed(notification); }

    private void changed(StatusBarNotification notification) {
        if (getPackageName().equals(notification.getPackageName())) { changes.release(); }
    }

    static boolean awaitConnected(long timeout, TimeUnit unit) throws InterruptedException {
        return connected.await(timeout, unit);
    }

    static StatusBarNotification[] awaitActive(NotificationManager manager,
            Predicate<StatusBarNotification[]> expected) throws Exception {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(30);
        while (true) {
            StatusBarNotification[] active = manager.getActiveNotifications();
            if (expected.test(active)) { return active; }
            long remaining = deadline - System.nanoTime();
            if (remaining <= 0 || !changes.tryAcquire(remaining, TimeUnit.NANOSECONDS)) {
                throw new TimeoutException("Expected OS notification state did not arrive");
            }
        }
    }
}

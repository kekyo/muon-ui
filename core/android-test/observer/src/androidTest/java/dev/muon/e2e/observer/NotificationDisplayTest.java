// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

package dev.muon.e2e.observer;

import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;

import android.accessibilityservice.AccessibilityServiceInfo;
import android.app.UiAutomation;
import android.util.Base64;
import android.view.accessibility.AccessibilityNodeInfo;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.concurrent.Semaphore;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.Test;
import org.junit.runner.RunWith;

/** Observes a notification in System UI without requiring the whole screen to become idle. */
@RunWith(AndroidJUnit4.class)
public final class NotificationDisplayTest {
    private AccessibilityNodeInfo findTitle(UiAutomation automation, String title, AtomicReference<String> diagnostic) {
        var windows = automation.getWindows();
        StringBuilder detail = new StringBuilder("Interactive windows: " + windows.size());
        for (var window : windows) {
            AccessibilityNodeInfo root = window.getRoot();
            if (root == null) continue;
            detail.append("; package=").append(root.getPackageName()).append(" children=").append(root.getChildCount());
            if (root.getPackageName() == null || !"com.android.systemui".contentEquals(root.getPackageName())) continue;
            ArrayDeque<AccessibilityNodeInfo> pending = new ArrayDeque<>();
            pending.add(root);
            while (!pending.isEmpty()) {
                AccessibilityNodeInfo node = pending.removeFirst();
                CharSequence text = node.getText();
                if (text != null && text.toString().equals(title)) {
                    detail.append("; matched title visible=").append(node.isVisibleToUser());
                    if (node.isVisibleToUser()) return node;
                }
                for (int index = 0; index < node.getChildCount(); index++) {
                    AccessibilityNodeInfo child = node.getChild(index);
                    if (child != null) pending.add(child);
                }
            }
        }
        diagnostic.set(detail.toString());
        return null;
    }

    @Test public void observesNotificationTitle() throws Exception {
        var arguments = InstrumentationRegistry.getArguments();
        String encoded = arguments.getString("expectedBase64");
        assertNotNull("Specify the notification title in expectedBase64", encoded);
        String title = new String(Base64.decode(encoded, Base64.DEFAULT), StandardCharsets.UTF_8);
        String containerEncoded = arguments.getString("notificationTitleBase64");
        String containerTitle = containerEncoded == null ? null
                : new String(Base64.decode(containerEncoded, Base64.DEFAULT), StandardCharsets.UTF_8);
        UiAutomation automation = InstrumentationRegistry.getInstrumentation().getUiAutomation();
        var info = automation.getServiceInfo();
        info.flags |= AccessibilityServiceInfo.FLAG_RETRIEVE_INTERACTIVE_WINDOWS
                | AccessibilityServiceInfo.FLAG_INCLUDE_NOT_IMPORTANT_VIEWS;
        automation.setServiceInfo(info);
        Semaphore changed = new Semaphore(1);
        AtomicReference<String> diagnostic = new AtomicReference<>("No snapshot");
        // Accessibility callbacks run on the main thread. Read the remote tree
        // on the instrumentation thread so callback delivery remains available.
        automation.setOnAccessibilityEventListener(event -> changed.release());
        try {
            long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(60);
            AccessibilityNodeInfo found = null;
            boolean expanded = false;
            long remaining;
            while ((remaining = deadline - System.nanoTime()) > 0
                    && changed.tryAcquire(remaining, TimeUnit.NANOSECONDS)) {
                changed.drainPermits();
                found = findTitle(automation, title, diagnostic);
                if (found != null) break;
                if (!expanded && containerTitle != null) {
                    // Only expand the row containing this test's exact title.
                    // Other notifications can make this row initially collapsed.
                    AccessibilityNodeInfo container = findTitle(automation, containerTitle, diagnostic);
                    while (container != null) {
                        if (container.getActionList().contains(
                                AccessibilityNodeInfo.AccessibilityAction.ACTION_EXPAND)) {
                            assertTrue("The test notification must expand",
                                    container.performAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_EXPAND.getId()));
                            expanded = true;
                            break;
                        }
                        container = container.getParent();
                    }
                }
            }
            assertNotNull("The OS notification title must be visible: " + title + ". " + diagnostic.get(), found);
            if ("true".equals(arguments.getString("click"))) {
                AccessibilityNodeInfo node = findTitle(automation, title, diagnostic);
                while (node != null && !node.isClickable()) node = node.getParent();
                assertNotNull("The notification must expose a click action", node);
                assertTrue("The notification click must be accepted", node.performAction(AccessibilityNodeInfo.ACTION_CLICK));
            }
        } finally {
            automation.setOnAccessibilityEventListener(null);
        }
    }
}

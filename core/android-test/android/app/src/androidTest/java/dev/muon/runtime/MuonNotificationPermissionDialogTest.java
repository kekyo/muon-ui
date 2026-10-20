/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

package dev.muon.runtime;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;

import android.Manifest;
import android.accessibilityservice.AccessibilityService;
import android.accessibilityservice.AccessibilityServiceInfo;
import android.app.UiAutomation;
import android.app.Notification;
import android.app.NotificationManager;
import android.app.NotificationChannel;
import android.content.Context;
import android.content.pm.PackageManager;
import android.os.Build;
import android.util.Log;
import android.view.accessibility.AccessibilityNodeInfo;
import androidx.lifecycle.Lifecycle;
import androidx.test.core.app.ActivityScenario;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import org.json.JSONObject;
import org.junit.Test;
import org.junit.runner.RunWith;
import java.util.concurrent.Semaphore;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import java.util.ArrayDeque;

/** Drives the actual OS permission dialog, including dismissal and Activity destruction. */
@RunWith(AndroidJUnit4.class)
public final class MuonNotificationPermissionDialogTest {
    @Test
    public void destroysPendingRequestsAndDistinguishesDismissDenyAndGrant() throws Exception {
        if (Build.VERSION.SDK_INT < 33) { return; }
        var instrumentation = InstrumentationRegistry.getInstrumentation();
        Context context = instrumentation.getTargetContext();
        // Revoking a permission can terminate this instrumentation's own UID.
        // The connected-test Gradle task resets POST_NOTIFICATIONS and its
        // user-set/user-fixed flags with adb before instrumentation starts.
        assertEquals("The connected-test task must reset POST_NOTIFICATIONS before instrumentation",
                PackageManager.PERMISSION_DENIED, context.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS));
        context.getSharedPreferences("muon.notifications", Context.MODE_PRIVATE).edit().clear().apply();
        UiAutomation automation = instrumentation.getUiAutomation();
        AccessibilityServiceInfo automationInfo = automation.getServiceInfo();
        int originalFlags = automationInfo.flags;
        automationInfo.flags |= AccessibilityServiceInfo.FLAG_REPORT_VIEW_IDS
                | AccessibilityServiceInfo.FLAG_RETRIEVE_INTERACTIVE_WINDOWS;
        automation.setServiceInfo(automationInfo);

        try {
            MuonRpcBridge destroyedBridge;
            MuonActivity destroyedActivity;
            try (ActivityScenario<MuonActivity> scenario = ActivityScenario.launch(MuonActivity.class)) {
                AtomicReference<MuonActivity> reference = new AtomicReference<>();
                scenario.onActivity(reference::set);
                destroyedActivity = reference.get();
                assertTrue(destroyedActivity.awaitPageReadyForTest(30, TimeUnit.SECONDS));
                destroyedBridge = destroyedActivity.getRpcBridgeForTest();
                destroyedActivity.clearTestMessages();
                scenario.onActivity(current -> current.getWebViewForTest().evaluateJavascript(
                        "void (async()=>{try{await __muon_plugin_call('notifications-capability','muon.notifications.post',"
                                + "[JSON.stringify({id:'denied',title:'Denied',body:''})]);muonAndroidTest.postMessage('unexpected');}"
                                + "catch(error){muonAndroidTest.postMessage(String(error));}})();", null));
                String denied = destroyedActivity.awaitTestMessage(30, TimeUnit.SECONDS);
                assertNotNull(denied);
                assertTrue(denied, denied.contains("runtime permission is not granted"));
                requestDialog(scenario, destroyedActivity, automation);
                assertEquals(1, destroyedActivity.getNativePendingCallCountForTest());
                scenario.onActivity(MuonActivity::releaseNativeContextForTest);
                assertEquals(0, destroyedActivity.getNativePendingCallCountForTest());
                assertTrue(automation.performGlobalAction(AccessibilityService.GLOBAL_ACTION_BACK));
                scenario.moveToState(Lifecycle.State.RESUMED);
                destroyedActivity.clearFinishedPageUrlsForTest();
                scenario.onActivity(current -> current.getWebViewForTest().reload());
                assertEquals(MuonActivity.TRUSTED_ORIGIN + "/index.html",
                        destroyedActivity.awaitFinishedPageUrlForTest(30, TimeUnit.SECONDS));
                // An old permission result must not settle a later document's request.
                requestDialog(scenario, destroyedActivity, automation);
                assertEquals(1, destroyedActivity.getNativePendingCallCountForTest());
            }
            assertTrue(destroyedActivity.awaitDestroyedForTest(30, TimeUnit.SECONDS));
            instrumentation.runOnMainSync(() -> assertEquals(0, destroyedBridge.getNativePendingCallCount()));

            try (ActivityScenario<MuonActivity> scenario = ActivityScenario.launch(MuonActivity.class)) {
                AtomicReference<MuonActivity> reference = new AtomicReference<>();
                scenario.onActivity(reference::set);
                MuonActivity activity = reference.get();
                assertTrue(activity.awaitPageReadyForTest(30, TimeUnit.SECONDS));
                for (String action : new String[] { "dismiss", "deny", "allow" }) {
                    requestDialog(scenario, activity, automation);
                    if (action.equals("dismiss")) {
                        // A second call cannot replace the first callback while Android owns the dialog.
                        scenario.onActivity(current -> current.getWebViewForTest().evaluateJavascript(
                                "void (async()=>{try{await __muon_plugin_call('android-notifications-capability',"
                                        + "'muon.android.notifications.requestPermission',[]);"
                                        + "muonAndroidTest.postMessage('unexpected-second-request');}catch(error){"
                                        + "muonAndroidTest.postMessage('rejected-second-request');}})();", null));
                        assertEquals("rejected-second-request", activity.awaitTestMessage(30, TimeUnit.SECONDS));
                        assertTrue(automation.performGlobalAction(AccessibilityService.GLOBAL_ACTION_BACK));
                    } else {
                        AccessibilityNodeInfo root = automation.getRootInActiveWindow();
                        assertNotNull(root);
                        AccessibilityNodeInfo button = permissionButton(root, action);
                        assertNotNull("The OS permission action must exist", button);
                        assertTrue(button.performAction(AccessibilityNodeInfo.ACTION_CLICK));
                    }
                    String message = activity.awaitTestMessage(30, TimeUnit.SECONDS);
                    assertNotNull("Permission request must settle after " + action, message);
                    JSONObject reply = new JSONObject(message);
                    assertTrue(reply.toString(), reply.getBoolean("ok"));
                    JSONObject result = new JSONObject(reply.getString("value"));
                    Log.i("MuonPermissionTest", action + ": " + result);
                    JSONObject state = result.getJSONObject("state");
                    assertTrue(state.getBoolean("requestedBefore"));
                    assertEquals(5, state.length());
                    if (action.equals("allow")) {
                        assertEquals("granted", result.getString("result"));
                        assertEquals("granted", state.getString("runtimePermission"));
                        assertTrue(state.getBoolean("notificationsEnabled"));
                        assertFalse(state.getBoolean("settingsSuggested"));
                    } else {
                        assertEquals("denied", state.getString("runtimePermission"));
                        assertFalse(state.getBoolean("notificationsEnabled"));
                        if (action.equals("deny")) {
                            assertEquals("not-granted", result.getString("result"));
                        } else {
                            assertTrue(result.toString(), result.getString("result").equals("interrupted")
                                    || result.getString("result").equals("not-granted"));
                        }
                    }
                    scenario.moveToState(Lifecycle.State.RESUMED);
                    assertEquals(0, activity.getNativePendingCallCountForTest());
                }
                NotificationManager manager = activity.getSystemService(NotificationManager.class);
                if (Build.VERSION.SDK_INT >= 26) {
                    manager.createNotificationChannel(new NotificationChannel("post-blocked", "Blocked", NotificationManager.IMPORTANCE_NONE));
                    activity.clearTestMessages();
                    scenario.onActivity(current -> current.getWebViewForTest().evaluateJavascript(
                            "void (async()=>{try{await __muon_plugin_call('notifications-capability','muon.notifications.post',"
                                    + "[JSON.stringify({id:'blocked',title:'Blocked',body:'',category:'post-blocked'})]);muonAndroidTest.postMessage('unexpected');}"
                                    + "catch(error){muonAndroidTest.postMessage(String(error));}})();", null));
                    String blocked = activity.awaitTestMessage(30, TimeUnit.SECONDS);
                    assertNotNull(blocked);
                    assertTrue(blocked, blocked.contains("notification channel is disabled"));
                }
                String[][] operations = {
                        { "post", "{\"id\":\"posting-a\",\"title\":\"First\",\"body\":\"Body A\"}" },
                        { "post", "{\"id\":\"posting-b\",\"title\":\"Second\",\"body\":\"Body B\",\"category\":\"quiet\"}" },
                        { "post", "{\"id\":\"posting-a\",\"title\":\"Updated\",\"body\":\"Changed\"}" },
                        { "cancel", "posting-a" },
                        { "cancel", "posting-b" }
                };
                for (int index = 0; index < operations.length; index++) {
                    String[] operation = operations[index];
                    activity.clearTestMessages();
                    scenario.onActivity(current -> current.getWebViewForTest().evaluateJavascript(
                            "void (async()=>{try{await __muon_plugin_call('notifications-capability','muon.notifications."
                                    + operation[0] + "',[" + JSONObject.quote(operation[1]) + "]);"
                                    + "muonAndroidTest.postMessage('posted');}catch(error){muonAndroidTest.postMessage(String(error));}})();", null));
                    assertEquals("posted", activity.awaitTestMessage(30, TimeUnit.SECONDS));
                    var notifications = manager.getActiveNotifications();
                    assertEquals(new int[] { 1, 2, 2, 1, 0 }[index], notifications.length);
                    for (var notification : notifications) {
                        assertNotNull(notification.getNotification().getSmallIcon());
                        assertNotNull(notification.getNotification().contentIntent);
                        assertTrue((notification.getNotification().flags & Notification.FLAG_AUTO_CANCEL) != 0);
                        if (notification.getTag().equals("posting-a")) {
                            assertEquals(index == 2 ? "Updated" : "First",
                                    notification.getNotification().extras.getString(Notification.EXTRA_TITLE));
                            if (Build.VERSION.SDK_INT >= 26) {
                                assertEquals("general", notification.getNotification().getChannelId());
                            }
                        } else {
                            assertEquals("posting-b", notification.getTag());
                            assertEquals("Second", notification.getNotification().extras.getString(Notification.EXTRA_TITLE));
                        }
                    }
                }
                scenario.recreate();
                scenario.onActivity(reference::set);
                MuonActivity recreated = reference.get();
                assertTrue(recreated.awaitPageReadyForTest(30, TimeUnit.SECONDS));
                recreated.clearTestMessages();
                scenario.onActivity(current -> current.getWebViewForTest().evaluateJavascript(
                        "void (async()=>{const value=await __muon_plugin_call('android-notifications-capability',"
                                + "'muon.android.notifications.getPermissionState',[]);muonAndroidTest.postMessage(value);})();", null));
                String restored = recreated.awaitTestMessage(30, TimeUnit.SECONDS);
                assertNotNull(restored);
                JSONObject state = new JSONObject(restored);
                assertEquals("granted", state.getString("runtimePermission"));
                assertTrue(state.getBoolean("requestedBefore"));
            }
        } finally {
            automationInfo.flags = originalFlags;
            automation.setServiceInfo(automationInfo);
            automation.setOnAccessibilityEventListener(null);
        }
    }

    private static void requestDialog(ActivityScenario<MuonActivity> scenario, MuonActivity activity,
            UiAutomation automation) throws Exception {
        Semaphore changes = new Semaphore(0);
        AccessibilityNodeInfo previousRoot = automation.getRootInActiveWindow();
        int previousWindowId = previousRoot == null ? -1 : previousRoot.getWindowId();
        // UiAutomation delivers callbacks on the application's main thread.
        // Query accessibility nodes on the instrumentation thread so the UI can respond.
        automation.setOnAccessibilityEventListener(event -> changes.release());
        try {
            activity.clearTestMessages();
            scenario.onActivity(current -> current.getWebViewForTest().evaluateJavascript(
                    "void (async()=>{try{const value=await __muon_plugin_call('android-notifications-capability',"
                            + "'muon.android.notifications.requestPermission',[]);"
                            + "muonAndroidTest.postMessage(JSON.stringify({ok:true,value}));}catch(error){"
                            + "muonAndroidTest.postMessage(JSON.stringify({ok:false,error:String(error)}));}})();", null));
            long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(30);
            while (true) {
                AccessibilityNodeInfo root = automation.getRootInActiveWindow();
                var window = root == null ? null : root.getWindow();
                // A closing dialog's cached nodes can outlive its Activity. Wait for
                // the new window to own input focus before sending Back or clicking.
                if (root != null && root.getWindowId() != previousWindowId
                        && window != null && window.isFocused() && permissionButton(root, "allow") != null) { break; }
                long remaining = deadline - System.nanoTime();
                assertTrue("The actual OS notification dialog must appear", remaining > 0
                        && changes.tryAcquire(remaining, TimeUnit.NANOSECONDS));
            }
        } finally {
            automation.setOnAccessibilityEventListener(null);
        }
    }

    private static AccessibilityNodeInfo permissionButton(AccessibilityNodeInfo root, String action) {
        ArrayDeque<AccessibilityNodeInfo> nodes = new ArrayDeque<>();
        nodes.add(root);
        while (!nodes.isEmpty()) {
            AccessibilityNodeInfo node = nodes.remove();
            String id = node.getViewIdResourceName();
            // Google and AOSP PermissionController can use different package and resource namespaces.
            if (id != null && id.endsWith(":id/permission_" + action + "_button")) { return node; }
            for (int i = 0; i < node.getChildCount(); i++) {
                AccessibilityNodeInfo child = node.getChild(i);
                if (child != null) { nodes.add(child); }
            }
        }
        return null;
    }
}

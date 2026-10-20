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
import android.content.pm.ActivityInfo;
import android.content.res.Configuration;
import android.os.Build;
import android.os.ParcelFileDescriptor;
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
                setNotificationObservation(automation, true);
                assertTrue(MuonNotificationObserver.awaitConnected(30, TimeUnit.SECONDS));
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
                    final int step = index;
                    var notifications = MuonNotificationObserver.awaitActive(manager, active -> {
                        if (active.length != new int[] { 1, 2, 2, 1, 0 }[step]) { return false; }
                        for (var notification : active) {
                            if (notification.getTag().equals("posting-a") && !(step == 2 ? "Updated" : "First")
                                    .equals(notification.getNotification().extras.getString(Notification.EXTRA_TITLE))) { return false; }
                        }
                        return true;
                    });
                    assertEquals(new int[] { 1, 2, 2, 1, 0 }[index], notifications.length);
                    for (var notification : notifications) {
                        assertNotNull(notification.getNotification().getSmallIcon());
                        assertNotNull(notification.getNotification().contentIntent);
                        // Notification removal must follow the durable operation commit.
                        assertEquals(0, notification.getNotification().flags & Notification.FLAG_AUTO_CANCEL);
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
                activity.clearTestMessages();
                scenario.onActivity(current -> current.getWebViewForTest().evaluateJavascript("""
                        void (async()=>{try{await __muon_plugin_call('notifications-capability','muon.notifications.post',
                          [JSON.stringify({id:'event-default',title:'Event notification',body:'',data:{screen:'inbox'}})]);
                          muonAndroidTest.postMessage('event-posted');}catch(error){muonAndroidTest.postMessage(String(error));}})();
                        """, null));
                assertEquals("event-posted", activity.awaitTestMessage(30, TimeUnit.SECONDS));
                var posted = MuonNotificationObserver.awaitActive(manager, active -> active.length == 1);
                assertEquals(1, posted.length);
                // ActivityScenario matches the original Intent and cannot follow setIntent on notification activation.
                scenario.close();
                var monitor = instrumentation.addMonitor(MuonActivity.class.getName(), null, false);
                AtomicReference<MuonActivity> currentActivity = new AtomicReference<>();
                int originalRequestedOrientation = activity.getRequestedOrientation();
                try {
                    posted[0].getNotification().contentIntent.send();
                    MuonActivity activated = (MuonActivity) instrumentation.waitForMonitorWithTimeout(monitor, 30000);
                    assertNotNull(activated);
                    currentActivity.set(activated);
                    assertTrue(activated.awaitPageReadyForTest(30, TimeUnit.SECONDS));
                    activated.clearTestMessages();
                    instrumentation.runOnMainSync(() -> activated.getWebViewForTest().evaluateJavascript("""
                            void (async()=>{try{const events=await __muon_plugin_call('notifications-capability',
                              'muon.notifications.getPendingEvents',[]);muonAndroidTest.postMessage(events);
                            }catch(error){muonAndroidTest.postMessage(JSON.stringify({error:String(error)}));}})();
                            """, null));
                    String pendingEvents = activated.awaitTestMessage(30, TimeUnit.SECONDS);
                    assertNotNull(pendingEvents);
                    org.json.JSONArray events = new org.json.JSONArray(pendingEvents);
                    assertEquals(1, events.length());
                    JSONObject event = events.getJSONObject(0);
                    assertEquals("event-default", event.getString("id"));
                    assertEquals("activate", event.getString("type"));
                    assertTrue(event.isNull("actionId"));
                    assertEquals("inbox", event.getJSONObject("data").getString("screen"));
                    assertEquals(0, MuonNotificationObserver.awaitActive(manager, active -> active.length == 0).length);
                    var recreationMonitor = instrumentation.addMonitor(MuonActivity.class.getName(), null, false);
                    MuonActivity recreated;
                    try {
                        instrumentation.runOnMainSync(activated::recreate);
                        recreated = (MuonActivity) instrumentation.waitForMonitorWithTimeout(recreationMonitor, 30000);
                    } finally { instrumentation.removeMonitor(recreationMonitor); }
                    assertNotNull(recreated);
                    assertTrue(activated != recreated);
                    currentActivity.set(recreated);
                    assertTrue(recreated.awaitPageReadyForTest(30, TimeUnit.SECONDS));
                    recreated.clearTestMessages();
                    instrumentation.runOnMainSync(() -> recreated.getWebViewForTest().evaluateJavascript(
                            "void (async()=>{const value=await __muon_plugin_call('android-notifications-capability',"
                                    + "'muon.android.notifications.getPermissionState',[]);"
                                    + "const events=JSON.parse(await __muon_plugin_call('notifications-capability','muon.notifications.getPendingEvents',[]));"
                                    + "muonAndroidTest.postMessage(JSON.stringify({state:JSON.parse(value),events}));})();", null));
                    String restored = recreated.awaitTestMessage(30, TimeUnit.SECONDS);
                    assertNotNull(restored);
                    JSONObject restoredReply = new JSONObject(restored);
                    JSONObject state = restoredReply.getJSONObject("state");
                    assertEquals("granted", state.getString("runtimePermission"));
                    assertTrue(state.getBoolean("requestedBefore"));
                    assertEquals(1, restoredReply.getJSONArray("events").length());
                    assertEquals(event.getString("eventId"), restoredReply.getJSONArray("events").getJSONObject(0).getString("eventId"));
                    int originalOrientation = recreated.getResources().getConfiguration().orientation;
                    int otherOrientation = originalOrientation == Configuration.ORIENTATION_PORTRAIT
                            ? Configuration.ORIENTATION_LANDSCAPE : Configuration.ORIENTATION_PORTRAIT;
                    for (int orientation : new int[] { otherOrientation, originalOrientation }) {
                        MuonActivity previous = currentActivity.get();
                        var rotationMonitor = instrumentation.addMonitor(MuonActivity.class.getName(), null, false);
                        MuonActivity rotated;
                        try {
                            instrumentation.runOnMainSync(() -> previous.setRequestedOrientation(
                                    orientation == Configuration.ORIENTATION_PORTRAIT ? ActivityInfo.SCREEN_ORIENTATION_PORTRAIT
                                            : ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE));
                            rotated = (MuonActivity) instrumentation.waitForMonitorWithTimeout(rotationMonitor, 30000);
                        } finally { instrumentation.removeMonitor(rotationMonitor); }
                        assertNotNull(rotated);
                        assertTrue(previous != rotated);
                        currentActivity.set(rotated);
                        assertTrue(rotated.awaitResumedForTest(30, TimeUnit.SECONDS));
                        assertEquals(orientation, rotated.getResources().getConfiguration().orientation);
                        assertTrue(rotated.awaitPageReadyForTest(30, TimeUnit.SECONDS));
                        rotated.clearTestMessages();
                        instrumentation.runOnMainSync(() -> rotated.getWebViewForTest().evaluateJavascript(
                                "void (async()=>{muonAndroidTest.postMessage(await __muon_plugin_call('notifications-capability',"
                                        + "'muon.notifications.getPendingEvents',[]));})();", null));
                        String rotatedEvents = rotated.awaitTestMessage(30, TimeUnit.SECONDS);
                        assertNotNull(rotatedEvents);
                        var retained = new org.json.JSONArray(rotatedEvents);
                        assertEquals(1, retained.length());
                        assertEquals(event.toString(), retained.getJSONObject(0).toString());
                    }
                    String eventId = event.getString("eventId");
                    instrumentation.runOnMainSync(() -> currentActivity.get().getWebViewForTest().evaluateJavascript(
                            "void (async()=>{await __muon_plugin_call('notifications-capability','muon.notifications.acknowledgeEvent',["
                                    + JSONObject.quote(eventId) + "]);muonAndroidTest.postMessage('acknowledged');})();", null));
                    assertEquals("acknowledged", currentActivity.get().awaitTestMessage(30, TimeUnit.SECONDS));
                    instrumentation.runOnMainSync(() -> currentActivity.get().getWebViewForTest().evaluateJavascript("""
                            void (async()=>{await __muon_plugin_call('notifications-capability','muon.notifications.post',
                              [JSON.stringify({id:'event-action',title:'Action notification',body:'',data:{screen:'detail'},
                                actions:[{id:'default',title:'Open detail'}]})]);muonAndroidTest.postMessage('action-posted');})();
                            """, null));
                    assertEquals("action-posted", currentActivity.get().awaitTestMessage(30, TimeUnit.SECONDS));
                    var actionPosted = MuonNotificationObserver.awaitActive(manager, active -> active.length == 1);
                    assertEquals(1, actionPosted.length);
                    Notification actionNotification = actionPosted[0].getNotification();
                    assertEquals(1, actionNotification.actions.length);
                    actionNotification.actions[0].actionIntent.send();
                    assertNotNull(currentActivity.get().awaitNotificationIntentForTest(30, TimeUnit.SECONDS));
                    String readEvents = "void (async()=>{muonAndroidTest.postMessage(await __muon_plugin_call('notifications-capability',"
                            + "'muon.notifications.getPendingEvents',[]));})();";
                    instrumentation.runOnMainSync(() -> currentActivity.get().getWebViewForTest().evaluateJavascript(readEvents, null));
                    String actionEvents = currentActivity.get().awaitTestMessage(30, TimeUnit.SECONDS);
                    assertNotNull(actionEvents);
                    var operated = new org.json.JSONArray(actionEvents);
                    assertEquals(1, operated.length());
                    JSONObject actionEvent = operated.getJSONObject(0);
                    assertEquals("action", actionEvent.getString("type"));
                    assertEquals("default", actionEvent.getString("actionId"));
                    assertEquals("detail", actionEvent.getJSONObject("data").getString("screen"));
                    assertEquals(0, MuonNotificationObserver.awaitActive(manager, active -> active.length == 0).length);
                    // A delayed default tap cannot replace the revision's first named operation.
                    actionNotification.contentIntent.send();
                    assertNotNull(currentActivity.get().awaitNotificationIntentForTest(30, TimeUnit.SECONDS));
                    instrumentation.runOnMainSync(() -> currentActivity.get().getWebViewForTest().evaluateJavascript(readEvents, null));
                    String repeatedEvents = currentActivity.get().awaitTestMessage(30, TimeUnit.SECONDS);
                    assertNotNull(repeatedEvents);
                    var repeated = new org.json.JSONArray(repeatedEvents);
                    assertEquals(1, repeated.length());
                    assertEquals(actionEvent.toString(), repeated.getJSONObject(0).toString());
                    instrumentation.runOnMainSync(() -> currentActivity.get().getWebViewForTest().evaluateJavascript(
                            "void (async()=>{await __muon_plugin_call('notifications-capability','muon.notifications.acknowledgeEvent',["
                                    + JSONObject.quote(actionEvent.optString("eventId")) + "]);muonAndroidTest.postMessage('acknowledged');})();", null));
                    assertEquals("acknowledged", currentActivity.get().awaitTestMessage(30, TimeUnit.SECONDS));
                } finally {
                    instrumentation.removeMonitor(monitor);
                    instrumentation.runOnMainSync(() -> {
                        if (currentActivity.get() != null) {
                            currentActivity.get().setRequestedOrientation(originalRequestedOrientation);
                            currentActivity.get().finish();
                        }
                    });
                }
            }
        } finally {
            setNotificationObservation(automation, false);
            automationInfo.flags = originalFlags;
            automation.setServiceInfo(automationInfo);
            automation.setOnAccessibilityEventListener(null);
        }
    }

    private static void setNotificationObservation(UiAutomation automation, boolean enabled) throws Exception {
        String command = "cmd notification " + (enabled ? "allow_listener " : "disallow_listener ")
                + "dev.muon.testhost/dev.muon.runtime.MuonNotificationObserver";
        try (var stream = new ParcelFileDescriptor.AutoCloseInputStream(automation.executeShellCommand(command))) {
            byte[] output = new byte[1024];
            while (stream.read(output) != -1) { /* Wait for the shell command to finish. */ }
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

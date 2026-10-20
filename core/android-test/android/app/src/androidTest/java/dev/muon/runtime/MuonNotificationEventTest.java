/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

package dev.muon.runtime;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;

import androidx.test.core.app.ActivityScenario;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.webkit.WebViewCompat;
import androidx.webkit.WebMessageCompat;
import android.view.View;
import android.webkit.WebView;
import java.util.Collections;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.LinkedBlockingQueue;
import org.json.JSONObject;
import org.junit.Test;
import org.junit.runner.RunWith;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

/** Exercises notification event RPCs through the actual trusted document. */
@RunWith(AndroidJUnit4.class)
public final class MuonNotificationEventTest {
    @Test public void readsPendingEventsAndAcknowledgesUnknownEventsIdempotently() throws Exception {
        try (ActivityScenario<MuonActivity> scenario = ActivityScenario.launch(MuonActivity.class)) {
            AtomicReference<MuonActivity> reference = new AtomicReference<>();
            scenario.onActivity(reference::set);
            MuonActivity activity = reference.get();
            assertTrue(activity.awaitPageReadyForTest(30, TimeUnit.SECONDS));
            scenario.onActivity(current -> current.getWebViewForTest().evaluateJavascript("""
                    void (async()=>{try{
                      const call=(name,args)=>__muon_plugin_call('notifications-capability','muon.notifications.'+name,args);
                      await call('acknowledgeEvent',['unknown-event']);
                      await call('acknowledgeEvent',['unknown-event']);
                      const events=JSON.parse(await call('getPendingEvents',[]));
                      muonAndroidTest.postMessage(JSON.stringify({events}));
                    }catch(error){muonAndroidTest.postMessage(JSON.stringify({error:String(error)}));}})();
                    """, null));
            String message = activity.awaitTestMessage(30, TimeUnit.SECONDS);
            assertNotNull(message);
            JSONObject reply = new JSONObject(message);
            assertTrue(reply.toString(), reply.has("events"));
            assertEquals(0, reply.getJSONArray("events").length());
        }
    }

    @Test public void releasesAnIdleWatchAndItsPendingNextCall() throws Exception {
        try (ActivityScenario<MuonActivity> scenario = ActivityScenario.launch(MuonActivity.class)) {
            AtomicReference<MuonActivity> reference = new AtomicReference<>();
            scenario.onActivity(reference::set);
            MuonActivity activity = reference.get();
            assertTrue(activity.awaitPageReadyForTest(30, TimeUnit.SECONDS));
            scenario.onActivity(current -> current.getWebViewForTest().evaluateJavascript("""
                    void (async()=>{try{
                      const call=(value)=>__muon_plugin_call('notifications-capability','muon.notifications.watchEvents',[JSON.stringify(value)]);
                      const watch=JSON.parse(await call({operation:'acquire'}));
                      const next=call({operation:'next',token:watch.token,afterCursor:watch.cursor});
                      await call({operation:'release',token:watch.token});
                      await call({operation:'release',token:watch.token});
                      muonAndroidTest.postMessage(JSON.stringify({result:JSON.parse(await next)}));
                    }catch(error){muonAndroidTest.postMessage(JSON.stringify({error:String(error)}));}})();
                    """, null));
            String message = activity.awaitTestMessage(30, TimeUnit.SECONDS);
            assertNotNull(message);
            JSONObject reply = new JSONObject(message);
            assertTrue(reply.toString(), reply.has("result"));
            assertTrue(reply.getJSONObject("result").isNull("events"));
            assertEquals(0, activity.getNativePendingCallCountForTest());
        }
    }

    @Test public void releasesWatchesWhenTheDocumentContextIsDestroyed() throws Exception {
        try (ActivityScenario<MuonActivity> scenario = ActivityScenario.launch(MuonActivity.class)) {
            AtomicReference<MuonActivity> reference = new AtomicReference<>();
            scenario.onActivity(reference::set);
            MuonActivity activity = reference.get();
            assertTrue(activity.awaitPageReadyForTest(30, TimeUnit.SECONDS));
            scenario.onActivity(current -> current.getWebViewForTest().evaluateJavascript("""
                    void (async()=>{const call=value=>__muon_plugin_call('notifications-capability',
                      'muon.notifications.watchEvents',[JSON.stringify(value)]);
                      const watch=JSON.parse(await call({operation:'acquire'}));
                      const next=call({operation:'next',token:watch.token,afterCursor:watch.cursor});
                      muonAndroidTest.postMessage('watching');
                      try{await next;}catch{};
                    })();
                    """, null));
            assertEquals("watching", activity.awaitTestMessage(30, TimeUnit.SECONDS));
            scenario.onActivity(current -> {
                assertEquals(1, current.getRpcBridgeForTest().getActiveNotificationWatchCountForTest());
                current.releaseNativeContextForTest();
                assertEquals(0, current.getRpcBridgeForTest().getActiveNotificationWatchCountForTest());
                assertEquals(0, current.getNativePendingCallCountForTest());
            });
        }
    }

    @Test public void isolatesWebViewLeasesWhileSharingDurableEventsAndAcknowledgements() throws Exception {
        try (ActivityScenario<MuonActivity> scenario = ActivityScenario.launch(MuonActivity.class)) {
            AtomicReference<MuonActivity> reference = new AtomicReference<>();
            scenario.onActivity(reference::set);
            MuonActivity first = reference.get();
            assertTrue(first.awaitPageReadyForTest(30, TimeUnit.SECONDS));
            AtomicReference<MuonWebViewHost> secondHost = new AtomicReference<>();
            AtomicReference<WebView> secondView = new AtomicReference<>();
            AtomicReference<MuonRpcBridge> secondBridge = new AtomicReference<>();
            AtomicReference<View> firstContent = new AtomicReference<>();
            LinkedBlockingQueue<String> secondMessages = new LinkedBlockingQueue<>();
            CountDownLatch ready = new CountDownLatch(1);
            CountDownLatch notificationsChanged = new CountDownLatch(1);
            Runnable changed = notificationsChanged::countDown;
            try {
                scenario.onActivity(activity -> {
                    firstContent.set((View) activity.getWebViewForTest().getParent());
                    MuonWebViewHost host = new MuonWebViewHost(activity, new MuonWebViewHost.Listener() {
                        @Override public void beforePageLoad(WebView view, MuonRpcBridge bridge) {
                            secondView.set(view);
                            secondBridge.set(bridge);
                            WebViewCompat.addWebMessageListener(view, "muonAndroidTest", Collections.singleton(MuonActivity.TRUSTED_ORIGIN),
                                    (webView, message, origin, mainFrame, reply) -> {
                                        if (mainFrame && message.getType() == WebMessageCompat.TYPE_STRING && message.getData() != null) {
                                            secondMessages.add(message.getData());
                                        }
                                    });
                        }
                        @Override public void pageFinished(String url) { ready.countDown(); }
                        @Override public void mainFrameHttpError(int status) { throw new AssertionError("Second page HTTP failure: " + status); }
                        @Override public void startupFailed(String diagnostic) { throw new AssertionError(diagnostic); }
                    });
                    secondHost.set(host);
                    host.start("", MuonAppConfig.load(activity));
                });
                assertTrue(ready.await(30, TimeUnit.SECONDS));
                first.clearTestMessages();
                secondMessages.clear();
                String acquire = """
                        void (async()=>{globalThis.eventWatch=JSON.parse(await __muon_plugin_call('notifications-capability',
                          'muon.notifications.watchEvents',[JSON.stringify({operation:'acquire'})]));
                          muonAndroidTest.postMessage(JSON.stringify(eventWatch));})();
                        """;
                scenario.onActivity(activity -> activity.getWebViewForTest().evaluateJavascript(acquire, null));
                String firstAcquired = first.awaitTestMessage(30, TimeUnit.SECONDS);
                assertNotNull(firstAcquired);
                JSONObject firstWatch = new JSONObject(firstAcquired);
                String foreignNext = new JSONObject().put("operation", "next").put("token", firstWatch.getString("token"))
                        .put("afterCursor", firstWatch.getLong("cursor")).toString();
                scenario.onActivity(activity -> secondView.get().evaluateJavascript(
                        "void (async()=>{try{await __muon_plugin_call('notifications-capability','muon.notifications.watchEvents',["
                                + JSONObject.quote(foreignNext) + "]);muonAndroidTest.postMessage('unexpected');}"
                                + "catch(error){muonAndroidTest.postMessage(String(error));}})();", null));
                String foreignResult = secondMessages.poll(30, TimeUnit.SECONDS);
                assertNotNull(foreignResult);
                assertTrue(foreignResult, foreignResult.contains("subscription is not active"));
                scenario.onActivity(activity -> secondView.get().evaluateJavascript(acquire, null));
                assertNotNull(secondMessages.poll(30, TimeUnit.SECONDS));
                String next = """
                        void (async()=>{const waiting=__muon_plugin_call('notifications-capability',
                          'muon.notifications.watchEvents',[JSON.stringify({operation:'next',token:eventWatch.token,afterCursor:eventWatch.cursor})]);
                          muonAndroidTest.postMessage('waiting');muonAndroidTest.postMessage(await waiting);})();
                        """;
                scenario.onActivity(activity -> {
                    activity.getWebViewForTest().evaluateJavascript(next, null);
                    secondView.get().evaluateJavascript(next, null);
                });
                assertEquals("waiting", first.awaitTestMessage(30, TimeUnit.SECONDS));
                assertEquals("waiting", secondMessages.poll(30, TimeUnit.SECONDS));
                CountDownLatch committed = new CountDownLatch(1);
                AtomicReference<String> eventId = new AtomicReference<>();
                AtomicReference<String> failure = new AtomicReference<>();
                scenario.onActivity(activity -> {
                    MuonNotificationRuntime runtime = MuonNotificationRuntime.get(activity);
                    runtime.addListener(changed);
                    runtime.execute(store -> store.post("multiple-views", "{\"views\":2}", "[]", value -> {}), new MuonNotificationRuntime.Result() {
                        @Override public void complete(Object token) {
                            eventId.set((String) token);
                            runtime.activate((String) token, "activate", null,
                                    id -> { throw new IllegalStateException("OS removal failure"); }, new MuonNotificationRuntime.Result() {
                                @Override public void complete(Object value) { committed.countDown(); }
                                @Override public void fail(String diagnostic) { failure.set(diagnostic); committed.countDown(); }
                            });
                        }
                        @Override public void fail(String diagnostic) { failure.set(diagnostic); committed.countDown(); }
                    });
                });
                assertTrue(committed.await(30, TimeUnit.SECONDS));
                assertEquals("OS removal failure", failure.get());
                assertEquals("A saved operation must wake watchers even if OS removal fails", 0, notificationsChanged.getCount());
                String firstEvent = first.awaitTestMessage(30, TimeUnit.SECONDS);
                String secondEvent = secondMessages.poll(30, TimeUnit.SECONDS);
                assertNotNull(firstEvent);
                assertNotNull(secondEvent);
                assertEquals(eventId.get(), new JSONObject(firstEvent).getJSONArray("events").getJSONObject(0).getString("eventId"));
                assertEquals(eventId.get(), new JSONObject(secondEvent).getJSONArray("events").getJSONObject(0).getString("eventId"));
                scenario.onActivity(activity -> secondView.get().evaluateJavascript(
                        "void (async()=>{await __muon_plugin_call('notifications-capability','muon.notifications.acknowledgeEvent',["
                                + JSONObject.quote(eventId.get()) + "]);muonAndroidTest.postMessage('acknowledged');})();", null));
                assertEquals("acknowledged", secondMessages.poll(30, TimeUnit.SECONDS));
                String pending = "void (async()=>{muonAndroidTest.postMessage(await __muon_plugin_call('notifications-capability','muon.notifications.getPendingEvents',[]));})();";
                scenario.onActivity(activity -> {
                    activity.getWebViewForTest().evaluateJavascript(pending, null);
                    secondView.get().evaluateJavascript(pending, null);
                });
                assertEquals("[]", first.awaitTestMessage(30, TimeUnit.SECONDS));
                assertEquals("[]", secondMessages.poll(30, TimeUnit.SECONDS));
                scenario.onActivity(activity -> {
                    assertEquals(1, activity.getRpcBridgeForTest().getActiveNotificationWatchCountForTest());
                    assertEquals(1, secondBridge.get().getActiveNotificationWatchCountForTest());
                    activity.releaseNativeContextForTest();
                    assertEquals(0, activity.getRpcBridgeForTest().getActiveNotificationWatchCountForTest());
                    assertEquals(1, secondBridge.get().getActiveNotificationWatchCountForTest());
                });
            } finally {
                scenario.onActivity(activity -> {
                    MuonNotificationRuntime.get(activity).removeListener(changed);
                    if (secondHost.get() != null) { secondHost.get().close(false); }
                    if (firstContent.get() != null) { activity.setContentView(firstContent.get()); }
                });
            }
        }
    }
}

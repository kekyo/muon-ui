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

import android.os.Build;
import androidx.test.core.app.ActivityScenario;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.Test;
import org.junit.runner.RunWith;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

/** Observes notification capabilities, channel registration and strict RPC boundaries. */
@RunWith(AndroidJUnit4.class)
public final class MuonNotificationTest {
    @Test
    public void reportsCapabilitiesAndFreshApplicationAndCategoryState() throws Exception {
        try (ActivityScenario<MuonActivity> scenario = ActivityScenario.launch(MuonActivity.class)) {
            AtomicReference<MuonActivity> reference = new AtomicReference<>();
            scenario.onActivity(reference::set);
            MuonActivity activity = reference.get();
            assertTrue(activity.awaitPageReadyForTest(30, TimeUnit.SECONDS));
            JSONObject capabilities = value(call(scenario, activity, "notifications-capability",
                    "muon.notifications.getCapabilities", new JSONArray()));
            assertEquals(6, capabilities.length());
            for (String name : new String[] { "interaction", "launch", "actions" }) {
                assertEquals("unsupported", capabilities.getJSONObject(name).getString("state"));
            }
            assertEquals("supported", capabilities.getJSONObject("persistence").getString("state"));
            assertEquals("supported", capabilities.getJSONObject("appStatus").getString("state"));
            assertEquals(Build.VERSION.SDK_INT >= 26 ? "supported" : "unsupported",
                    capabilities.getJSONObject("categoryStatus").getString("state"));
            JSONObject all = value(call(scenario, activity, "notifications-capability",
                    "muon.notifications.getStatus", new JSONArray().put("{}")));
            assertTrue(all.isNull("category"));
            JSONObject category = value(call(scenario, activity, "notifications-capability",
                    "muon.notifications.getStatus", new JSONArray().put("{\"category\":\"general\"}")));
            assertEquals("general", category.getJSONObject("category").getString("id"));
            assertEquals(Build.VERSION.SDK_INT >= 26 ? "enabled" : "unknown",
                    category.getJSONObject("category").getString("state"));
        }
    }

    @Test
    public void ensuresChannelsWithoutOverwritingTheirEffectiveSettings() throws Exception {
        try (ActivityScenario<MuonActivity> scenario = ActivityScenario.launch(MuonActivity.class)) {
            AtomicReference<MuonActivity> reference = new AtomicReference<>();
            scenario.onActivity(reference::set);
            MuonActivity activity = reference.get();
            assertTrue(activity.awaitPageReadyForTest(30, TimeUnit.SECONDS));
            String path = "muon.android.notifications.";
            JSONObject capabilities = value(call(scenario, activity, "android-notifications-capability",
                    path + "getCapabilities", new JSONArray()));
            assertEquals(Build.VERSION.SDK_INT >= 26 ? "supported" : "unsupported",
                    capabilities.getJSONObject("channels").getString("state"));
            if (Build.VERSION.SDK_INT >= 26) {
                JSONObject general = value(call(scenario, activity, "android-notifications-capability",
                        path + "getChannel", new JSONArray().put("general")));
                assertEquals("default", general.getString("importance"));
                String id = "dynamic-" + System.nanoTime();
                JSONObject options = new JSONObject().put("id", id).put("name", "Dynamic channel").put("importance", "low");
                JSONObject created = value(call(scenario, activity, "android-notifications-capability",
                        path + "ensureChannel", new JSONArray().put(options.toString())));
                assertEquals("low", created.getString("importance"));
                assertEquals("", created.getString("description"));
                options.put("importance", "high");
                JSONObject existing = value(call(scenario, activity, "android-notifications-capability",
                        path + "ensureChannel", new JSONArray().put(options.toString())));
                assertEquals("low", existing.getString("importance"));
                assertEquals("null", call(scenario, activity, "android-notifications-capability",
                        path + "getChannel", new JSONArray().put("missing-channel")).getString("value"));
            } else {
                assertFalse(call(scenario, activity, "android-notifications-capability", path + "getChannel",
                        new JSONArray().put("general")).getBoolean("ok"));
            }
        }
    }

    @Test
    public void rejectsUnknownCategoriesMalformedPostsAndOtherCapabilities() throws Exception {
        try (ActivityScenario<MuonActivity> scenario = ActivityScenario.launch(MuonActivity.class)) {
            AtomicReference<MuonActivity> reference = new AtomicReference<>();
            scenario.onActivity(reference::set);
            MuonActivity activity = reference.get();
            assertTrue(activity.awaitPageReadyForTest(30, TimeUnit.SECONDS));
            for (String options : new String[] {
                    "{\"id\":\"x\",\"title\":\"Title\",\"body\":\"\",\"category\":\"missing\"}",
                    "{\"id\":\"x\",\"title\":\"Title\",\"body\":\"\",\"actions\":[]}",
                    "{\"id\":\"\",\"title\":\"Title\",\"body\":\"\"}" }) {
                assertFalse(options, call(scenario, activity, "notifications-capability", "muon.notifications.post",
                        new JSONArray().put(options)).getBoolean("ok"));
            }
            assertFalse(call(scenario, activity, "notifications-capability", "muon.notifications.getStatus",
                    new JSONArray().put("{\"category\":\"missing\"}")).getBoolean("ok"));
            assertFalse(call(scenario, activity, "android-notifications-capability", "muon.notifications.getCapabilities",
                    new JSONArray()).getBoolean("ok"));
        }
    }

    @Test
    public void enforcesPublishedChannelStringLimits() throws Exception {
        if (Build.VERSION.SDK_INT < 26) { return; }
        try (ActivityScenario<MuonActivity> scenario = ActivityScenario.launch(MuonActivity.class)) {
            AtomicReference<MuonActivity> reference = new AtomicReference<>();
            scenario.onActivity(reference::set);
            MuonActivity activity = reference.get();
            assertTrue(activity.awaitPageReadyForTest(30, TimeUnit.SECONDS));
            for (String field : new String[] { "name", "description" }) {
                JSONObject options = new JSONObject().put("id", "long-" + field + System.nanoTime())
                        .put("name", "Valid").put("importance", "low");
                options.put(field, new String(new char[field.equals("name") ? 129 : 1025]).replace('\0', 'x'));
                assertFalse(field + " must respect the public string limit",
                        call(scenario, activity, "android-notifications-capability",
                                "muon.android.notifications.ensureChannel", new JSONArray().put(options.toString())).getBoolean("ok"));
            }
        }
    }

    @Test
    public void boundsJsonDataBeforeAttemptingOsDelivery() throws Exception {
        try (ActivityScenario<MuonActivity> scenario = ActivityScenario.launch(MuonActivity.class)) {
            AtomicReference<MuonActivity> reference = new AtomicReference<>();
            scenario.onActivity(reference::set);
            MuonActivity activity = reference.get();
            assertTrue(activity.awaitPageReadyForTest(30, TimeUnit.SECONDS));
            String base = "{\"id\":\"data-boundary\",\"title\":\"Title\",\"body\":\"\",\"data\":";
            String[] invalid = { base + JSONObject.quote(new String(new char[5500]).replace('\0', '界')) + "}",
                    base + new String(new char[65]).replace('\0', '[') + "null" + new String(new char[65]).replace('\0', ']') + "}" };
            for (String options : invalid) {
                JSONObject result = call(scenario, activity, "notifications-capability", "muon.notifications.post",
                        new JSONArray().put(options));
                assertFalse(result.getBoolean("ok"));
                assertTrue(result.toString(), result.getString("error").contains("Notification data exceeds"));
            }
        }
    }

    @Test
    public void rejectsIdentifiersContainingOnlyUnicodeWhitespace() throws Exception {
        try (ActivityScenario<MuonActivity> scenario = ActivityScenario.launch(MuonActivity.class)) {
            AtomicReference<MuonActivity> reference = new AtomicReference<>();
            scenario.onActivity(reference::set);
            MuonActivity activity = reference.get();
            assertTrue(activity.awaitPageReadyForTest(30, TimeUnit.SECONDS));
            JSONObject post = new JSONObject().put("id", "\u00a0").put("title", "Title").put("body", "");
            JSONObject result = call(scenario, activity, "notifications-capability", "muon.notifications.post",
                    new JSONArray().put(post.toString()));
            assertFalse(result.getBoolean("ok"));
            assertTrue(result.toString(), result.getString("error").contains("Invalid notification id"));
            JSONObject target = new JSONObject().put("target", "notification-channel").put("channelId", "\u00a0");
            assertFalse(call(scenario, activity, "settings-capability", "muon.android.settings.isAvailable",
                    new JSONArray().put(target.toString())).getBoolean("ok"));
        }
    }

    private static JSONObject value(JSONObject result) throws Exception {
        assertTrue(result.toString(), result.getBoolean("ok"));
        return new JSONObject(result.getString("value"));
    }

    private static JSONObject call(ActivityScenario<MuonActivity> scenario, MuonActivity activity,
            String capability, String path, JSONArray arguments) throws Exception {
        activity.clearTestMessages();
        scenario.onActivity(current -> current.getWebViewForTest().evaluateJavascript(
                "void (async()=>{try{const value=await __muon_plugin_call(" + JSONObject.quote(capability)
                        + "," + JSONObject.quote(path) + "," + arguments + ");"
                        + "muonAndroidTest.postMessage(JSON.stringify({ok:true,value}));}catch(error){"
                        + "muonAndroidTest.postMessage(JSON.stringify({ok:false,error:String(error)}));}})();", null));
        String result = activity.awaitTestMessage(30, TimeUnit.SECONDS);
        assertNotNull(path + " must settle", result);
        return new JSONObject(result);
    }
}

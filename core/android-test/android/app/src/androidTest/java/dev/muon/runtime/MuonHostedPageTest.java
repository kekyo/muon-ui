/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
package dev.muon.runtime;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;
import android.net.Uri;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.Test;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public final class MuonHostedPageTest {
    @Test public void geolocationRequiresAnExplicitLocationPermissionAndTrustedOrigin() throws Exception {
        JSONObject web = new JSONObject().put("locationPermissions",
                new JSONArray().put(android.Manifest.permission.ACCESS_COARSE_LOCATION));
        MuonWebDeployment hosted = new MuonWebDeployment("https://app.example/", web);
        assertEquals(1, hosted.locationPermissions.length);
        assertTrue(hosted.trusted("https://app.example"));
        assertFalse(hosted.trusted("https://location.example"));
        assertFalse(hosted.localNetworkPermission);
        assertEquals(0, new MuonWebDeployment("https://app.example/", null).locationPermissions.length);
        try {
            new MuonWebDeployment("https://app.example/", new JSONObject()
                    .put("locationPermissions", new JSONArray().put(android.Manifest.permission.CAMERA)));
            org.junit.Assert.fail("Camera permission must never answer a geolocation prompt");
        } catch (org.json.JSONException expected) {
            assertTrue(expected.getMessage().contains("location"));
        }
    }

    @Test public void opensTheConfiguredHostedDocument() throws Exception {
        JSONObject config = new JSONObject(MuonAppConfig.readAsset(
                InstrumentationRegistry.getInstrumentation().getTargetContext(),"muon/config.json"));
        String page = "https://app.example:8443/main?view=open";
        config.put("startPage",page);
        config.put("web",new JSONObject().put("resourceOrigins",new JSONArray().put("https://static.example"))
                .put("loopbackResourcePaths",new JSONArray().put("/media/")));
        MuonAppConfig application = new MuonAppConfig(config);
        assertEquals(page,application.startPage);
        assertTrue(application.web.trusted("https://app.example:8443/"));
        assertTrue(application.web.allowsNavigation("https://app.example:8443/next"));
        assertFalse(application.web.allowsNavigation("https://static.example/page"));
        assertFalse(application.web.trusted("https://static.example"));
        assertTrue(application.web.allowsResource(Uri.parse("https://static.example/image")));
        assertFalse(application.web.allowsResource(Uri.parse("https://undeclared.example/image")));
        assertTrue(application.web.allowsResource(Uri.parse("http://127.0.0.1:32123/media/movie")));
        assertFalse(application.web.allowsResource(Uri.parse("http://127.0.0.1:32123/other/movie")));
    }
}

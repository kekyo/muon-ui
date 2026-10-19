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

import android.graphics.Bitmap;
import android.graphics.Color;
import android.os.SystemClock;
import android.view.Choreographer;
import android.view.InputDevice;
import android.view.MotionEvent;

import androidx.core.view.ViewCompat;
import androidx.core.view.WindowInsetsCompat;
import androidx.test.core.app.ActivityScenario;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;

import org.json.JSONObject;
import org.junit.Test;
import org.junit.runner.RunWith;

import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

/** Exercises the actual OS edge gesture and observes its temporary overlay in rendered frames. */
@RunWith(AndroidJUnit4.class)
public final class MuonTransientBarsTest {
    @Test
    public void preservesFullscreenThroughTransientSwipeBars() throws Exception {
        var automation = InstrumentationRegistry.getInstrumentation().getUiAutomation();
        try (ActivityScenario<MuonActivity> scenario = ActivityScenario.launch(MuonActivity.class)) {
            AtomicReference<MuonActivity> reference = new AtomicReference<>();
            scenario.onActivity(reference::set);
            MuonActivity activity = reference.get();
            assertTrue(activity.awaitPageReadyForTest(30, TimeUnit.SECONDS));
            AtomicReference<int[]> geometry = new AtomicReference<>();
            scenario.onActivity(current -> {
                var decor = current.getWindow().getDecorView();
                var insets = ViewCompat.getRootWindowInsets(decor);
                assertNotNull(insets);
                int bottom = insets.getInsetsIgnoringVisibility(WindowInsetsCompat.Type.navigationBars()).bottom;
                assertTrue("This video test requires bottom navigation", bottom > 0);
                geometry.set(new int[] {decor.getWidth(), decor.getHeight(), bottom});
            });
            activity.clearTestMessages();
            scenario.onActivity(current -> current.getWebViewForTest().evaluateJavascript("""
                    void (async () => {
                      await muon.android.systemBars.configure({contentLayout:'edge-to-edge',
                        statusBar:{visible:false,icons:'light'},navigationBar:{visible:false,icons:'light'}});
                      const strip = document.createElement('div');
                      strip.style.cssText = 'position:fixed;left:0;right:0;bottom:0;height:160px;background:#0000ff;z-index:999998';
                      const marker = document.createElement('div');
                      marker.id = 'transient-state-marker';
                      marker.style.cssText = 'position:fixed;left:calc(50% - 40px);top:120px;width:80px;height:80px;background:#00ff00;z-index:999999';
                      document.body.append(strip,marker);
                      await new Promise(resolve => requestAnimationFrame(resolve));
                      muonAndroidTest.postMessage('ready');
                    })();
                    """, null));
            assertEquals("ready", activity.awaitTestMessage(30, TimeUnit.SECONDS));
            var frames = new ArrayBlockingQueue<Long>(1);
            AtomicReference<Choreographer.FrameCallback> frameCallback = new AtomicReference<>();
            scenario.onActivity(current -> {
                Choreographer.FrameCallback callback = new Choreographer.FrameCallback() {
                    @Override public void doFrame(long time) {
                        frames.offer(time);
                        Choreographer.getInstance().postFrameCallback(this);
                    }
                };
                frameCallback.set(callback);
                Choreographer.getInstance().postFrameCallback(callback);
            });
            try {
                // The test paints the navigation region solid blue. White pixels in that
                // region therefore come from the OS navigation buttons/gesture indicator.
                boolean hidden = false;
                long deadline = SystemClock.uptimeMillis() + 30000;
                while (!hidden && SystemClock.uptimeMillis() < deadline) {
                    assertNotNull(frames.poll(30, TimeUnit.SECONDS));
                    Bitmap bitmap = automation.takeScreenshot();
                    assertNotNull(bitmap);
                    int[] counts = countNavigationPixels(bitmap, geometry.get()[2]);
                    bitmap.recycle();
                    hidden = counts[0] == 0 && counts[1] > counts[2] * 0.9;
                }
                assertTrue("Fullscreen must expose the blue strip before the gesture", hidden);
                for (int frame = 0; frame < 60; frame++) { assertNotNull(frames.poll(30, TimeUnit.SECONDS)); }

                int[] bounds = geometry.get();
                long down = SystemClock.uptimeMillis();
                for (int step = 0; step <= 12; step++) {
                    assertNotNull(frames.poll(30, TimeUnit.SECONDS));
                    int action = step == 0 ? MotionEvent.ACTION_DOWN : step == 12 ? MotionEvent.ACTION_UP : MotionEvent.ACTION_MOVE;
                    float y = bounds[1] - 1 - (bounds[2] * 4.0f * step / 12);
                    MotionEvent event = MotionEvent.obtain(down, SystemClock.uptimeMillis(), action,
                            bounds[0] / 2.0f, y, 0);
                    event.setSource(InputDevice.SOURCE_TOUCHSCREEN);
                    try { assertTrue("OS swipe event must be injected", automation.injectInputEvent(event, true)); }
                    finally { event.recycle(); }
                }

                boolean appeared = false;
                deadline = SystemClock.uptimeMillis() + 30000;
                while (!appeared && SystemClock.uptimeMillis() < deadline) {
                    assertNotNull(frames.poll(30, TimeUnit.SECONDS));
                    Bitmap bitmap = automation.takeScreenshot();
                    assertNotNull(bitmap);
                    int[] counts = countNavigationPixels(bitmap, bounds[2]);
                    bitmap.recycle();
                    appeared = counts[0] >= 25;
                }
                assertTrue("A real edge swipe must reveal OS navigation controls", appeared);
                JSONObject transientState = stateAndMarker(scenario, activity, "#ff00ff");
                assertFalse(transientState.getJSONObject("requested").getJSONObject("statusBar").getBoolean("visible"));
                assertFalse(transientState.getJSONObject("requested").getJSONObject("navigationBar").getBoolean("visible"));
                assertTrue(activity.isFullscreenForTest());
                android.util.Log.i("MuonTransientTest", "Physical navigation overlay visible; state=" + transientState);
                for (int frame = 0; frame < 60; frame++) { assertNotNull(frames.poll(30, TimeUnit.SECONDS)); }

                hidden = false;
                deadline = SystemClock.uptimeMillis() + 30000;
                while (!hidden && SystemClock.uptimeMillis() < deadline) {
                    assertNotNull(frames.poll(30, TimeUnit.SECONDS));
                    Bitmap bitmap = automation.takeScreenshot();
                    assertNotNull(bitmap);
                    int[] counts = countNavigationPixels(bitmap, bounds[2]);
                    bitmap.recycle();
                    hidden = counts[0] == 0 && counts[1] > counts[2] * 0.9;
                }
                assertTrue("The OS must hide the transient overlay without another API request", hidden);
                JSONObject settled = stateAndMarker(scenario, activity, "#00ff00");
                assertFalse(settled.getJSONObject("requested").getJSONObject("statusBar").getBoolean("visible"));
                assertFalse(settled.getJSONObject("requested").getJSONObject("navigationBar").getBoolean("visible"));
                assertTrue(activity.isFullscreenForTest());
                android.util.Log.i("MuonTransientTest", "Physical navigation overlay hidden; state=" + settled);
                for (int frame = 0; frame < 60; frame++) { assertNotNull(frames.poll(30, TimeUnit.SECONDS)); }
            } finally {
                scenario.onActivity(current -> Choreographer.getInstance().removeFrameCallback(frameCallback.get()));
            }
        }
    }

    private static int[] countNavigationPixels(Bitmap bitmap, int height) {
        int[] pixels = new int[bitmap.getWidth() * height];
        bitmap.getPixels(pixels, 0, bitmap.getWidth(), 0, bitmap.getHeight() - height, bitmap.getWidth(), height);
        int white = 0;
        int blue = 0;
        for (int pixel : pixels) {
            if (Color.red(pixel) > 210 && Color.green(pixel) > 210 && Color.blue(pixel) > 210) { white++; }
            if (Color.red(pixel) < 25 && Color.green(pixel) < 25 && Color.blue(pixel) > 230) { blue++; }
        }
        return new int[] {white, blue, pixels.length};
    }

    private static JSONObject stateAndMarker(ActivityScenario<MuonActivity> scenario,
            MuonActivity activity, String color) throws Exception {
        activity.clearTestMessages();
        scenario.onActivity(current -> current.getWebViewForTest().evaluateJavascript(
                "void (async()=>{document.querySelector('#transient-state-marker').style.backgroundColor="
                        + JSONObject.quote(color) + ";muonAndroidTest.postMessage(JSON.stringify(await muon.android.systemBars.getState()));})();", null));
        String message = activity.awaitTestMessage(30, TimeUnit.SECONDS);
        assertNotNull(message);
        return new JSONObject(message);
    }
}

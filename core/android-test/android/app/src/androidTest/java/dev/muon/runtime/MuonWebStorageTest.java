/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
package dev.muon.runtime;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

import androidx.test.core.app.ActivityScenario;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.Test;
import org.junit.runner.RunWith;

/** Check browser storage across the lifetime of a bundled application's page. */
@RunWith(AndroidJUnit4.class)
public final class MuonWebStorageTest {
    private static String evaluate(ActivityScenario<MuonActivity> scenario, String script) throws Exception {
        AtomicReference<MuonActivity> activity = new AtomicReference<>();
        scenario.onActivity(activity::set);
        assertTrue(activity.get().awaitPageReadyForTest(30, TimeUnit.SECONDS));
        CompletableFuture<String> result = new CompletableFuture<>();
        scenario.onActivity(view -> view.getWebViewForTest().evaluateJavascript(script, result::complete));
        return result.get(30, TimeUnit.SECONDS);
    }

    @Test public void readsPageHistoryAfterActivityRecreation() throws Exception {
        try (ActivityScenario<MuonActivity> scenario = ActivityScenario.launch(MuonActivity.class)) {
            assertEquals("true", evaluate(scenario,
                    "(() => { localStorage.setItem('muon.storage-test', 'record'); return true; })()"));
            scenario.recreate();
            assertEquals("\"record\"", evaluate(scenario,
                    "(() => { const v = localStorage.getItem('muon.storage-test'); localStorage.removeItem('muon.storage-test'); return v; })()"));
        }
    }
}

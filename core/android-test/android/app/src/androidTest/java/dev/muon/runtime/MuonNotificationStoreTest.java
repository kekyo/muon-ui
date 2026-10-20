/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
package dev.muon.runtime;

import static org.junit.Assert.*;
import android.content.Context;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import org.json.JSONObject;
import org.junit.Test;
import org.junit.runner.RunWith;
import java.util.UUID;
import java.util.concurrent.atomic.AtomicLong;

/** Verifies durable event behavior, bounded capacity and transaction failures. */
@RunWith(AndroidJUnit4.class)
public final class MuonNotificationStoreTest {
    @Test public void preservesPendingEventsAcrossReopenAndAcknowledgesDuplicates() throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        String name = "event-store-" + UUID.randomUUID() + ".db";
        String token;
        try {
            try (MuonNotificationStore store = new MuonNotificationStore(context, name, System::currentTimeMillis)) {
                token = store.post("first", "{\"screen\":\"inbox\"}", "[]", value -> {});
                assertTrue(store.activate(token, "activate", null, id -> assertEquals("first", id)));
                assertFalse(store.activate(token, "activate", null, id -> fail("Duplicate must not cancel again")));
            }
            try (MuonNotificationStore store = new MuonNotificationStore(context, name, System::currentTimeMillis)) {
                JSONObject event = store.snapshot(-1).getJSONArray("events").getJSONObject(0);
                assertEquals(token, event.getString("eventId"));
                assertEquals("inbox", event.getJSONObject("data").getString("screen"));
                assertTrue(event.isNull("actionId"));
                store.acknowledge(token);
                store.acknowledge(token);
                store.acknowledge("unknown");
                assertFalse(store.activate(token, "activate", null, id -> fail("ACK must suppress duplicates")));
                assertEquals(0, store.snapshot(-1).getJSONArray("events").length());
            }
            try (MuonNotificationStore store = new MuonNotificationStore(context, name, System::currentTimeMillis)) {
                assertFalse(store.activate(token, "activate", null, id -> fail("Reopened ACK must suppress duplicates")));
                assertEquals(0, store.snapshot(-1).getJSONArray("events").length());
            }
        } finally { context.deleteDatabase(name); }
    }

    @Test public void invalidatesOldRevisionsAndPreservesPendingEventsAcrossUpdateAndCancel() throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        String name = "event-store-" + UUID.randomUUID() + ".db";
        try (MuonNotificationStore store = new MuonNotificationStore(context, name, System::currentTimeMillis)) {
            String old = store.post("same", "1", "[]", value -> {});
            String current = store.post("same", "2", "[{\"id\":\"open\",\"title\":\"Open\"}]", value -> {});
            assertFalse(store.activate(old, "activate", null, id -> fail("An expired revision must be ignored")));
            assertFalse(store.activate(current, "action", "forged", id -> fail("Unknown action must be ignored")));
            assertTrue(store.activate(current, "action", "open", id -> {}));
            long cursor = store.snapshot(-1).getLong("cursor");
            String updated = store.post("same", "3", "[]", value -> {});
            store.cancel("same", () -> {});
            assertFalse(store.activate(updated, "activate", null, id -> fail("Cancelled revision must be ignored")));
            JSONObject snapshot = store.snapshot(-1);
            assertEquals(1, snapshot.getJSONArray("events").length());
            assertEquals("open", snapshot.getJSONArray("events").getJSONObject(0).getString("actionId"));
            assertEquals(2, snapshot.getJSONArray("events").getJSONObject(0).getInt("data"));
            assertEquals(0, store.snapshot(cursor).getJSONArray("events").length());
        } finally { context.deleteDatabase(name); }
    }

    @Test public void reservesCapacityBeforePostingAndReusesOnlyUnoperatedSlots() throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        String name = "event-store-" + UUID.randomUUID() + ".db";
        try (MuonNotificationStore store = new MuonNotificationStore(context, name, System::currentTimeMillis)) {
            for (int i = 0; i < 256; i++) { store.post("n" + i, "null", "[]", token -> {}); }
            assertThrows(IllegalStateException.class, () -> store.post("overflow", "null", "[]", token -> fail("Full stores must not post")));
            String token = store.post("n0", "null", "[]", value -> {});
            assertTrue(store.activate(token, "activate", null, id -> {}));
            assertThrows(IllegalStateException.class, () -> store.post("n0", "null", "[]", value -> fail("Pending events retain capacity")));
            store.acknowledge(token);
            store.post("n0", "null", "[]", value -> {});
        } finally { context.deleteDatabase(name); }
    }

    @Test public void expiresUnoperatedAndPendingRecordsFromTheirOwnTimestamps() throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        String name = "event-store-" + UUID.randomUUID() + ".db";
        AtomicLong clock = new AtomicLong(1000000000L);
        long day = 24L * 60 * 60 * 1000;
        try (MuonNotificationStore store = new MuonNotificationStore(context, name, clock::get)) {
            String expired = store.post("expires", "null", "[]", value -> {});
            String active = store.post("pending", "null", "[]", value -> {});
            clock.addAndGet(29 * day);
            assertTrue(store.activate(active, "activate", null, id -> {}));
            clock.addAndGet(2 * day);
            assertFalse(store.activate(expired, "activate", null, id -> fail("Expired token must be ignored")));
            assertEquals(1, store.snapshot(-1).getJSONArray("events").length());
            clock.addAndGet(29 * day);
            assertEquals(0, store.snapshot(-1).getJSONArray("events").length());
            assertFalse(store.activate(active, "activate", null, id -> fail("Expired event must not be recreated")));
        } finally { context.deleteDatabase(name); }
    }

    @Test public void rollsBackKnownPostFailuresWithoutLosingThePreviousRevision() throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        String name = "event-store-" + UUID.randomUUID() + ".db";
        try (MuonNotificationStore store = new MuonNotificationStore(context, name, System::currentTimeMillis)) {
            String old = store.post("same", "1", "[]", value -> {});
            assertThrows(IllegalStateException.class, () -> store.post("same", "2", "[]", value -> {
                throw new IllegalStateException("OS submission failed");
            }));
            assertTrue(store.activate(old, "activate", null, id -> {}));
            assertEquals(1, store.snapshot(-1).getJSONArray("events").getJSONObject(0).getInt("data"));
        } finally { context.deleteDatabase(name); }
    }

    @Test public void boundsAcknowledgementRecordsAndExpiresThemAtThirtyDays() throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        String name = "event-store-" + UUID.randomUUID() + ".db";
        AtomicLong clock = new AtomicLong(1000000000L);
        String first = null;
        try {
            try (MuonNotificationStore store = new MuonNotificationStore(context, name, clock::get)) {
                for (int i = 0; i < 1025; i++) {
                    String token = store.post("same", "null", "[]", value -> {});
                    if (first == null) { first = token; }
                    assertTrue(store.activate(token, "activate", null, id -> {}));
                    store.acknowledge(token);
                    clock.incrementAndGet();
                }
            }
            // This is a retained-record limit, so inspect the committed store rather than timing or file allocation.
            try (var db = context.openOrCreateDatabase(name, Context.MODE_PRIVATE, null);
                    var rows = db.rawQuery("SELECT COUNT(*) FROM acknowledged", null)) {
                assertTrue(rows.moveToFirst());
                assertEquals(1024, rows.getInt(0));
            }
            try (MuonNotificationStore store = new MuonNotificationStore(context, name, clock::get)) {
                assertFalse(store.activate(first, "activate", null, id -> fail("Evicted ACK must never regenerate an event")));
                clock.addAndGet(30L * 24 * 60 * 60 * 1000);
                assertEquals(0, store.snapshot(-1).getJSONArray("events").length());
                assertFalse(store.activate(first, "activate", null, id -> fail("Expired ACK must never regenerate an event")));
            }
            try (var db = context.openOrCreateDatabase(name, Context.MODE_PRIVATE, null);
                    var rows = db.rawQuery("SELECT COUNT(*) FROM acknowledged", null)) {
                assertTrue(rows.moveToFirst());
                assertEquals(0, rows.getInt(0));
            }
        } finally { context.deleteDatabase(name); }
    }

    @Test public void failsBeforeOsSubmissionWhenTheDatabaseCannotBeOpened() throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        // An existing directory cannot be opened as a database file.
        try (MuonNotificationStore store = new MuonNotificationStore(context,
                context.getCacheDir().getAbsolutePath(), System::currentTimeMillis)) {
            assertThrows(android.database.sqlite.SQLiteException.class,
                    () -> store.post("fails", "null", "[]", token -> fail("Failed persistence must not submit")));
        }
    }

    @Test public void preservesCorruptFilesInsteadOfResettingUnacknowledgedData() throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        String name = "event-store-" + UUID.randomUUID() + ".db";
        byte[] corrupt = new byte[4096];
        java.util.Arrays.fill(corrupt, (byte) 0x41);
        try {
            try (var stream = new java.io.FileOutputStream(context.getDatabasePath(name))) { stream.write(corrupt); }
            try (MuonNotificationStore store = new MuonNotificationStore(context, name, System::currentTimeMillis)) {
                assertThrows(android.database.sqlite.SQLiteException.class, () -> store.snapshot(-1));
            }
            assertEquals(corrupt.length, context.getDatabasePath(name).length());
            try (var stream = new java.io.FileInputStream(context.getDatabasePath(name))) { assertEquals(0x41, stream.read()); }
        } finally { context.deleteDatabase(name); }
    }

    @Test public void retainsTheOperationWhenOsNotificationRemovalFails() throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        String name = "event-store-" + UUID.randomUUID() + ".db";
        try (MuonNotificationStore store = new MuonNotificationStore(context, name, System::currentTimeMillis)) {
            String token = store.post("retained", "{\"screen\":\"inbox\"}", "[]", value -> {});
            assertThrows(IllegalStateException.class, () -> store.activate(token, "activate", null, id -> {
                throw new IllegalStateException("OS removal failed");
            }));
            assertEquals(1, store.snapshot(-1).getJSONArray("events").length());
            assertEquals(token, store.snapshot(-1).getJSONArray("events").getJSONObject(0).getString("eventId"));
            assertFalse(store.activate(token, "activate", null, id -> fail("First operation must remain consumed")));
        } finally { context.deleteDatabase(name); }
    }

    @Test public void keepsTheRevisionWhenEventPersistenceFailsBeforeRemoval() throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        String name = "event-store-" + UUID.randomUUID() + ".db";
        try {
            String token;
            try (MuonNotificationStore store = new MuonNotificationStore(context, name, System::currentTimeMillis)) {
                token = store.post("persist-failure", "null", "[]", value -> {});
            }
            // Inject a real SQLite write failure without exhausting the device's shared disk.
            try (var db = context.openOrCreateDatabase(name, Context.MODE_PRIVATE, null)) {
                db.execSQL("CREATE TRIGGER fail_event BEFORE INSERT ON events BEGIN SELECT RAISE(ABORT,'event persistence failed'); END");
            }
            try (MuonNotificationStore store = new MuonNotificationStore(context, name, System::currentTimeMillis)) {
                assertThrows(android.database.sqlite.SQLiteException.class,
                        () -> store.activate(token, "activate", null, id -> fail("Unsaved operations must not remove notifications")));
                assertEquals(0, store.snapshot(-1).getJSONArray("events").length());
            }
            try (var db = context.openOrCreateDatabase(name, Context.MODE_PRIVATE, null)) { db.execSQL("DROP TRIGGER fail_event"); }
            try (MuonNotificationStore store = new MuonNotificationStore(context, name, System::currentTimeMillis)) {
                assertTrue(store.activate(token, "activate", null, id -> {}));
                assertEquals(token, store.snapshot(-1).getJSONArray("events").getJSONObject(0).getString("eventId"));
            }
        } finally { context.deleteDatabase(name); }
    }
}

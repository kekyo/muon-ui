/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
package dev.muon.runtime;

import android.content.ContentValues;
import android.content.Context;
import android.database.sqlite.SQLiteDatabase;
import android.database.sqlite.SQLiteDatabaseCorruptException;
import androidx.annotation.Nullable;
import org.json.JSONArray;
import org.json.JSONObject;
import org.json.JSONTokener;
import java.util.UUID;
import java.util.function.Consumer;
import java.util.function.LongSupplier;

/** Owns durable revisions and events; all access belongs to one application I/O executor. */
final class MuonNotificationStore implements AutoCloseable {
    private static final long RETENTION_MILLIS = 30L * 24 * 60 * 60 * 1000;
    private final Context context;
    private final String name;
    private final LongSupplier clock;
    @Nullable private SQLiteDatabase database;

    MuonNotificationStore(Context context, String name, LongSupplier clock) {
        this.context = context.getApplicationContext();
        this.name = name;
        this.clock = clock;
    }

    private SQLiteDatabase database() {
        if (database != null) { return database; }
        SQLiteDatabase opened = context.openOrCreateDatabase(name, Context.MODE_PRIVATE, null, corrupt -> {
            // The default handler deletes corrupt databases. Preserve unacknowledged data for diagnosis.
            throw new SQLiteDatabaseCorruptException("The notification event database is corrupt");
        });
        try {
            if (opened.getVersion() == 0) {
                opened.beginTransaction();
                try {
                    opened.execSQL("CREATE TABLE posted (token TEXT PRIMARY KEY, id TEXT UNIQUE NOT NULL, data TEXT NOT NULL, actions TEXT NOT NULL, posted_at INTEGER NOT NULL)");
                    opened.execSQL("CREATE TABLE events (token TEXT PRIMARY KEY, id TEXT NOT NULL, data TEXT NOT NULL, type TEXT NOT NULL, action_id TEXT, received_at INTEGER NOT NULL, cursor INTEGER UNIQUE NOT NULL)");
                    opened.execSQL("CREATE TABLE acknowledged (token TEXT PRIMARY KEY, acknowledged_at INTEGER NOT NULL)");
                    opened.execSQL("CREATE TABLE sequence (id INTEGER PRIMARY KEY CHECK(id=1), cursor INTEGER NOT NULL)");
                    ContentValues initial = new ContentValues();
                    initial.put("id", 1);
                    initial.put("cursor", 0);
                    opened.insertOrThrow("sequence", null, initial);
                    opened.setVersion(1);
                    opened.setTransactionSuccessful();
                } finally { opened.endTransaction(); }
            } else if (opened.getVersion() != 1) {
                throw new IllegalStateException("Unsupported notification event database version");
            }
            database = opened;
            return opened;
        } catch (RuntimeException error) {
            opened.close();
            throw error;
        }
    }

    String post(String id, String data, String actions, Consumer<String> submit) throws Exception {
        SQLiteDatabase db = database();
        String token = UUID.randomUUID().toString();
        db.beginTransaction();
        try {
            prune(db);
            // Replacing an unoperated revision reuses its slot; pending events retain theirs.
            db.delete("posted", "id=?", new String[] { id });
            if (scalar(db, "SELECT (SELECT COUNT(*) FROM posted)+(SELECT COUNT(*) FROM events)") >= 256) {
                throw new IllegalStateException("Notification storage capacity of 256 revisions and pending events is exhausted");
            }
            ContentValues values = new ContentValues();
            values.put("token", token);
            values.put("id", id);
            values.put("data", data);
            values.put("actions", actions);
            values.put("posted_at", clock.getAsLong());
            db.insertOrThrow("posted", null, values);
            // The OS operation shares this execution order, but is not part of the SQLite transaction.
            submit.accept(token);
            db.setTransactionSuccessful();
        } finally { db.endTransaction(); }
        return token;
    }

    void cancel(String id, Runnable cancel) {
        SQLiteDatabase db = database();
        db.beginTransaction();
        try {
            prune(db);
            db.delete("posted", "id=?", new String[] { id });
            cancel.run();
            db.setTransactionSuccessful();
        } finally { db.endTransaction(); }
    }

    boolean activate(String token, String type, @Nullable String actionId, Consumer<String> cancel) throws Exception {
        SQLiteDatabase db = database();
        String notificationId = null;
        db.beginTransaction();
        try {
            prune(db);
            try (var row = db.rawQuery("SELECT id,data,actions FROM posted WHERE token=?", new String[] { token })) {
                if (row.moveToFirst()) {
                    boolean valid = type.equals("activate") && actionId == null;
                    if (type.equals("action") && actionId != null) {
                        JSONArray actions = new JSONArray(row.getString(2));
                        for (int i = 0; i < actions.length(); i++) {
                            if (actionId.equals(actions.getJSONObject(i).getString("id"))) { valid = true; break; }
                        }
                    }
                    if (valid) {
                        long cursor = scalar(db, "SELECT cursor FROM sequence WHERE id=1") + 1;
                        if (cursor > 9007199254740991L) { throw new IllegalStateException("Notification event cursor is exhausted"); }
                        ContentValues sequence = new ContentValues();
                        sequence.put("cursor", cursor);
                        db.update("sequence", sequence, "id=1", null);
                        ContentValues event = new ContentValues();
                        event.put("token", token);
                        event.put("id", row.getString(0));
                        event.put("data", row.getString(1));
                        event.put("type", type);
                        event.put("action_id", actionId);
                        event.put("received_at", clock.getAsLong());
                        event.put("cursor", cursor);
                        db.insertOrThrow("events", null, event);
                        db.delete("posted", "token=?", new String[] { token });
                        notificationId = row.getString(0);
                    }
                }
                // Unknown, expired, cancelled and already consumed tokens never recreate events.
            }
            db.setTransactionSuccessful();
        } finally { db.endTransaction(); }
        // Keep the source notification available when persistence fails. Once committed,
        // a removal failure must not roll back the operation or allow it to be recreated.
        if (notificationId != null) { cancel.accept(notificationId); }
        return notificationId != null;
    }

    JSONObject snapshot(long afterCursor) throws Exception {
        SQLiteDatabase db = database();
        db.beginTransaction();
        try {
            prune(db);
            long cursor = scalar(db, "SELECT cursor FROM sequence WHERE id=1");
            if (afterCursor < -1 || afterCursor > cursor) {
                throw new IllegalArgumentException("Notification cursor must be an observed sequence");
            }
            JSONArray events = new JSONArray();
            try (var rows = db.rawQuery("SELECT token,id,type,action_id,data FROM events WHERE cursor>? ORDER BY cursor",
                    new String[] { Long.toString(afterCursor) })) {
                while (rows.moveToNext()) {
                    events.put(new JSONObject().put("eventId", rows.getString(0)).put("id", rows.getString(1))
                            .put("type", rows.getString(2)).put("actionId", rows.isNull(3) ? JSONObject.NULL : rows.getString(3))
                            .put("data", new JSONTokener(rows.getString(4)).nextValue()));
                }
            }
            JSONObject result = new JSONObject().put("events", events).put("cursor", cursor);
            db.setTransactionSuccessful();
            return result;
        } finally { db.endTransaction(); }
    }

    void acknowledge(String eventId) {
        SQLiteDatabase db = database();
        db.beginTransaction();
        try {
            prune(db);
            if (db.delete("events", "token=?", new String[] { eventId }) != 0) {
                ContentValues values = new ContentValues();
                values.put("token", eventId);
                values.put("acknowledged_at", clock.getAsLong());
                db.insertOrThrow("acknowledged", null, values);
                db.delete("acknowledged", "token IN (SELECT token FROM acknowledged ORDER BY acknowledged_at DESC,rowid DESC LIMIT -1 OFFSET 1024)", null);
            }
            db.setTransactionSuccessful();
        } finally { db.endTransaction(); }
    }

    private void prune(SQLiteDatabase db) {
        String cutoff = Long.toString(clock.getAsLong() - RETENTION_MILLIS);
        db.delete("posted", "posted_at<=?", new String[] { cutoff });
        db.delete("events", "received_at<=?", new String[] { cutoff });
        db.delete("acknowledged", "acknowledged_at<=?", new String[] { cutoff });
    }

    private static long scalar(SQLiteDatabase db, String sql) {
        try (var result = db.rawQuery(sql, null)) {
            if (!result.moveToFirst()) { throw new IllegalStateException("Notification database state is incomplete"); }
            return result.getLong(0);
        }
    }

    @Override public void close() {
        if (database != null) { database.close(); database = null; }
    }
}

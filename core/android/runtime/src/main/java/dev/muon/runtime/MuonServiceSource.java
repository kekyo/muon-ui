/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
package dev.muon.runtime;

import android.content.Context;
import android.os.Handler;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.ForkJoinPool;

/** Loads a bounded, strictly UTF-8 service bundle without blocking its owner. */
final class MuonServiceSource {
    private MuonServiceSource() {}

    static void read(Context context, String asset, Handler owner,
            MuonServicePreferences.Completion<String> complete) {
        try {
            // AssetManager also supports compressed APK assets. Android/cardio
            // has no regular-file async adapter; reuse the JVM's existing pool.
            ForkJoinPool.commonPool().execute(() -> {
                try {
                    String source;
                    try (InputStream input = context.getAssets().open(asset)) {
                        source = decode(input);
                    }
                    owner.post(() -> complete.complete(source, null));
                } catch (Exception error) {
                    owner.post(() -> complete.complete(null, error.toString()));
                }
            });
        } catch (RuntimeException error) {
            owner.post(() -> complete.complete(null, error.toString()));
        }
    }

    /** Decodes caller-owned input; closing it remains the caller's responsibility. */
    static String decode(InputStream input) throws IOException {
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        byte[] buffer = new byte[16384];
        int count;
        while ((count = input.read(buffer)) != -1) {
            if (count > 8 * 1024 * 1024 - output.size())
                throw new IOException("Service source exceeds 8 MiB");
            output.write(buffer, 0, count);
        }
        return StandardCharsets.UTF_8.newDecoder()
                .onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT)
                .decode(ByteBuffer.wrap(output.toByteArray())).toString();
    }
}

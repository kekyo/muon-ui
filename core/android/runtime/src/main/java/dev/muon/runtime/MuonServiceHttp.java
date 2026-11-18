/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 */
package dev.muon.runtime;

import android.security.NetworkSecurityPolicy;
import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.Proxy;
import java.net.SocketTimeoutException;
import java.net.URL;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import javax.net.ssl.SSLException;

/** One HTTP hop running on the service process I/O executor. */
final class MuonServiceHttp {
    private volatile HttpURLConnection connection;
    private volatile boolean cancelled;
    int status;
    String[] headers = new String[0];
    String code, message;

    void cancel() {
        cancelled = true;
        HttpURLConnection active = connection;
        if (active != null) active.disconnect();
    }

    byte[] send(String address, String method, String[] fields, byte[] body, int timeout) {
        try {
            URL url = new URL(address);
            if (url.getProtocol().equals("http") && !NetworkSecurityPolicy.getInstance().isCleartextTrafficPermitted(url.getHost())) {
                code = "permission-denied"; message = "Android cleartext policy denies this host";
                return null;
            }
            HttpURLConnection active = (HttpURLConnection) url.openConnection(Proxy.NO_PROXY);
            connection = active;
            if (cancelled) throw new java.io.InterruptedIOException("HTTP request cancelled");
            active.setInstanceFollowRedirects(false);
            active.setUseCaches(false);
            active.setConnectTimeout(timeout);
            active.setReadTimeout(timeout);
            active.setRequestMethod(method);
            active.setRequestProperty("Accept-Encoding", "identity");
            for (int i = 0; i < fields.length; i += 2) active.setRequestProperty(fields[i], fields[i + 1]);
            if (body.length > 0) {
                active.setDoOutput(true);
                active.setFixedLengthStreamingMode(body.length);
                try (OutputStream output = active.getOutputStream()) { output.write(body); }
            }
            status = active.getResponseCode();
            List<String> pairs = new ArrayList<>();
            int headerSize = 0;
            for (Map.Entry<String, List<String>> entry : active.getHeaderFields().entrySet()) {
                if (entry.getKey() == null) continue;
                for (String value : entry.getValue()) {
                    headerSize += entry.getKey().length() + value.length() + 4;
                    if (headerSize > 65536) { code = "size-limit"; message = "HTTP headers exceed limit"; return null; }
                    pairs.add(entry.getKey()); pairs.add(value);
                }
            }
            headers = pairs.toArray(new String[0]);
            try (InputStream input = status >= 400 ? active.getErrorStream() : active.getInputStream()) {
                if (input == null) return new byte[0];
                ByteArrayOutputStream bytes = new ByteArrayOutputStream();
                byte[] buffer = new byte[16384];
                int n;
                while ((n = input.read(buffer)) != -1) {
                    if (bytes.size() + n > 1024 * 1024) { code = "size-limit"; message = "HTTP body exceeds 1 MiB"; return null; }
                    bytes.write(buffer, 0, n);
                }
                return bytes.toByteArray();
            }
        } catch (Exception error) {
            code = cancelled ? "cancelled" : error instanceof SSLException ? "tls-error" :
                error instanceof SocketTimeoutException ? "timeout" : "network-error";
            message = error.toString();
            return null;
        } finally {
            HttpURLConnection active = connection;
            connection = null;
            if (active != null) active.disconnect();
        }
    }
}

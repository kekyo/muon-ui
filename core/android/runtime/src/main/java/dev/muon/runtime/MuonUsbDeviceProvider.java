/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
package dev.muon.runtime;

import android.content.BroadcastReceiver;
import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.database.Cursor;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbManager;
import android.net.Uri;
import android.os.Binder;
import android.os.Bundle;
import android.os.Process;
import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.core.content.ContextCompat;
import androidx.core.content.IntentCompat;
import androidx.core.os.BundleCompat;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;
import java.util.HashMap;
import java.util.Map;
import java.util.TreeMap;
import java.util.UUID;

/** Application-private registry of USB attachment IDs shared between Android processes. */
public final class MuonUsbDeviceProvider extends ContentProvider {
    private final Map<String, String> idsByName = new HashMap<>();
    private boolean observing;

    @Override public boolean onCreate() { return true; }

    private static Uri uri(Context context) {
        return Uri.parse("content://" + context.getPackageName() + ".muon.usb");
    }

    static String list(Context context) {
        return context.getContentResolver().call(uri(context), "list", null, null).getString("devices");
    }

    static UsbDevice resolve(Context context, String id) {
        Bundle result = context.getContentResolver().call(uri(context), "resolve", id, null);
        UsbDevice device = result == null ? null : BundleCompat.getParcelable(result, "device", UsbDevice.class);
        if (device == null) throw new IllegalStateException("usb_disconnected");
        return device;
    }

    /** Open a previously authorized USB attachment inside the calling process.
     * @param context Application or service context in the process that will own the device.
     * @param connectionId Opaque identifier from the permission or enumeration UI.
     * @return Open connection. Its owner must keep it alive until capture ends and close it afterwards.
     * @throws IllegalStateException If the attachment expired or Android could not open it.
     * @throws SecurityException If the application's USB grant has expired.
     */
    @NonNull public static UsbDeviceConnection open(@NonNull Context context, @NonNull String connectionId) {
        UsbManager manager = context.getSystemService(UsbManager.class);
        UsbDevice device = resolve(context, connectionId);
        if (!manager.hasPermission(device)) throw new SecurityException("usb_permission_required");
        UsbDeviceConnection connection = manager.openDevice(device);
        if (connection == null) throw new IllegalStateException("usb_open_failed");
        // Detach/re-attach can race openDevice. Reject a connection if its generation changed.
        try {
            resolve(context, connectionId);
            return connection;
        } catch (RuntimeException error) {
            connection.close();
            throw error;
        }
    }

    private void observe(Context context) {
        if (observing) return;
        ContextCompat.registerReceiver(context, new BroadcastReceiver() {
            @Override public void onReceive(Context receiverContext, Intent intent) {
                UsbDevice device = IntentCompat.getParcelableExtra(intent, UsbManager.EXTRA_DEVICE, UsbDevice.class);
                if (device != null && UsbManager.ACTION_USB_DEVICE_DETACHED.equals(intent.getAction())) {
                    synchronized (MuonUsbDeviceProvider.this) { idsByName.remove(device.getDeviceName()); }
                }
            }
        }, new IntentFilter(UsbManager.ACTION_USB_DEVICE_DETACHED), ContextCompat.RECEIVER_NOT_EXPORTED);
        observing = true;
    }

    @Override public synchronized Bundle call(@NonNull String method, @Nullable String id, @Nullable Bundle extras) {
        if (Binder.getCallingUid() != Process.myUid()) throw new SecurityException("USB registry is private");
        Context context = getContext();
        UsbManager manager = context.getSystemService(UsbManager.class);
        observe(context);
        Map<String, UsbDevice> attached = manager == null ? new TreeMap<>() : new TreeMap<>(manager.getDeviceList());
        idsByName.keySet().retainAll(attached.keySet());
        Bundle result = new Bundle();
        if ("resolve".equals(method)) {
            for (Map.Entry<String, String> entry : idsByName.entrySet()) {
                if (entry.getValue().equals(id)) {
                    result.putParcelable("device", attached.get(entry.getKey()));
                    break;
                }
            }
            return result;
        }
        if (!"list".equals(method)) throw new IllegalArgumentException("Unknown USB operation");
        JSONArray devices = new JSONArray();
        try {
            for (Map.Entry<String, UsbDevice> entry : attached.entrySet()) {
                UsbDevice device = entry.getValue();
                String attachment = idsByName.computeIfAbsent(entry.getKey(), name -> UUID.randomUUID().toString());
                boolean permitted = manager.hasPermission(device);
                String serial = null;
                if (permitted) {
                    try { serial = device.getSerialNumber(); }
                    catch (SecurityException denied) { permitted = false; }
                }
                JSONArray interfaces = new JSONArray();
                for (int i = 0; i < device.getInterfaceCount(); i++) interfaces.put(device.getInterface(i).getInterfaceClass());
                devices.put(new JSONObject().put("connectionId", attachment).put("vendorId", device.getVendorId())
                        .put("productId", device.getProductId()).put("deviceClass", device.getDeviceClass())
                        .put("productName", JSONObject.wrap(device.getProductName()))
                        .put("serialNumber", JSONObject.wrap(serial)).put("interfaceClasses", interfaces).put("permitted", permitted));
            }
        } catch (JSONException e) { throw new IllegalStateException(e); }
        result.putString("devices", devices.toString());
        return result;
    }

    @Override public Cursor query(@NonNull Uri uri, String[] projection, String selection, String[] args, String order) {
        throw new UnsupportedOperationException();
    }
    @Override public String getType(@NonNull Uri uri) { return null; }
    @Override public Uri insert(@NonNull Uri uri, ContentValues values) { throw new UnsupportedOperationException(); }
    @Override public int update(@NonNull Uri uri, ContentValues values, String selection, String[] args) { throw new UnsupportedOperationException(); }
    @Override public int delete(@NonNull Uri uri, String selection, String[] args) { throw new UnsupportedOperationException(); }
}

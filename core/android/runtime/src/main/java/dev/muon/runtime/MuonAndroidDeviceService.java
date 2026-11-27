/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
package dev.muon.runtime;

import android.app.Activity;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageManager;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbManager;
import androidx.core.content.ContextCompat;
import androidx.core.content.IntentCompat;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;
import java.util.Arrays;
import java.util.HashSet;
import java.util.LinkedHashSet;
import java.util.Set;
import java.util.UUID;

/** Page-owned OS permission prompts. Listing devices never creates a recording service. */
final class MuonAndroidDeviceService implements AutoCloseable {
    private final Activity activity;
    private boolean resumed;
    private static int nextCode = 0x4d00;
    private int code, permissionCall, usbCall;
    private String[] requested = {};
    private MuonAndroidPlatformService.Completion permissionReply, usbReply;
    private BroadcastReceiver usbReceiver;
    private PendingIntent usbIntent;

    MuonAndroidDeviceService(Activity activity) { this.activity = activity; }
    void setResumed(boolean value) { resumed = value; }

    // Codes belong to the process so a late result cannot settle a recreated Activity's request.
    static int allocatePermissionCode() {
        if (nextCode == Integer.MAX_VALUE) throw new IllegalStateException("Permission request identifiers exhausted");
        return nextCode++;
    }

    private void requireForeground() {
        if (!resumed || activity.isFinishing() || activity.isDestroyed())
            throw new IllegalStateException("Permission requests require a foreground Activity");
    }

    private JSONObject grants(String[] names) throws JSONException {
        JSONObject result = new JSONObject();
        for (String name : names) result.put(name, activity.checkSelfPermission(name) == PackageManager.PERMISSION_GRANTED);
        return result;
    }

    private String[] names(String encoded) throws JSONException, PackageManager.NameNotFoundException {
        JSONArray values = new JSONArray(encoded);
        if (values.length() == 0) throw new IllegalArgumentException("Permission names are required");
        String[] declared = activity.getPackageManager().getPackageInfo(activity.getPackageName(), PackageManager.GET_PERMISSIONS).requestedPermissions;
        Set<String> manifest = declared == null ? new HashSet<>() : new HashSet<>(Arrays.asList(declared));
        Set<String> names = new LinkedHashSet<>();
        for (int i = 0; i < values.length(); i++) {
            Object name = values.get(i);
            if (!(name instanceof String) || !manifest.contains(name)) throw new IllegalArgumentException("Permission is not declared: " + name);
            names.add((String) name);
        }
        return names.toArray(new String[0]);
    }

    void invoke(int call, String path, JSONArray args, byte[][] attachments, MuonAndroidPlatformService.Completion reply) {
        try {
            if (attachments.length != 0) throw new IllegalArgumentException("Unexpected binary argument");
            if (path.equals("muon.android.usb.listDevices")) {
                if (args.length() != 0) throw new IllegalArgumentException("Expected no arguments");
                reply.completeString(MuonUsbDeviceProvider.list(activity));
                return;
            }
            if (args.length() != 1 || !(args.get(0) instanceof String))
                throw new IllegalArgumentException("Expected one string argument");
            String argument = args.getString(0);
            if (path.equals("muon.android.usb.requestPermission")) {
                requestUsb(call, argument, reply);
                return;
            }
            String[] names = names(argument);
            JSONObject current = grants(names);
            if (path.equals("muon.android.permissions.getState")) {
                reply.completeString(current.toString());
                return;
            }
            if (!path.equals("muon.android.permissions.request")) throw new IllegalArgumentException("Unknown Android permission operation");
            requireForeground();
            if (code != 0 || usbReceiver != null) throw new IllegalStateException("A permission request is pending");
            boolean allGranted = true;
            for (String name : names) if (!current.getBoolean(name)) allGranted = false;
            if (allGranted) { reply.completeString(current.toString()); return; }
            code = allocatePermissionCode();
            permissionCall = call;
            requested = names;
            permissionReply = reply;
            try { activity.requestPermissions(names, code); }
            catch (RuntimeException error) { code = 0; permissionReply = null; throw error; }
        } catch (RuntimeException | JSONException | PackageManager.NameNotFoundException error) { reply.fail(error.toString()); }
    }

    boolean onRequestPermissionsResult(int requestCode, String[] names, int[] results) {
        if (code == 0 || code != requestCode) return false;
        code = 0;
        MuonAndroidPlatformService.Completion reply = permissionReply;
        permissionReply = null;
        if (reply != null) {
            if (results.length == 0 || names.length == 0) reply.fail("permission_request_interrupted");
            else try { reply.completeString(grants(requested).toString()); }
            catch (JSONException | RuntimeException error) { reply.fail(error.toString()); }
        }
        requested = new String[0];
        return true;
    }

    private void requestUsb(int call, String id, MuonAndroidPlatformService.Completion reply) {
        requireForeground();
        if (code != 0 || usbReceiver != null) throw new IllegalStateException("A permission request is pending");
        UsbManager manager = activity.getSystemService(UsbManager.class);
        UsbDevice device = MuonUsbDeviceProvider.resolve(activity, id);
        if (manager.hasPermission(device)) { reply.completeString("true"); return; }
        String action = activity.getPackageName() + ".muon.USB_PERMISSION." + UUID.randomUUID();
        IntentFilter filter = new IntentFilter(action);
        filter.addAction(UsbManager.ACTION_USB_DEVICE_DETACHED);
        BroadcastReceiver receiver = new BroadcastReceiver() {
            @Override public void onReceive(Context context, Intent intent) {
                boolean detached = UsbManager.ACTION_USB_DEVICE_DETACHED.equals(intent.getAction());
                if (detached) {
                    UsbDevice removed = IntentCompat.getParcelableExtra(intent, UsbManager.EXTRA_DEVICE, UsbDevice.class);
                    if (!device.equals(removed)) return;
                } else if (!action.equals(intent.getAction())) return;
                MuonAndroidPlatformService.Completion pending = usbReply;
                clearUsb();
                if (pending == null) return;
                if (detached) { pending.fail("usb_disconnected"); return; }
                try { pending.completeString(Boolean.toString(manager.hasPermission(MuonUsbDeviceProvider.resolve(activity, id)))); }
                catch (RuntimeException error) { pending.fail(error.toString()); }
            }
        };
        ContextCompat.registerReceiver(activity, receiver, filter, ContextCompat.RECEIVER_NOT_EXPORTED);
        usbReceiver = receiver;
        usbCall = call;
        usbReply = reply;
        try {
            // Read the actual OS grant on callback; no mutable fill-in extras are needed.
            usbIntent = PendingIntent.getBroadcast(activity, 0, new Intent(action).setPackage(activity.getPackageName()),
                    PendingIntent.FLAG_ONE_SHOT | PendingIntent.FLAG_IMMUTABLE);
            manager.requestPermission(device, usbIntent);
        } catch (RuntimeException error) { clearUsb(); throw error; }
    }

    private void clearUsb() {
        if (usbReceiver != null) activity.unregisterReceiver(usbReceiver);
        if (usbIntent != null) usbIntent.cancel();
        usbReceiver = null;
        usbIntent = null;
        usbReply = null;
    }

    void cancel(int callId) {
        if (callId == permissionCall) permissionReply = null;
        if (callId == usbCall) clearUsb();
    }
    void cancelAll() { permissionReply = null; clearUsb(); }
    @Override public void close() { cancelAll(); resumed = false; }
}

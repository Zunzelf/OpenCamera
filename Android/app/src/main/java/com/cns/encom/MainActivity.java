package com.cns.encom;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.ActivityNotFoundException;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.ServiceConnection;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.content.res.ColorStateList;
import android.graphics.Matrix;
import android.graphics.SurfaceTexture;
import android.net.Uri;
import android.os.BatteryManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.preference.PreferenceManager;
import android.provider.Settings;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.TextureView;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputMethodManager;
import android.widget.Button;
import android.widget.EditText;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;

import net.majorkernelpanic.streaming.SessionBuilder;
import net.majorkernelpanic.streaming.rtsp.RtspServer;

import java.io.BufferedOutputStream;
import java.io.BufferedReader;
import java.io.DataOutputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.InterfaceAddress;
import java.net.NetworkInterface;
import java.net.Socket;
import java.net.SocketTimeoutException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Random;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Phone side of OpenCamera.
 *
 * Flow: find PCs on the Wi-Fi (UDP probe on port 65501) -> tap one ->
 * TCP hello on 65500 (protocol kept compatible with older PC versions) ->
 * the PC pulls H.264 video from the RTSP server on this phone.
 * The link is retried automatically until the user taps Disconnect.
 *
 * Newer PCs also talk back on the same socket (text lines): "HELLO 2",
 * "SET key=value ..." (pro camera controls) and "POINT x y". Once a PC has
 * said HELLO, the phone sends framed messages to it: 'M', int32 length,
 * UTF-8 text ("CAPS ...", "STAT ...", "EVT ...").
 */
public class MainActivity extends Activity {

    private static final int CONTROL_PORT = 65500;
    private static final int DISCOVERY_PORT = 65501;
    private static final int NAME_MAGIC = 0x4F43414D; // 'OCAM'
    private static final int REQ_CAMERA = 1;

    private enum LinkState { IDLE, CONNECTING, WAITING_PC, LIVE, RECONNECTING, NO_PERMISSION }

    private final Handler ui = new Handler(Looper.getMainLooper());
    private SharedPreferences prefs;

    private TextView statusText, title, subtitle, searching;
    private ImageView statusDot;
    private LinearLayout pcList, pickPanel, livePanel, manualRow;
    private EditText address;
    private Button grant, torch;
    private TextureView preview;
    private TextView previewNote;
    private TextView usbButton;
    private View settingsPanel;
    private LinearLayout settingsList;
    private final CameraEngine camera = CameraEngine.INSTANCE;
    private volatile String lastCaps;
    private int frameW = 1280, frameH = 720;
    private boolean frameFront;
    private boolean cameraRunning;
    private final Runnable dimNow = this::dimScreen;
    private boolean dimmed;

    private LinkState state = LinkState.IDLE;
    private String connectedName = "";
    private int rtspPort;

    // discovery
    /** One computer, with every address it answered from (Wi-Fi, USB cable...). */
    private static class Pc {
        String name;
        final Map<String, Long> seen = new LinkedHashMap<>(); // host -> last reply
        final Map<String, Boolean> usb = new HashMap<>();      // host -> reached over the USB cable
    }
    private static final long FRESH_MS = 5000;
    private final Map<String, Pc> found = Collections.synchronizedMap(new LinkedHashMap<>());
    private volatile boolean discovering;
    private Thread discoveryThread;
    private boolean autoConnectAllowed = true;

    // connection
    private Link link;
    private RtspServer rtsp;
    private boolean rtspBound;
    private final ServiceConnection rtspConnection = new ServiceConnection() {
        @Override public void onServiceConnected(ComponentName name, IBinder binder) {
            rtsp = ((RtspServer.LocalBinder) binder).getService();
            rtsp.addCallbackListener(rtspListener);
        }
        @Override public void onServiceDisconnected(ComponentName name) { rtsp = null; }
    };
    private final RtspServer.CallbackListener rtspListener = new RtspServer.CallbackListener() {
        @Override public void onError(RtspServer server, Exception e, int error) {
            ui.post(() -> {
                if (error == RtspServer.ERROR_BIND_FAILED) {
                    // port taken: pick another one next time
                    rtspPort = 20000 + new Random().nextInt(10000);
                    prefs.edit().putInt("ocam_rtsp_port", rtspPort)
                            .putString(RtspServer.KEY_PORT, String.valueOf(rtspPort)).apply();
                }
            });
        }
        @Override public void onMessage(RtspServer server, int message) {
            ui.post(() -> {
                if (link == null) return;
                if (message == RtspServer.MESSAGE_STREAMING_STARTED) setState(LinkState.LIVE);
                else if (message == RtspServer.MESSAGE_STREAMING_STOPPED && state == LinkState.LIVE) setState(LinkState.WAITING_PC);
            });
        }
    };

    // ------------------------------------------------------------ lifecycle
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        prefs = PreferenceManager.getDefaultSharedPreferences(this);

        statusText = findViewById(R.id.status_text);
        statusDot = findViewById(R.id.status_dot);
        title = findViewById(R.id.title);
        subtitle = findViewById(R.id.subtitle);
        searching = findViewById(R.id.searching);
        pcList = findViewById(R.id.pc_list);
        pickPanel = findViewById(R.id.pick_panel);
        livePanel = findViewById(R.id.live_panel);
        manualRow = findViewById(R.id.manual_row);
        address = findViewById(R.id.address);
        grant = findViewById(R.id.grant);
        torch = findViewById(R.id.torch);
        preview = findViewById(R.id.preview);
        previewNote = findViewById(R.id.preview_note);
        usbButton = findViewById(R.id.use_usb);
        settingsPanel = findViewById(R.id.settings_panel);
        settingsList = findViewById(R.id.settings_list);

        // Our own key: libstreaming owns "rtsp_port" and stores it as a String.
        rtspPort = prefs.getInt("ocam_rtsp_port", 0);
        if (rtspPort < 1024) rtspPort = 20000 + new Random().nextInt(10000);
        // Write the library's copy before the service can be bound and created.
        prefs.edit().remove(RtspServer.KEY_PORT)
                .putInt("ocam_rtsp_port", rtspPort)
                .putString(RtspServer.KEY_PORT, String.valueOf(rtspPort)).commit();
        address.setText(prefs.getString("last_host", ""));

        camera.init(this);
        applyPhoneSettings();
        camera.setListener(cameraListener);
        preview.setSurfaceTextureListener(previewListener);
        SessionBuilder.getInstance()
                .setContext(getApplicationContext())
                .setAudioEncoder(SessionBuilder.AUDIO_NONE)
                .setVideoEncoder(SessionBuilder.VIDEO_H264);

        findViewById(R.id.manual_toggle).setOnClickListener(v -> {
            manualRow.setVisibility(manualRow.getVisibility() == View.VISIBLE ? View.GONE : View.VISIBLE);
            if (manualRow.getVisibility() == View.VISIBLE) address.requestFocus();
        });
        findViewById(R.id.connect).setOnClickListener(v -> connectManual());
        address.setOnEditorActionListener((v, actionId, event) -> {
            if (actionId == EditorInfo.IME_ACTION_GO) { connectManual(); return true; }
            return false;
        });
        findViewById(R.id.disconnect).setOnClickListener(v -> disconnect());
        findViewById(R.id.flip).setOnClickListener(v -> { if (link != null) link.requestFlip(); });
        torch.setOnClickListener(v -> {
            boolean on = camera.toggleTorchLocal();
            renderTorch();
            Link l = link;
            if (l != null) l.sendMessage("EVT torch=" + (on ? 1 : 0));
        });
        findViewById(R.id.settings).setOnClickListener(v -> showSettings(true));
        findViewById(R.id.settings_link).setOnClickListener(v -> showSettings(true));
        usbButton.setOnClickListener(v -> useUsbCable());
        grant.setOnClickListener(v -> askCameraPermission(true));

        bindService(new Intent(this, RtspServer.class), rtspConnection, Context.BIND_AUTO_CREATE);
        rtspBound = true;

        if (hasCameraPermission()) setState(LinkState.IDLE);
        else { setState(LinkState.NO_PERMISSION); askCameraPermission(false); }
    }

    @Override
    protected void onResume() {
        super.onResume();
        startDiscovery();
    }

    @Override
    protected void onPause() {
        super.onPause();
        // While connected, keep looking: a USB pop-up (e.g. MIUI's "Use USB for")
        // can cover the app right when the cable comes back.
        if (link == null) stopDiscovery();
    }

    @Override
    public void onBackPressed() {
        if (settingsPanel.getVisibility() == View.VISIBLE) { showSettings(false); return; }
        super.onBackPressed();
    }

    @Override
    public boolean dispatchTouchEvent(MotionEvent ev) {
        if (ev.getAction() == MotionEvent.ACTION_DOWN) {
            boolean wasDimmed = dimmed;
            undim();
            if (wasDimmed) return true; // first tap only wakes the screen
        }
        return super.dispatchTouchEvent(ev);
    }

    @Override
    protected void onDestroy() {
        ui.removeCallbacks(dimNow);
        camera.clearListener(cameraListener);
        disconnect();
        stopDiscovery();
        if (rtspBound) {
            if (rtsp != null) rtsp.removeCallbackListener(rtspListener);
            unbindService(rtspConnection);
            rtspBound = false;
        }
        super.onDestroy();
    }

    // ------------------------------------------------------------ permission
    private boolean hasCameraPermission() {
        return Build.VERSION.SDK_INT < 23 || checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED;
    }

    private void askCameraPermission(boolean fromButton) {
        if (Build.VERSION.SDK_INT < 23) return;
        if (fromButton && !shouldShowRequestPermissionRationale(Manifest.permission.CAMERA)
                && prefs.getBoolean("asked_camera", false)) {
            // "Don't ask again" was chosen: the system dialog won't show, open settings.
            startActivity(new Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.parse("package:" + getPackageName())));
            return;
        }
        prefs.edit().putBoolean("asked_camera", true).apply();
        requestPermissions(new String[]{Manifest.permission.CAMERA}, REQ_CAMERA);
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        if (requestCode == REQ_CAMERA) setState(hasCameraPermission() ? LinkState.IDLE : LinkState.NO_PERMISSION);
    }

    // ------------------------------------------------------------ UI state
    private void setState(LinkState s) {
        state = s;
        int dot;
        String chip;
        switch (s) {
            case CONNECTING:   dot = R.color.accent; chip = "Connecting to " + connectedName + "…"; break;
            case WAITING_PC:   dot = R.color.warn;   chip = "Connected – accept on the PC if asked"; break;
            case LIVE:         dot = R.color.good;   chip = "Live on " + connectedName + "  ·  " + linkLabel(); break;
            case RECONNECTING: dot = R.color.warn;   chip = "Connection lost – reconnecting…"; break;
            case NO_PERMISSION:dot = R.color.bad;    chip = "Camera access needed"; break;
            default:           dot = R.color.muted;  chip = "Not connected"; break;
        }
        statusText.setText(chip);
        scheduleDim();
        statusDot.setImageTintList(ColorStateList.valueOf(color(dot)));

        final boolean connected = s == LinkState.CONNECTING || s == LinkState.WAITING_PC || s == LinkState.LIVE || s == LinkState.RECONNECTING;
        pickPanel.setVisibility(!connected && s != LinkState.NO_PERMISSION ? View.VISIBLE : View.GONE);
        livePanel.setVisibility(connected ? View.VISIBLE : View.GONE);
        grant.setVisibility(s == LinkState.NO_PERMISSION ? View.VISIBLE : View.GONE);
        renderUsbButton();

        if (s == LinkState.NO_PERMISSION) {
            title.setText(R.string.app_name);
            subtitle.setText(R.string.need_camera);
        } else if (connected) {
            title.setText(connectedName);
            subtitle.setText(s == LinkState.LIVE ? "Your phone is the webcam now. Pick \"OpenCamera\" in your video app."
                    : s == LinkState.WAITING_PC ? "If the PC asks, click Allow. Video starts by itself."
                    : s == LinkState.RECONNECTING ? "Make sure OpenCamera is still open on the PC and both are on the same Wi-Fi."
                    : "Saying hello to the PC…");
        } else {
            title.setText(R.string.choose_computer);
            subtitle.setText(R.string.choose_hint);
        }
    }

    @SuppressWarnings("deprecation")
    private int color(int id) {
        return Build.VERSION.SDK_INT >= 23 ? getColor(id) : getResources().getColor(id);
    }

    private int dp(float v) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics());
    }

    private void renderPcList() {
        pcList.removeAllViews();
        List<Pc> pcs;
        synchronized (found) { pcs = new ArrayList<>(found.values()); }
        final String last = prefs.getString("last_host", "");
        final String lastName = prefs.getString("last_name", "");
        for (Pc pc : pcs) {
            final String host = bestHost(pc);
            if (host == null) continue;
            final boolean viaUsb = isUsbHost(pc, host);
            LinearLayout row = new LinearLayout(this);
            row.setOrientation(LinearLayout.VERTICAL);
            row.setBackgroundResource(R.drawable.bg_row);
            row.setPadding(dp(18), dp(14), dp(18), dp(14));
            row.setClickable(true);
            row.setFocusable(true);
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
            lp.bottomMargin = dp(8);
            row.setLayoutParams(lp);

            TextView name = new TextView(this);
            name.setText(pc.name);
            name.setTextColor(color(R.color.text));
            name.setTextSize(TypedValue.COMPLEX_UNIT_SP, 18);
            name.setTypeface(android.graphics.Typeface.create("sans-serif-medium", android.graphics.Typeface.NORMAL));
            TextView sub = new TextView(this);
            String tag = host + "  ·  " + (viaUsb ? "USB cable" : "Wi-Fi");
            if (pc.name.equals(lastName) || pc.seen.containsKey(last)) tag += "  ·  " + getString(R.string.last_used);
            sub.setText(tag);
            sub.setTextColor(color(R.color.muted));
            sub.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
            row.addView(name);
            row.addView(sub);
            row.setContentDescription("Connect to " + pc.name);
            row.setOnClickListener(v -> connect(host, pc.name));
            pcList.addView(row);
        }
        searching.setVisibility(pcs.isEmpty() ? View.VISIBLE : View.GONE);
    }

    // ------------------------------------------------------------ discovery
    private void startDiscovery() {
        if (discovering) return;
        discovering = true;
        discoveryThread = new Thread(this::discoveryLoop, "discovery");
        discoveryThread.start();
    }

    private void stopDiscovery() {
        discovering = false;
        if (discoveryThread != null) discoveryThread.interrupt();
        discoveryThread = null;
    }

    private static List<InetAddress> broadcastTargets() {
        List<InetAddress> out = new ArrayList<>();
        try {
            out.add(InetAddress.getByName("255.255.255.255"));
            for (NetworkInterface ni : Collections.list(NetworkInterface.getNetworkInterfaces())) {
                if (!ni.isUp() || ni.isLoopback()) continue;
                for (InterfaceAddress ia : ni.getInterfaceAddresses()) {
                    if (ia.getBroadcast() != null && !out.contains(ia.getBroadcast())) out.add(ia.getBroadcast());
                }
            }
        } catch (Exception ignored) { }
        return out;
    }

    private void discoveryLoop() {
        final byte[] probe = "OPENCAMERA?".getBytes(StandardCharsets.US_ASCII);
        DatagramSocket sock = null;
        try {
            sock = new DatagramSocket();
            sock.setBroadcast(true);
            sock.setSoTimeout(300);
            long nextProbe = 0;
            byte[] buf = new byte[512];
            while (discovering) {
                long now = System.currentTimeMillis();
                if (now >= nextProbe) {
                    for (InetAddress t : broadcastTargets()) {
                        try { sock.send(new DatagramPacket(probe, probe.length, t, DISCOVERY_PORT)); } catch (IOException ignored) { }
                    }
                    nextProbe = now + 1500;
                    probeUsbCable();
                    pruneStale(now);
                }
                try {
                    DatagramPacket p = new DatagramPacket(buf, buf.length);
                    sock.receive(p);
                    String msg = new String(p.getData(), 0, p.getLength(), StandardCharsets.UTF_8);
                    if (msg.startsWith("OPENCAMERA1|")) {
                        String[] parts = msg.split("\\|");
                        String host = p.getAddress().getHostAddress();
                        String name = parts.length > 1 && !parts[1].isEmpty() ? parts[1] : host;
                        boolean usb = isUsbAddress(p.getAddress());
                        boolean isNew;
                        Pc pc;
                        synchronized (found) {
                            pc = found.get(name);
                            if (pc == null) { pc = new Pc(); pc.name = name; found.put(name, pc); }
                            isNew = !pc.seen.containsKey(host);
                            pc.seen.put(host, System.currentTimeMillis());
                            pc.usb.put(host, usb);
                        }
                        final Pc fpc = pc;
                        if (isNew) ui.post(() -> { renderPcList(); renderUsbButton(); maybeAutoConnect(fpc); });
                    }
                } catch (SocketTimeoutException ignored) {
                }
            }
        } catch (IOException ignored) {
        } finally {
            if (sock != null) sock.close();
        }
    }

    private void pruneStale(long now) {
        boolean changed = false;
        synchronized (found) {
            Iterator<Pc> it = found.values().iterator();
            while (it.hasNext()) {
                Pc pc = it.next();
                Iterator<Map.Entry<String, Long>> hi = pc.seen.entrySet().iterator();
                while (hi.hasNext()) if (now - hi.next().getValue() > 6000) { hi.remove(); changed = true; }
                if (pc.seen.isEmpty()) it.remove();
            }
        }
        if (changed) ui.post(() -> { renderPcList(); renderUsbButton(); });
    }

    /**
     * USB cable without tethering: when the PC has set up "adb reverse" (USB debugging on),
     * 127.0.0.1:65501 on this phone leads to the PC, which answers like a discovery reply.
     */
    private void probeUsbCable() {
        Socket s = new Socket();
        try {
            s.connect(new InetSocketAddress("127.0.0.1", DISCOVERY_PORT), 300);
            s.setSoTimeout(700);
            s.getOutputStream().write("OPENCAMERA?\n".getBytes(StandardCharsets.US_ASCII));
            BufferedReader in = new BufferedReader(new InputStreamReader(s.getInputStream(), StandardCharsets.UTF_8));
            String msg = in.readLine();
            if (msg == null || !msg.startsWith("OPENCAMERA1|")) return;
            String[] parts = msg.split("\\|");
            String host = "127.0.0.1";
            String name = parts.length > 1 && !parts[1].isEmpty() ? parts[1] : "PC (USB cable)";
            boolean isNew;
            Pc pc;
            synchronized (found) {
                pc = found.get(name);
                if (pc == null) { pc = new Pc(); pc.name = name; found.put(name, pc); }
                isNew = !pc.seen.containsKey(host);
                pc.seen.put(host, System.currentTimeMillis());
                pc.usb.put(host, true);
            }
            final Pc fpc = pc;
            if (isNew) ui.post(() -> { renderPcList(); renderUsbButton(); maybeAutoConnect(fpc); });
        } catch (IOException ignored) {
            // no cable, USB debugging off, or OpenCamera not running on the PC
        } finally {
            try { s.close(); } catch (IOException ignored) { }
        }
    }

    // ------------------------------------------------------------ Wi-Fi or USB cable
    /** Android names the USB tethering network rndis0, usb0 or ncm0. */
    private static boolean isUsbInterface(NetworkInterface ni) {
        String n = ni.getName();
        return n != null && (n.startsWith("rndis") || n.startsWith("usb") || n.startsWith("ncm"));
    }

    /** True when `a` sits on the phone's USB tethering network. */
    private static boolean isUsbAddress(InetAddress a) {
        byte[] t = a.getAddress();
        try {
            for (NetworkInterface ni : Collections.list(NetworkInterface.getNetworkInterfaces())) {
                if (!ni.isUp() || !isUsbInterface(ni)) continue;
                for (InterfaceAddress ia : ni.getInterfaceAddresses()) {
                    byte[] m = ia.getAddress().getAddress();
                    if (m.length != t.length) continue;
                    int bits = ia.getNetworkPrefixLength();
                    boolean same = true;
                    for (int i = 0; i < m.length && bits > 0; i++, bits -= 8) {
                        int mask = bits >= 8 ? 0xFF : (0xFF << (8 - bits)) & 0xFF;
                        if ((m[i] & mask) != (t[i] & mask)) { same = false; break; }
                    }
                    if (same) return true;
                }
            }
        } catch (Exception ignored) { }
        return false;
    }

    private static boolean isUsbHost(Pc pc, String host) {
        Boolean b = pc.usb.get(host);
        return b != null && b;
    }

    /** The address to use for this PC now: the cable if it answers there (and the user prefers it), else the newest. */
    private String bestHost(Pc pc) {
        final boolean preferUsb = prefs.getBoolean("prefer_usb", true);
        final long now = System.currentTimeMillis();
        String best = null;
        long bestSeen = 0;
        synchronized (found) {
            for (Map.Entry<String, Long> e : pc.seen.entrySet()) {
                if (now - e.getValue() > FRESH_MS) continue;
                boolean usb = isUsbHost(pc, e.getKey());
                if (preferUsb && usb) return e.getKey();
                if (!usb && e.getValue() > bestSeen) { best = e.getKey(); bestSeen = e.getValue(); }
            }
            if (best == null) // only the cable answers, or nothing fresh
                for (Map.Entry<String, Long> e : pc.seen.entrySet())
                    if (e.getValue() > bestSeen) { best = e.getKey(); bestSeen = e.getValue(); }
        }
        return best;
    }

    private String linkLabel() {
        Link l = link;
        return l != null && l.viaUsb ? "USB cable" : "Wi-Fi";
    }

    private boolean pluggedIntoUsb() {
        Intent b = registerReceiver(null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
        return b != null && b.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0) == BatteryManager.BATTERY_PLUGGED_USB;
    }

    private void renderUsbButton() {
        if (usbButton == null) return;
        Link l = link;
        boolean onUsb = l != null && l.viaUsb && state == LinkState.LIVE;
        usbButton.setText(onUsb ? "Connected by USB cable" : "Use USB cable instead of Wi-Fi");
        usbButton.setTextColor(color(onUsb ? R.color.good : R.color.accent));
        usbButton.setEnabled(!onUsb);
    }

    /** Guide the user to turn on USB tethering; the link moves to the cable by itself. */
    private void useUsbCable() {
        Link l = link;
        if (l != null && l.viaUsb) { Toast.makeText(this, "Already using the USB cable.", Toast.LENGTH_SHORT).show(); return; }
        AlertDialog.Builder d = new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert);
        if (!pluggedIntoUsb()) {
            d.setTitle("Plug in the USB cable")
             .setMessage("Connect this phone to the PC with a USB cable, then tap this button again.\n\n"
                     + "A cable is steadier than Wi-Fi and charges the phone while you film.")
             .setPositiveButton("OK", null);
        } else if (usbDebuggingOn()) {
            d.setTitle("Using the USB cable")
             .setMessage("USB debugging is on, so OpenCamera on the PC switches to the cable by itself within a few seconds.\n\n"
                     + "Not switching? Make sure OpenCamera is open on the PC, and tap \"Allow\" if the phone asks about USB debugging. "
                     + "You can also turn on USB tethering instead.")
             .setPositiveButton("OK", null)
             .setNeutralButton("USB tethering", (x, y) -> openTetherSettings());
        } else {
            d.setTitle("Turn on USB tethering")
             .setMessage("Android will open its sharing settings. Turn on \"USB tethering\", then come back here.\n\n"
                     + "OpenCamera moves to the cable by itself, and goes back to Wi-Fi if you unplug it.")
             .setPositiveButton("Open settings", (x, y) -> openTetherSettings())
             .setNegativeButton("Cancel", null);
        }
        d.show();
    }

    private boolean usbDebuggingOn() {
        try { return Settings.Global.getInt(getContentResolver(), Settings.Global.ADB_ENABLED, 0) == 1; }
        catch (Exception e) { return false; }
    }

    private void openTetherSettings() {
        Intent i = new Intent(Intent.ACTION_MAIN);
        i.setComponent(new ComponentName("com.android.settings", "com.android.settings.TetherSettings"));
        try {
            startActivity(i);
        } catch (ActivityNotFoundException | SecurityException e) {
            try { startActivity(new Intent(Settings.ACTION_WIRELESS_SETTINGS)); }
            catch (ActivityNotFoundException e2) { startActivity(new Intent(Settings.ACTION_SETTINGS)); }
        }
    }

    // Reconnect to the PC used last time without asking, once per app start.
    private void maybeAutoConnect(Pc pc) {
        if (!autoConnectAllowed || state != LinkState.IDLE || link != null) return;
        boolean known = pc.name.equals(prefs.getString("last_name", ""));
        synchronized (found) { known |= pc.seen.containsKey(prefs.getString("last_host", "")); }
        if (known && prefs.getBoolean("auto_connect", true)) {
            String host = bestHost(pc);
            if (host == null) return;
            autoConnectAllowed = false;
            connect(host, pc.name);
        }
    }

    // ------------------------------------------------------------ connection
    private void connectManual() {
        String host = address.getText().toString().trim();
        if (host.isEmpty()) { address.setError("Type the address shown on the PC"); return; }
        InputMethodManager imm = (InputMethodManager) getSystemService(INPUT_METHOD_SERVICE);
        if (imm != null) imm.hideSoftInputFromWindow(address.getWindowToken(), 0);
        connect(host, host);
    }

    private void connect(String host, String name) {
        if (!hasCameraPermission()) { setState(LinkState.NO_PERMISSION); askCameraPermission(false); return; }
        if (link != null) link.shutdown();
        connectedName = name;
        prefs.edit().putString("last_host", host).putString("last_name", name).putBoolean("auto_connect", true).apply();
        link = new Link(host, name);
        setState(LinkState.CONNECTING);
        link.start();
    }

    private void disconnect() {
        autoConnectAllowed = false;
        if (link != null) {
            link.shutdown();
            link = null;
            prefs.edit().putBoolean("auto_connect", false).apply();
        }
        if (state != LinkState.NO_PERMISSION) setState(LinkState.IDLE);
        renderPcList();
    }

    private void startRtsp() {
        prefs.edit().putString(RtspServer.KEY_PORT, String.valueOf(rtspPort)).commit();
        try {
            startService(new Intent(this, RtspServer.class));
        } catch (IllegalStateException | SecurityException e) {
            // Android 8+ refuses this while the app is in the background. The
            // service still runs while bound (bindService in onCreate).
        }
    }

    private void stopRtsp() {
        stopService(new Intent(this, RtspServer.class));
    }

    private static String deviceName(Context c) {
        String n = null;
        if (Build.VERSION.SDK_INT >= 25) n = Settings.Global.getString(c.getContentResolver(), Settings.Global.DEVICE_NAME);
        if (n == null || n.trim().isEmpty()) n = Build.MODEL;
        return n;
    }

    // ------------------------------------------------------------ camera + preview
    private final CameraEngine.Listener cameraListener = new CameraEngine.Listener() {
        @Override public void onCaps(String caps) {
            lastCaps = caps;
            Link l = link;
            if (l != null) l.sendMessage("CAPS " + caps);
        }
        @Override public void onStatus(String status) {
            Link l = link;
            if (l != null) l.sendMessage("STAT " + status);
        }
        @Override public void onPreviewGeometry(int w, int h, boolean front) {
            ui.post(() -> { frameW = w; frameH = h; frameFront = front; fitPreview(); });
        }
        @Override public void onRunning(boolean running) {
            ui.post(() -> { cameraRunning = running; renderPreviewNote(); renderTorch(); });
        }
    };

    private final TextureView.SurfaceTextureListener previewListener = new TextureView.SurfaceTextureListener() {
        @Override public void onSurfaceTextureAvailable(SurfaceTexture st, int w, int h) {
            camera.setPreviewTexture(st);
            fitPreview();
        }
        @Override public void onSurfaceTextureSizeChanged(SurfaceTexture st, int w, int h) { fitPreview(); }
        @Override public boolean onSurfaceTextureDestroyed(SurfaceTexture st) {
            camera.setPreviewTexture(null); // waits until the camera stopped drawing into it
            return true;
        }
        @Override public void onSurfaceTextureUpdated(SurfaceTexture st) { }
    };

    /** Fill the screen with the (portrait-turned) camera picture, cropping the edges. */
    private void fitPreview() {
        int vw = preview.getWidth(), vh = preview.getHeight();
        if (vw == 0 || vh == 0 || frameW == 0 || frameH == 0) return;
        float ar = frameH / (float) frameW; // width / height once shown upright in portrait
        float hh = Math.max(vh, vw / ar), ww = ar * hh;
        Matrix m = new Matrix();
        m.setScale((frameFront ? -1f : 1f) * ww / vw, hh / vh, vw / 2f, vh / 2f); // selfie view is mirrored
        preview.setTransform(m);
    }

    private void renderPreviewNote() {
        boolean show = prefs.getBoolean("set_preview", true);
        preview.setAlpha(show ? 1f : 0f);
        previewNote.setVisibility(!show && cameraRunning ? View.VISIBLE : View.GONE);
    }

    private void renderTorch() {
        boolean on = camera.torchOn();
        torch.setText(on ? R.string.light_on : R.string.light_off);
        torch.setBackgroundResource(on ? R.drawable.bg_primary : R.drawable.bg_secondary);
        torch.setTextColor(color(on ? R.color.on_accent : R.color.text));
    }

    private void applyPhoneSettings() {
        camera.showPreview = prefs.getBoolean("set_preview", true);
        int br = prefs.getInt("set_bitrate", 1);
        camera.bitrateScale = br == 0 ? 0.6f : br == 2 ? 1.6f : 1f;
        camera.reportOrientation = prefs.getBoolean("set_orient", true);
    }

    // ------------------------------------------------------------ screen dimming
    private void scheduleDim() {
        ui.removeCallbacks(dimNow);
        if (state == LinkState.LIVE && prefs.getBoolean("set_dim", false)) ui.postDelayed(dimNow, 20000);
        else undim();
    }

    private void dimScreen() {
        if (settingsPanel.getVisibility() == View.VISIBLE) return;
        WindowManager.LayoutParams lp = getWindow().getAttributes();
        lp.screenBrightness = 0.02f;
        getWindow().setAttributes(lp);
        dimmed = true;
    }

    private void undim() {
        if (dimmed) {
            WindowManager.LayoutParams lp = getWindow().getAttributes();
            lp.screenBrightness = WindowManager.LayoutParams.BRIGHTNESS_OVERRIDE_NONE;
            getWindow().setAttributes(lp);
            dimmed = false;
        }
        if (state == LinkState.LIVE && prefs.getBoolean("set_dim", false)) {
            ui.removeCallbacks(dimNow);
            ui.postDelayed(dimNow, 20000);
        }
    }

    // ------------------------------------------------------------ settings screen
    private void showSettings(boolean show) {
        if (show) buildSettings();
        settingsPanel.setVisibility(show ? View.VISIBLE : View.GONE);
    }

    private void buildSettings() {
        LinearLayout list = settingsList;
        list.removeAllViews();

        LinearLayout head = new LinearLayout(this);
        head.setOrientation(LinearLayout.HORIZONTAL);
        head.setGravity(Gravity.CENTER_VERTICAL);
        TextView t = new TextView(this);
        t.setText(R.string.settings);
        t.setTextColor(color(R.color.text));
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, 24);
        t.setTypeface(android.graphics.Typeface.create("sans-serif-medium", android.graphics.Typeface.NORMAL));
        head.addView(t, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        TextView done = new TextView(this);
        done.setText("Done");
        done.setTextColor(color(R.color.accent));
        done.setTextSize(TypedValue.COMPLEX_UNIT_SP, 17);
        done.setPadding(dp(16), dp(12), dp(4), dp(12));
        done.setOnClickListener(v -> showSettings(false));
        head.addView(done);
        list.addView(head);

        section(list, "VIDEO");
        switchRow(list, "Show preview on this phone", "Turn off to save battery and keep the phone cooler. The PC still gets video.",
                "set_preview", true, on -> { camera.setShowPreview(on); renderPreviewNote(); });
        choiceRow(list, "Video data", "How much Wi-Fi the video uses. Higher looks sharper. Applies the next time video starts.",
                new String[] { "Saver", "Standard", "High" }, "set_bitrate", 1, v -> applyPhoneSettings());
        switchRow(list, "Tell the PC how the phone is held", "Lets the PC turn the picture upright automatically.",
                "set_orient", true, on -> applyPhoneSettings());

        section(list, "SCREEN");
        switchRow(list, "Dim the screen while live", "Saves battery. Tap the screen to brighten it again.",
                "set_dim", false, on -> scheduleDim());

        section(list, "CONNECTION");
        switchRow(list, "Reconnect automatically", "Connect to the last computer when the app opens.",
                "auto_connect", true, on -> { });
        switchRow(list, "Prefer the USB cable", "When the phone is plugged into the PC with USB tethering on, use the cable instead of Wi-Fi.",
                "prefer_usb", true, on -> { });
        Button usb = new Button(this);
        usb.setText("Use USB cable…");
        usb.setAllCaps(false);
        usb.setTextColor(color(R.color.text));
        usb.setBackgroundResource(R.drawable.bg_secondary);
        usb.setStateListAnimator(null);
        LinearLayout.LayoutParams ulp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(48));
        ulp.topMargin = dp(4);
        ulp.bottomMargin = dp(8);
        usb.setOnClickListener(v -> useUsbCable());
        list.addView(usb, ulp);
        String last = prefs.getString("last_host", "");
        if (!last.isEmpty()) {
            Button forget = new Button(this);
            forget.setText("Forget " + last);
            forget.setAllCaps(false);
            forget.setTextColor(color(R.color.text));
            forget.setBackgroundResource(R.drawable.bg_secondary);
            forget.setStateListAnimator(null);
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(48));
            lp.topMargin = dp(4);
            forget.setOnClickListener(v -> {
                prefs.edit().remove("last_host").remove("last_name").apply();
                address.setText("");
                forget.setText("Forgotten");
                forget.setEnabled(false);
            });
            list.addView(forget, lp);
        }

        section(list, "ABOUT");
        infoRow(list, "This phone", phoneAddresses());
        infoRow(list, "Camera", camera.supportSummary());
        String version = "";
        try { version = getPackageManager().getPackageInfo(getPackageName(), 0).versionName; } catch (Exception ignored) { }
        infoRow(list, "App version", version);
    }

    private interface OnBool { void on(boolean v); }
    private interface OnInt { void on(int v); }

    private void section(LinearLayout list, String title) {
        TextView s = new TextView(this);
        s.setText(title);
        s.setTextColor(color(R.color.muted));
        s.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        s.setLetterSpacing(0.08f);
        s.setPadding(dp(4), dp(24), 0, dp(8));
        list.addView(s);
    }

    private LinearLayout card(LinearLayout list) {
        LinearLayout c = new LinearLayout(this);
        c.setOrientation(LinearLayout.VERTICAL);
        c.setBackgroundResource(R.drawable.bg_row);
        c.setPadding(dp(16), dp(14), dp(16), dp(14));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.bottomMargin = dp(8);
        list.addView(c, lp);
        return c;
    }

    private TextView label(String text, boolean title) {
        TextView v = new TextView(this);
        v.setText(text);
        v.setTextColor(color(title ? R.color.text : R.color.muted));
        v.setTextSize(TypedValue.COMPLEX_UNIT_SP, title ? 16 : 13);
        if (!title) v.setPadding(0, dp(3), 0, 0);
        return v;
    }

    private void switchRow(LinearLayout list, String title, String hint, String key, boolean def, OnBool onChange) {
        LinearLayout c = card(list);
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        LinearLayout texts = new LinearLayout(this);
        texts.setOrientation(LinearLayout.VERTICAL);
        texts.addView(label(title, true));
        texts.addView(label(hint, false));
        row.addView(texts, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        Switch sw = new Switch(this);
        sw.setChecked(prefs.getBoolean(key, def));
        sw.setPadding(dp(12), 0, 0, 0);
        sw.setContentDescription(title);
        sw.setOnCheckedChangeListener((b, on) -> {
            prefs.edit().putBoolean(key, on).apply();
            onChange.on(on);
        });
        row.addView(sw);
        c.addView(row);
        c.setOnClickListener(v -> sw.toggle());
    }

    private void choiceRow(LinearLayout list, String title, String hint, String[] options, String key, int def, OnInt onChange) {
        LinearLayout c = card(list);
        c.addView(label(title, true));
        c.addView(label(hint, false));
        LinearLayout seg = new LinearLayout(this);
        seg.setOrientation(LinearLayout.HORIZONTAL);
        LinearLayout.LayoutParams slp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(44));
        slp.topMargin = dp(12);
        c.addView(seg, slp);
        final TextView[] items = new TextView[options.length];
        final Runnable render = () -> {
            int cur = prefs.getInt(key, def);
            for (int i = 0; i < items.length; i++) {
                boolean on = i == cur;
                items[i].setBackgroundResource(on ? R.drawable.bg_primary : R.drawable.bg_secondary);
                items[i].setTextColor(color(on ? R.color.on_accent : R.color.text));
            }
        };
        for (int i = 0; i < options.length; i++) {
            TextView o = new TextView(this);
            o.setText(options[i]);
            o.setGravity(Gravity.CENTER);
            o.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
            o.setContentDescription(title + ": " + options[i]);
            final int idx = i;
            o.setOnClickListener(v -> {
                prefs.edit().putInt(key, idx).apply();
                render.run();
                onChange.on(idx);
            });
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1);
            if (i > 0) lp.leftMargin = dp(6);
            seg.addView(o, lp);
            items[i] = o;
        }
        render.run();
    }

    private void infoRow(LinearLayout list, String title, String value) {
        LinearLayout c = card(list);
        c.addView(label(title, false));
        TextView v = label(value == null || value.isEmpty() ? "-" : value, true);
        v.setTextIsSelectable(true);
        c.addView(v);
    }

    private static String phoneAddresses() {
        StringBuilder sb = new StringBuilder();
        try {
            for (NetworkInterface ni : Collections.list(NetworkInterface.getNetworkInterfaces())) {
                if (!ni.isUp() || ni.isLoopback()) continue;
                for (InterfaceAddress ia : ni.getInterfaceAddresses()) {
                    InetAddress a = ia.getAddress();
                    if (a.getAddress().length != 4) continue;
                    if (sb.length() > 0) sb.append('\n');
                    sb.append(a.getHostAddress()).append("  (").append(ni.getName()).append(')');
                }
            }
        } catch (Exception ignored) { }
        return sb.toString();
    }

    /** Keeps the control link to one PC alive, reconnecting until shutdown(). */
    private class Link extends Thread {
        private final String fallbackHost; // used when discovery doesn't see the PC
        private final String pcName;
        volatile boolean viaUsb;
        private volatile String host;
        private final AtomicBoolean wanted = new AtomicBoolean(true);
        private final AtomicBoolean flip = new AtomicBoolean(false);
        private volatile Socket socket;
        private volatile DataOutputStream out;
        private volatile boolean pcTalksBack;
        private final Object writeLock = new Object();

        Link(String host, String name) {
            super("link");
            this.fallbackHost = host;
            this.host = host;
            this.pcName = name;
        }

        /** Address for the next attempt: the best one discovery saw for this PC, else the last one used. */
        private String pickHost() {
            Pc pc = found.get(pcName);
            String h = pc != null ? bestHost(pc) : null;
            return h != null ? h : (host != null ? host : fallbackHost);
        }

        /** Move to a better path: the cable appeared, or the cable we were on went away. */
        private boolean shouldMove(Socket s) {
            if (viaUsb) {
                if (s.getInetAddress().isLoopbackAddress()) // adb: the socket closes when the cable goes
                    return !prefs.getBoolean("prefer_usb", true) && !pickHost().equals(host);
                try {
                    NetworkInterface ni = NetworkInterface.getByInetAddress(s.getLocalAddress());
                    if (ni == null || !ni.isUp()) return true;
                } catch (Exception e) { return true; }
                return !prefs.getBoolean("prefer_usb", true) && !pickHost().equals(host);
            }
            if (!prefs.getBoolean("prefer_usb", true)) return false;
            Pc pc = found.get(pcName);
            String h = pc != null ? bestHost(pc) : null;
            return h != null && !h.equals(host) && isUsbHost(pc, h);
        }

        void requestFlip() { flip.set(true); }

        /** Framed text message to a PC that said HELLO; dropped otherwise. */
        void sendMessage(String text) {
            DataOutputStream o = out;
            if (o == null || !pcTalksBack) return;
            byte[] b = text.getBytes(StandardCharsets.UTF_8);
            synchronized (writeLock) {
                try {
                    o.write('M');
                    o.writeInt(b.length);
                    o.write(b);
                    o.flush();
                } catch (IOException ignored) {
                    // the heartbeat loop notices the broken socket
                }
            }
        }

        private void readLoop(Socket s) {
            try {
                BufferedReader in = new BufferedReader(new InputStreamReader(s.getInputStream(), StandardCharsets.UTF_8));
                String line;
                while (wanted.get() && (line = in.readLine()) != null) handleLine(line.trim());
            } catch (IOException ignored) {
            }
        }

        private void handleLine(String line) {
            if (line.startsWith("HELLO")) {
                pcTalksBack = true;
                String caps = lastCaps;
                if (caps != null) sendMessage("CAPS " + caps);
            } else if (line.startsWith("SET ")) {
                Map<String, String> m = new HashMap<>();
                for (String kv : line.substring(4).split(" ")) {
                    int eq = kv.indexOf('=');
                    if (eq > 0) m.put(kv.substring(0, eq), kv.substring(eq + 1));
                }
                camera.applyFromPc(m);
                ui.post(MainActivity.this::renderTorch);
            } else if (line.startsWith("POINT")) {
                String[] p = line.split(" ");
                try {
                    if (p.length >= 3) camera.setPoint(Float.parseFloat(p[1]), Float.parseFloat(p[2]));
                    else camera.setPoint(-1, -1);
                } catch (NumberFormatException e) {
                    camera.setPoint(-1, -1);
                }
            }
        }

        void shutdown() {
            wanted.set(false);
            Socket s = socket;
            if (s != null) try { s.close(); } catch (IOException ignored) { }
            interrupt();
        }

        private void post(LinkState s) {
            ui.post(() -> { if (link == this) setState(s); });
        }

        @Override
        public void run() {
            int attempt = 0;
            final byte[] name = deviceName(getApplicationContext()).getBytes(StandardCharsets.UTF_8);
            while (wanted.get()) {
                post(attempt == 0 ? LinkState.CONNECTING : LinkState.RECONNECTING);
                boolean moving = false;
                try {
                    host = pickHost();
                    Socket s = new Socket();
                    socket = s;
                    s.connect(new InetSocketAddress(host, CONTROL_PORT), 3000);
                    try {
                        NetworkInterface ni = NetworkInterface.getByInetAddress(s.getLocalAddress());
                        viaUsb = s.getInetAddress().isLoopbackAddress() || (ni != null && isUsbInterface(ni));
                    } catch (Exception e) { viaUsb = false; }
                    ui.post(MainActivity.this::renderUsbButton);
                    s.setTcpNoDelay(true);
                    startRtsp();
                    // Buffered: the hello goes out in one piece (matters over adb); every write below flushes.
                    DataOutputStream out = new DataOutputStream(new BufferedOutputStream(s.getOutputStream(), 8192));
                    this.out = out;
                    pcTalksBack = false;
                    Thread reader = new Thread(() -> readLoop(s), "link-read");
                    reader.setDaemon(true);
                    reader.start();
                    byte[] ip = s.getLocalAddress().getAddress();
                    for (int i = 0; i < 4; i++) out.writeInt(ip.length == 4 ? (ip[i] & 0xFF) : 0);
                    out.writeInt(rtspPort);
                    out.writeInt(NAME_MAGIC);
                    out.writeInt(name.length);
                    out.write(name);
                    out.flush();
                    attempt = 0;
                    post(LinkState.WAITING_PC);
                    if (rtsp != null && rtsp.isStreaming()) post(LinkState.LIVE);
                    // Heartbeat twice a second; 'F' asks the PC to switch camera.
                    // Every 2 s, check whether the USB cable came or went.
                    int tick = 0;
                    while (wanted.get()) {
                        synchronized (writeLock) {
                            out.write(flip.getAndSet(false) ? 'F' : 1);
                            out.flush();
                        }
                        Thread.sleep(500);
                        if (++tick % 4 == 0 && shouldMove(s)) { moving = true; break; }
                    }
                } catch (IOException e) {
                    // fall through to retry
                } catch (InterruptedException e) {
                    break;
                } finally {
                    Socket s = socket;
                    if (s != null) try { s.close(); } catch (IOException ignored) { }
                    socket = null;
                    out = null;
                    pcTalksBack = false;
                }
                if (!wanted.get()) break;
                stopRtsp();
                if (moving) { attempt = 0; continue; }
                attempt++;
                post(LinkState.RECONNECTING);
                try { Thread.sleep(Math.min(5000, 1000L * attempt)); } catch (InterruptedException e) { break; }
            }
            stopRtsp();
        }
    }
}

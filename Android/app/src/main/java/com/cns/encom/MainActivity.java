package com.cns.encom;

import android.Manifest;
import android.app.Activity;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.content.res.ColorStateList;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.preference.PreferenceManager;
import android.provider.Settings;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputMethodManager;
import android.widget.Button;
import android.widget.EditText;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;

import net.majorkernelpanic.streaming.SessionBuilder;
import net.majorkernelpanic.streaming.gl.SurfaceView;
import net.majorkernelpanic.streaming.rtsp.RtspServer;

import java.io.DataOutputStream;
import java.io.IOException;
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
    private Button grant;

    private LinkState state = LinkState.IDLE;
    private String connectedName = "";
    private int rtspPort;

    // discovery
    private static class Pc { String name; String host; long lastSeen; }
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

        // Our own key: libstreaming owns "rtsp_port" and stores it as a String.
        rtspPort = prefs.getInt("ocam_rtsp_port", 0);
        if (rtspPort < 1024) rtspPort = 20000 + new Random().nextInt(10000);
        // Write the library's copy before the service can be bound and created.
        prefs.edit().remove(RtspServer.KEY_PORT)
                .putInt("ocam_rtsp_port", rtspPort)
                .putString(RtspServer.KEY_PORT, String.valueOf(rtspPort)).commit();
        address.setText(prefs.getString("last_host", ""));

        SurfaceView preview = findViewById(R.id.preview);
        SessionBuilder.getInstance()
                .setSurfaceView(preview)
                .setPreviewOrientation(90)
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
        stopDiscovery();
    }

    @Override
    protected void onDestroy() {
        disconnect();
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
            case LIVE:         dot = R.color.good;   chip = "Live on " + connectedName; break;
            case RECONNECTING: dot = R.color.warn;   chip = "Connection lost – reconnecting…"; break;
            case NO_PERMISSION:dot = R.color.bad;    chip = "Camera access needed"; break;
            default:           dot = R.color.muted;  chip = "Not connected"; break;
        }
        statusText.setText(chip);
        statusDot.setImageTintList(ColorStateList.valueOf(color(dot)));

        final boolean connected = s == LinkState.CONNECTING || s == LinkState.WAITING_PC || s == LinkState.LIVE || s == LinkState.RECONNECTING;
        pickPanel.setVisibility(!connected && s != LinkState.NO_PERMISSION ? View.VISIBLE : View.GONE);
        livePanel.setVisibility(connected ? View.VISIBLE : View.GONE);
        grant.setVisibility(s == LinkState.NO_PERMISSION ? View.VISIBLE : View.GONE);

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
        for (Pc pc : pcs) {
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
            sub.setText(pc.host.equals(last) ? pc.host + "  ·  " + getString(R.string.last_used) : pc.host);
            sub.setTextColor(color(R.color.muted));
            sub.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
            row.addView(name);
            row.addView(sub);
            row.setContentDescription("Connect to " + pc.name);
            row.setOnClickListener(v -> connect(pc.host, pc.name));
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
                    pruneStale(now);
                }
                try {
                    DatagramPacket p = new DatagramPacket(buf, buf.length);
                    sock.receive(p);
                    String msg = new String(p.getData(), 0, p.getLength(), StandardCharsets.UTF_8);
                    if (msg.startsWith("OPENCAMERA1|")) {
                        String[] parts = msg.split("\\|");
                        Pc pc = new Pc();
                        pc.host = p.getAddress().getHostAddress();
                        pc.name = parts.length > 1 && !parts[1].isEmpty() ? parts[1] : pc.host;
                        pc.lastSeen = System.currentTimeMillis();
                        boolean isNew = found.put(pc.host, pc) == null;
                        if (isNew) ui.post(() -> { renderPcList(); maybeAutoConnect(pc); });
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
            while (it.hasNext()) if (now - it.next().lastSeen > 6000) { it.remove(); changed = true; }
        }
        if (changed) ui.post(this::renderPcList);
    }

    // Reconnect to the PC used last time without asking, once per app start.
    private void maybeAutoConnect(Pc pc) {
        if (!autoConnectAllowed || state != LinkState.IDLE || link != null) return;
        if (pc.host.equals(prefs.getString("last_host", "")) && prefs.getBoolean("auto_connect", true)) {
            autoConnectAllowed = false;
            connect(pc.host, pc.name);
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
        prefs.edit().putString("last_host", host).putBoolean("auto_connect", true).apply();
        link = new Link(host);
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
        startService(new Intent(this, RtspServer.class));
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

    /** Keeps the control link to one PC alive, reconnecting until shutdown(). */
    private class Link extends Thread {
        private final String host;
        private final AtomicBoolean wanted = new AtomicBoolean(true);
        private final AtomicBoolean flip = new AtomicBoolean(false);
        private volatile Socket socket;

        Link(String host) {
            super("link");
            this.host = host;
        }

        void requestFlip() { flip.set(true); }

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
                try {
                    Socket s = new Socket();
                    socket = s;
                    s.connect(new InetSocketAddress(host, CONTROL_PORT), 3000);
                    s.setTcpNoDelay(true);
                    startRtsp();
                    DataOutputStream out = new DataOutputStream(s.getOutputStream());
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
                    while (wanted.get()) {
                        out.write(flip.getAndSet(false) ? 'F' : 1);
                        out.flush();
                        Thread.sleep(500);
                    }
                } catch (IOException e) {
                    // fall through to retry
                } catch (InterruptedException e) {
                    break;
                } finally {
                    Socket s = socket;
                    if (s != null) try { s.close(); } catch (IOException ignored) { }
                    socket = null;
                }
                if (!wanted.get()) break;
                stopRtsp();
                attempt++;
                post(LinkState.RECONNECTING);
                try { Thread.sleep(Math.min(5000, 1000L * attempt)); } catch (InterruptedException e) { break; }
            }
            stopRtsp();
        }
    }
}

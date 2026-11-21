package com.cns.encom;

import android.annotation.SuppressLint;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.graphics.Rect;
import android.graphics.SurfaceTexture;
import android.hardware.camera2.CameraAccessException;
import android.hardware.camera2.CameraCaptureSession;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraDevice;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.CaptureRequest;
import android.hardware.camera2.CaptureResult;
import android.hardware.camera2.TotalCaptureResult;
import android.hardware.camera2.params.ColorSpaceTransform;
import android.hardware.camera2.params.MeteringRectangle;
import android.hardware.camera2.params.RggbChannelVector;
import android.hardware.camera2.params.StreamConfigurationMap;
import android.os.BatteryManager;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.SystemClock;
import android.util.Range;
import android.util.Rational;
import android.util.Size;
import android.view.OrientationEventListener;
import android.view.Surface;

import net.majorkernelpanic.streaming.video.Camera2H264Stream;

import java.io.IOException;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.TreeSet;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * The phone camera, driven with Camera2 so the PC can use "pro" controls
 * (ISO, shutter, white balance in Kelvin, manual focus, zoom, torch,
 * stabilisation, flicker) while the video streams.
 *
 * Frames go straight from the camera into the H.264 encoder's input surface,
 * plus (optionally) a preview on the phone screen.
 *
 * All camera calls run on one background thread ({@link #h}).
 */
final class CameraEngine implements Camera2H264Stream.Source {

    interface Listener {
        void onCaps(String caps);
        void onStatus(String status);
        void onPreviewGeometry(int width, int height, boolean front);
        void onRunning(boolean running);
    }

    static final CameraEngine INSTANCE = new CameraEngine();

    private Context app;
    private CameraManager mgr;
    private Handler h;
    private volatile Listener listener;

    // ---- current camera session (camera thread only)
    private CameraDevice dev;
    private CameraCaptureSession ses;
    private CameraCharacteristics ch;
    private Surface encSurface, prevSurface;
    private int width, height, fps;
    private boolean front;
    private int sensorOrientation;

    // capabilities of the open camera
    private boolean manualSensor, manualPost, hasFlash, hasOis, hasEis, hasContinuousVideoAf;
    private Range<Integer> isoRange;
    private Range<Long> expRange;
    private Range<Integer> evRange;
    private float evStep = 1f / 3f;
    private float minFocus; // diopters, 0 = fixed focus
    private float zoomMin = 1f, zoomMax = 1f;
    private boolean zoomRatioApi;
    private Rect activeArray;
    private int maxAfRegions, maxAeRegions;

    // ---- settings sent by the PC (any thread, guarded by `pro`)
    private final Map<String, String> pro = new HashMap<>();
    private float pointX = -1, pointY = -1;
    private float lockedFocus = -1;

    // ---- last values reported by the camera
    private volatile int rIso;
    private volatile long rExp;
    private volatile float rFocus = -1;
    private volatile int rAf = -1;
    private volatile int rAe = -1;
    private ColorSpaceTransform autoCcm;
    private long lastStatus, lastBattery;
    private int battery = -1, batteryTemp = -1;
    private boolean charging;

    // ---- phone orientation
    private volatile int deviceAngle = -1;
    private volatile int snapped = 0;

    // ---- phone settings
    private volatile SurfaceTexture previewTexture;
    volatile boolean showPreview = true;
    volatile float bitrateScale = 1f;
    volatile boolean reportOrientation = true;

    private CameraEngine() { }

    void init(Context context) {
        if (app != null) return;
        app = context.getApplicationContext();
        mgr = (CameraManager) app.getSystemService(Context.CAMERA_SERVICE);
        HandlerThread t = new HandlerThread("camera");
        t.start();
        h = new Handler(t.getLooper());
        OrientationEventListener o = new OrientationEventListener(app) {
            @Override public void onOrientationChanged(int angle) {
                deviceAngle = angle;
                if (angle < 0) return;
                // Snap to 0/90/180/270 with some hysteresis so it does not flicker at 45 deg.
                int d = Math.abs(angle - snapped) % 360;
                if (d > 180) d = 360 - d;
                if (d > 55) snapped = ((angle + 45) / 90 * 90) % 360;
            }
        };
        if (o.canDetectOrientation()) o.enable();
        Camera2H264Stream.setSource(this);
    }

    void setListener(Listener l) { listener = l; }

    /** Only the screen that registered the listener may remove it. */
    void clearListener(Listener l) { if (listener == l) listener = null; }

    // ------------------------------------------------------------------ stream source
    @Override
    public int bitrateFor(int requested) {
        return Math.max(300_000, (int) (requested * bitrateScale));
    }

    @Override
    public synchronized void start(boolean wantFront, int w, int hgt, int framerate, Surface encoderInput) throws IOException {
        if (h == null) throw new IOException("Camera engine not ready");
        final CountDownLatch done = new CountDownLatch(1);
        final String[] error = new String[1];
        h.post(() -> {
            try {
                open(wantFront, w, hgt, framerate, encoderInput, done, error);
            } catch (CameraAccessException | SecurityException | IllegalArgumentException e) {
                error[0] = e.getMessage() == null ? e.toString() : e.getMessage();
                done.countDown();
            }
        });
        boolean ok;
        try {
            ok = done.await(6, TimeUnit.SECONDS);
        } catch (InterruptedException e) {
            ok = false;
        }
        if (!ok || error[0] != null) {
            stop();
            throw new IOException("Camera did not start: " + (error[0] != null ? error[0] : "timeout"));
        }
    }

    @Override
    public synchronized void stop() {
        if (h == null) return;
        final CountDownLatch done = new CountDownLatch(1);
        h.post(() -> { closeCamera(); done.countDown(); });
        try { done.await(2, TimeUnit.SECONDS); } catch (InterruptedException ignored) { }
    }

    // ------------------------------------------------------------------ preview (UI thread)
    /** The phone-screen preview appeared or went away; rebuild the session around it. */
    void setPreviewTexture(SurfaceTexture st) {
        previewTexture = st;
        reconfigure(st == null);
    }

    void setShowPreview(boolean show) {
        showPreview = show;
        reconfigure(false);
    }

    private void reconfigure(boolean wait) {
        if (h == null) return;
        final CountDownLatch done = new CountDownLatch(1);
        h.post(() -> {
            if (dev != null && encSurface != null) createSession(null, null);
            done.countDown();
        });
        if (wait) try { done.await(800, TimeUnit.MILLISECONDS); } catch (InterruptedException ignored) { }
    }

    // ------------------------------------------------------------------ settings from the PC
    void applyFromPc(Map<String, String> values) {
        synchronized (pro) {
            final boolean afLockBefore = "1".equals(pro.get("afl"));
            pro.putAll(values);
            if (!afLockBefore && "1".equals(pro.get("afl"))) lockedFocus = rFocus;
        }
        if (h != null) h.post(this::applyRequest);
    }

    /** Point of interest for focus/exposure in stream coordinates 0..1, or -1 to clear. */
    void setPoint(float x, float y) {
        synchronized (pro) { pointX = x; pointY = y; }
        if (h != null) h.post(this::applyRequest);
    }

    /** Called when the user flips the torch on the phone itself. */
    boolean toggleTorchLocal() {
        boolean on;
        synchronized (pro) {
            on = !"1".equals(pro.get("torch"));
            pro.put("torch", on ? "1" : "0");
        }
        if (h != null) h.post(this::applyRequest);
        return on;
    }

    boolean torchOn() {
        synchronized (pro) { return "1".equals(pro.get("torch")); }
    }

    private String get(String k, String def) {
        synchronized (pro) { String v = pro.get(k); return v == null ? def : v; }
    }

    private int getInt(String k, int def) {
        try { return Integer.parseInt(get(k, String.valueOf(def))); } catch (NumberFormatException e) { return def; }
    }

    private long getLong(String k, long def) {
        try { return Long.parseLong(get(k, String.valueOf(def))); } catch (NumberFormatException e) { return def; }
    }

    private float getFloat(String k, float def) {
        try { return Float.parseFloat(get(k, String.valueOf(def))); } catch (NumberFormatException e) { return def; }
    }

    // ------------------------------------------------------------------ camera thread
    @SuppressLint("MissingPermission")
    private void open(boolean wantFront, int w, int hgt, int framerate, Surface enc,
                      CountDownLatch done, String[] error) throws CameraAccessException {
        closeCamera();
        String id = null;
        for (String c : mgr.getCameraIdList()) {
            Integer facing = mgr.getCameraCharacteristics(c).get(CameraCharacteristics.LENS_FACING);
            if (facing == null) continue;
            boolean isFront = facing == CameraCharacteristics.LENS_FACING_FRONT;
            if (isFront == wantFront) { id = c; break; }
        }
        if (id == null) {
            String[] all = mgr.getCameraIdList();
            if (all.length == 0) throw new CameraAccessException(CameraAccessException.CAMERA_ERROR, "no camera");
            id = all[0];
        }
        ch = mgr.getCameraCharacteristics(id);
        readCapabilities();
        front = wantFront;
        width = w;
        height = hgt;
        fps = framerate;
        encSurface = enc;
        autoCcm = null;
        mgr.openCamera(id, new CameraDevice.StateCallback() {
            @Override public void onOpened(CameraDevice d) {
                dev = d;
                createSession(done, error);
            }
            @Override public void onDisconnected(CameraDevice d) {
                d.close();
                if (dev == d) dev = null;
                fail(done, error, "camera taken by another app");
            }
            @Override public void onError(CameraDevice d, int code) {
                d.close();
                if (dev == d) dev = null;
                fail(done, error, "camera error " + code);
            }
        }, h);
    }

    private static void fail(CountDownLatch done, String[] error, String why) {
        if (done == null) return;
        if (error != null) error[0] = why;
        done.countDown();
    }

    private void createSession(final CountDownLatch done, final String[] error) {
        if (dev == null || encSurface == null) { fail(done, error, "camera closed"); return; }
        if (ses != null) {
            try { ses.close(); } catch (Exception ignored) { }
            ses = null;
        }
        if (prevSurface != null) { prevSurface.release(); prevSurface = null; }
        List<Surface> outs = new ArrayList<>();
        outs.add(encSurface);
        final SurfaceTexture st = showPreview ? previewTexture : null;
        if (st != null) {
            st.setDefaultBufferSize(width, height);
            prevSurface = new Surface(st);
            outs.add(prevSurface);
        }
        try {
            dev.createCaptureSession(outs, new CameraCaptureSession.StateCallback() {
                @Override public void onConfigured(CameraCaptureSession s) {
                    ses = s;
                    applyRequest();
                    Listener l = listener;
                    if (l != null) {
                        l.onCaps(caps());
                        l.onPreviewGeometry(width, height, front);
                        l.onRunning(true);
                    }
                    if (done != null) done.countDown();
                }
                @Override public void onConfigureFailed(CameraCaptureSession s) {
                    if (prevSurface != null) {
                        // Some phones refuse the extra preview output: stream without it.
                        prevSurface.release();
                        prevSurface = null;
                        previewTexture = null;
                        createSession(done, error);
                    } else {
                        fail(done, error, "camera refused " + width + "x" + height);
                    }
                }
            }, h);
        } catch (CameraAccessException | IllegalArgumentException | IllegalStateException e) {
            fail(done, error, String.valueOf(e.getMessage()));
        }
    }

    private void closeCamera() {
        if (ses != null) {
            try { ses.close(); } catch (Exception ignored) { }
            ses = null;
        }
        if (dev != null) {
            dev.close();
            dev = null;
        }
        if (prevSurface != null) {
            prevSurface.release();
            prevSurface = null;
        }
        boolean wasRunning = encSurface != null;
        encSurface = null;
        Listener l = listener;
        if (wasRunning && l != null) l.onRunning(false);
    }

    private void readCapabilities() {
        sensorOrientation = orZero(ch.get(CameraCharacteristics.SENSOR_ORIENTATION));
        int[] caps = ch.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES);
        manualSensor = manualPost = false;
        if (caps != null) for (int c : caps) {
            if (c == CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR) manualSensor = true;
            if (c == CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_MANUAL_POST_PROCESSING) manualPost = true;
        }
        isoRange = ch.get(CameraCharacteristics.SENSOR_INFO_SENSITIVITY_RANGE);
        expRange = ch.get(CameraCharacteristics.SENSOR_INFO_EXPOSURE_TIME_RANGE);
        if (isoRange == null || expRange == null) manualSensor = false;
        evRange = ch.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_RANGE);
        Rational step = ch.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_STEP);
        evStep = step != null && step.getDenominator() != 0 ? step.floatValue() : 1f / 3f;
        Float mf = ch.get(CameraCharacteristics.LENS_INFO_MINIMUM_FOCUS_DISTANCE);
        minFocus = mf == null ? 0 : mf;
        Boolean flash = ch.get(CameraCharacteristics.FLASH_INFO_AVAILABLE);
        hasFlash = flash != null && flash;
        hasOis = contains(ch.get(CameraCharacteristics.LENS_INFO_AVAILABLE_OPTICAL_STABILIZATION),
                CameraCharacteristics.LENS_OPTICAL_STABILIZATION_MODE_ON);
        hasEis = contains(ch.get(CameraCharacteristics.CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES),
                CameraCharacteristics.CONTROL_VIDEO_STABILIZATION_MODE_ON);
        hasContinuousVideoAf = contains(ch.get(CameraCharacteristics.CONTROL_AF_AVAILABLE_MODES),
                CameraCharacteristics.CONTROL_AF_MODE_CONTINUOUS_VIDEO);
        activeArray = ch.get(CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE);
        maxAfRegions = orZero(ch.get(CameraCharacteristics.CONTROL_MAX_REGIONS_AF));
        maxAeRegions = orZero(ch.get(CameraCharacteristics.CONTROL_MAX_REGIONS_AE));
        zoomRatioApi = false;
        zoomMin = 1f;
        Float maxDigital = ch.get(CameraCharacteristics.SCALER_AVAILABLE_MAX_DIGITAL_ZOOM);
        zoomMax = maxDigital == null ? 1f : maxDigital;
        if (Build.VERSION.SDK_INT >= 30) {
            Range<Float> zr = ch.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE);
            if (zr != null) {
                zoomRatioApi = true;
                zoomMin = zr.getLower();
                zoomMax = zr.getUpper();
            }
        }
    }

    private static int orZero(Integer v) { return v == null ? 0 : v; }

    private static boolean contains(int[] list, int v) {
        if (list == null) return false;
        for (int x : list) if (x == v) return true;
        return false;
    }

    /** Builds the repeating request from the PC's settings. Camera thread. */
    private void applyRequest() {
        if (dev == null || ses == null) return;
        try {
            CaptureRequest.Builder b = dev.createCaptureRequest(CameraDevice.TEMPLATE_RECORD);
            b.addTarget(encSurface);
            if (prevSurface != null) b.addTarget(prevSurface);
            b.set(CaptureRequest.CONTROL_MODE, CaptureRequest.CONTROL_MODE_AUTO);
            Range<Integer> fr = bestFpsRange(fps);
            if (fr != null) b.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, fr);

            String flicker = get("flicker", "auto");
            b.set(CaptureRequest.CONTROL_AE_ANTIBANDING_MODE,
                    flicker.equals("50") ? CaptureRequest.CONTROL_AE_ANTIBANDING_MODE_50HZ
                  : flicker.equals("60") ? CaptureRequest.CONTROL_AE_ANTIBANDING_MODE_60HZ
                  : flicker.equals("off") ? CaptureRequest.CONTROL_AE_ANTIBANDING_MODE_OFF
                  : CaptureRequest.CONTROL_AE_ANTIBANDING_MODE_AUTO);

            // Exposure: ISO / shutter (manual) or auto with EV and AE lock.
            int iso = getInt("iso", 0);
            long exp = getLong("exp", 0);
            final boolean manualExposure = manualSensor && (iso > 0 || exp > 0);
            if (manualExposure) {
                if (iso <= 0) iso = rIso > 0 ? rIso : 400;
                if (exp <= 0) exp = rExp > 0 ? rExp : 1_000_000_000L / Math.max(1, fps * 2);
                iso = isoRange.clamp(iso);
                exp = expRange.clamp(exp);
                b.set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_OFF);
                b.set(CaptureRequest.SENSOR_SENSITIVITY, iso);
                b.set(CaptureRequest.SENSOR_EXPOSURE_TIME, exp);
                b.set(CaptureRequest.SENSOR_FRAME_DURATION, Math.max(1_000_000_000L / Math.max(1, fps), exp));
            } else {
                b.set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_ON);
                if (evRange != null) b.set(CaptureRequest.CONTROL_AE_EXPOSURE_COMPENSATION, evRange.clamp(getInt("ev", 0)));
                b.set(CaptureRequest.CONTROL_AE_LOCK, "1".equals(get("ael", "0")));
            }
            b.set(CaptureRequest.FLASH_MODE, hasFlash && "1".equals(get("torch", "0"))
                    ? CaptureRequest.FLASH_MODE_TORCH : CaptureRequest.FLASH_MODE_OFF);

            // White balance.
            String wb = get("wb", "auto");
            RggbChannelVector gains = wb.equals("k") && manualPost ? kelvinGains(getInt("k", 5500)) : null;
            if (gains != null) {
                b.set(CaptureRequest.CONTROL_AWB_MODE, CaptureRequest.CONTROL_AWB_MODE_OFF);
                b.set(CaptureRequest.COLOR_CORRECTION_MODE, CaptureRequest.COLOR_CORRECTION_MODE_TRANSFORM_MATRIX);
                b.set(CaptureRequest.COLOR_CORRECTION_GAINS, gains);
                if (autoCcm != null) b.set(CaptureRequest.COLOR_CORRECTION_TRANSFORM, autoCcm);
            } else {
                int mode = wb.equals("k") ? presetForKelvin(getInt("k", 5500)) : awbMode(wb);
                b.set(CaptureRequest.CONTROL_AWB_MODE, mode);
                b.set(CaptureRequest.CONTROL_AWB_LOCK, mode == CaptureRequest.CONTROL_AWB_MODE_AUTO && "1".equals(get("awbl", "0")));
            }

            // Focus.
            String af = get("af", "c");
            float lock;
            synchronized (pro) { lock = lockedFocus; }
            if (af.equals("m") && minFocus > 0) {
                b.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_OFF);
                b.set(CaptureRequest.LENS_FOCUS_DISTANCE, Math.max(0f, Math.min(minFocus, getFloat("fd", 0f))));
            } else if ("1".equals(get("afl", "0")) && minFocus > 0 && lock >= 0) {
                b.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_OFF);
                b.set(CaptureRequest.LENS_FOCUS_DISTANCE, lock);
            } else {
                b.set(CaptureRequest.CONTROL_AF_MODE, hasContinuousVideoAf
                        ? CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO : CaptureRequest.CONTROL_AF_MODE_AUTO);
            }

            // Zoom (the zoom-ratio API also reaches ultra-wide lenses below 1x).
            float zoom = getFloat("zoom", 1f);
            zoom = Math.max(zoomMin, Math.min(zoomMax, zoom));
            Rect crop = null;
            if (zoomRatioApi && Build.VERSION.SDK_INT >= 30) {
                b.set(CaptureRequest.CONTROL_ZOOM_RATIO, zoom);
            } else if (zoom > 1.01f && activeArray != null) {
                int cw = (int) (activeArray.width() / zoom), chh = (int) (activeArray.height() / zoom);
                int cx = activeArray.centerX(), cy = activeArray.centerY();
                crop = new Rect(cx - cw / 2, cy - chh / 2, cx + cw / 2, cy + chh / 2);
                b.set(CaptureRequest.SCALER_CROP_REGION, crop);
            }

            // Point of interest (click on the PC preview).
            float px, py;
            synchronized (pro) { px = pointX; py = pointY; }
            if (px >= 0 && py >= 0 && activeArray != null) {
                MeteringRectangle[] r = { region(px, py, crop) };
                if (maxAfRegions > 0) b.set(CaptureRequest.CONTROL_AF_REGIONS, r);
                if (maxAeRegions > 0 && !manualExposure) b.set(CaptureRequest.CONTROL_AE_REGIONS, r);
            }

            if (hasOis) b.set(CaptureRequest.LENS_OPTICAL_STABILIZATION_MODE, "0".equals(get("ois", "1"))
                    ? CaptureRequest.LENS_OPTICAL_STABILIZATION_MODE_OFF : CaptureRequest.LENS_OPTICAL_STABILIZATION_MODE_ON);
            if (hasEis) b.set(CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE, "1".equals(get("eis", "0"))
                    ? CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE_ON : CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE_OFF);

            ses.setRepeatingRequest(b.build(), captureCallback, h);
        } catch (CameraAccessException | IllegalStateException | IllegalArgumentException e) {
            // Session went away meanwhile; the next start rebuilds everything.
        }
    }

    /** Metering box around a point given in stream-frame coordinates (0..1). */
    private MeteringRectangle region(float x, float y, Rect crop) {
        // The 16:9 (or 4:3) stream is a centred cut of the sensor area.
        Rect area = crop != null ? crop : new Rect(0, 0, activeArray.width(), activeArray.height());
        float aw = area.width(), ah = area.height();
        float out = width / (float) height, arr = aw / ah;
        float vw = aw, vh = ah;
        if (out > arr) vh = aw / out; else vw = ah * out;
        float left = area.left + (aw - vw) / 2f, top = area.top + (ah - vh) / 2f;
        int cx = (int) (left + x * vw), cy = (int) (top + y * vh);
        int half = (int) (Math.min(vw, vh) * 0.08f);
        int l = Math.max(area.left, cx - half), t = Math.max(area.top, cy - half);
        int r = Math.min(area.right - 1, cx + half), bt = Math.min(area.bottom - 1, cy + half);
        return new MeteringRectangle(l, t, Math.max(1, r - l), Math.max(1, bt - t), MeteringRectangle.METERING_WEIGHT_MAX - 1);
    }

    private Range<Integer> bestFpsRange(int want) {
        Range<Integer>[] ranges = ch.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES);
        if (ranges == null) return null;
        Range<Integer> best = null;
        for (Range<Integer> r : ranges) {
            if (r.getUpper() != want) continue;
            if (best == null || r.getLower() > best.getLower()) best = r; // fixed [30,30] beats [15,30]
        }
        if (best != null) return best;
        for (Range<Integer> r : ranges) {
            if (r.getUpper() >= want && (best == null || r.getUpper() < best.getUpper())) best = r;
        }
        return best;
    }

    private static int awbMode(String wb) {
        switch (wb) {
            case "daylight": return CaptureRequest.CONTROL_AWB_MODE_DAYLIGHT;
            case "cloudy": return CaptureRequest.CONTROL_AWB_MODE_CLOUDY_DAYLIGHT;
            case "shade": return CaptureRequest.CONTROL_AWB_MODE_SHADE;
            case "tungsten": return CaptureRequest.CONTROL_AWB_MODE_INCANDESCENT;
            case "fluorescent": return CaptureRequest.CONTROL_AWB_MODE_FLUORESCENT;
            default: return CaptureRequest.CONTROL_AWB_MODE_AUTO;
        }
    }

    /** Phones without manual colour: nearest preset instead of an exact Kelvin. */
    private static int presetForKelvin(int k) {
        if (k < 3400) return CaptureRequest.CONTROL_AWB_MODE_INCANDESCENT;
        if (k < 4800) return CaptureRequest.CONTROL_AWB_MODE_FLUORESCENT;
        if (k < 6000) return CaptureRequest.CONTROL_AWB_MODE_DAYLIGHT;
        if (k < 7000) return CaptureRequest.CONTROL_AWB_MODE_CLOUDY_DAYLIGHT;
        return CaptureRequest.CONTROL_AWB_MODE_SHADE;
    }

    /**
     * White-balance gains for a light of `kelvin`, using the sensor's own
     * calibration (DNG-style colour matrices). Returns null if the phone does
     * not publish them; the caller then falls back to the nearest preset.
     */
    private RggbChannelVector kelvinGains(int kelvin) {
        ColorSpaceTransform cm1 = ch.get(CameraCharacteristics.SENSOR_COLOR_TRANSFORM1);
        ColorSpaceTransform cm2 = ch.get(CameraCharacteristics.SENSOR_COLOR_TRANSFORM2);
        if (cm1 == null) return null;
        if (cm2 == null) cm2 = cm1;
        ColorSpaceTransform cc1 = ch.get(CameraCharacteristics.SENSOR_CALIBRATION_TRANSFORM1);
        ColorSpaceTransform cc2 = ch.get(CameraCharacteristics.SENSOR_CALIBRATION_TRANSFORM2);
        if (cc2 == null) cc2 = cc1;
        double t1 = illuminantKelvin(ch.get(CameraCharacteristics.SENSOR_REFERENCE_ILLUMINANT1), 2856);
        Byte ill2 = ch.get(CameraCharacteristics.SENSOR_REFERENCE_ILLUMINANT2);
        double t2 = ill2 == null ? t1 : illuminantKelvin(ill2.intValue(), 6504);
        double k = Math.max(2000, Math.min(12000, kelvin));
        // interpolate in inverse temperature, like DNG
        double g = (t1 == t2) ? 1 : (1.0 / k - 1.0 / t2) / (1.0 / t1 - 1.0 / t2);
        g = Math.max(0, Math.min(1, g));
        double[] xyz = whiteXyz(k);
        double[] m = new double[9];
        for (int i = 0; i < 9; i++) {
            double a = el(cm1, i), c = el(cm2, i);
            m[i] = g * a + (1 - g) * c;
        }
        double[] cam = mul(m, xyz);
        if (cc1 != null) {
            double[] cc = new double[9];
            for (int i = 0; i < 9; i++) cc[i] = g * el(cc1, i) + (1 - g) * el(cc2, i);
            cam = mul(cc, cam);
        }
        if (cam[0] <= 1e-6 || cam[1] <= 1e-6 || cam[2] <= 1e-6) return null;
        float r = (float) (cam[1] / cam[0]), bl = (float) (cam[1] / cam[2]);
        float min = Math.min(1f, Math.min(r, bl));
        return new RggbChannelVector(r / min, 1f / min, 1f / min, bl / min);
    }

    private static double el(ColorSpaceTransform t, int i) {
        Rational v = t.getElement(i % 3, i / 3);
        return v.doubleValue();
    }

    private static double[] mul(double[] m, double[] v) {
        return new double[] {
                m[0] * v[0] + m[1] * v[1] + m[2] * v[2],
                m[3] * v[0] + m[4] * v[1] + m[5] * v[2],
                m[6] * v[0] + m[7] * v[1] + m[8] * v[2] };
    }

    /** CIE XYZ (Y=1) of a Planckian light, Kim et al. approximation. */
    private static double[] whiteXyz(double t) {
        double x;
        if (t <= 4000) x = -0.2661239e9 / (t * t * t) - 0.2343589e6 / (t * t) + 0.8776956e3 / t + 0.179910;
        else x = -3.0258469e9 / (t * t * t) + 2.1070379e6 / (t * t) + 0.2226347e3 / t + 0.240390;
        double y;
        if (t <= 2222) y = -1.1063814 * x * x * x - 1.34811020 * x * x + 2.18555832 * x - 0.20219683;
        else if (t <= 4000) y = -0.9549476 * x * x * x - 1.37418593 * x * x + 2.09137015 * x - 0.16748867;
        else y = 3.0817580 * x * x * x - 5.87338670 * x * x + 3.75112997 * x - 0.37001483;
        return new double[] { x / y, 1, (1 - x - y) / y };
    }

    private static double illuminantKelvin(Integer illuminant, double def) {
        if (illuminant == null) return def;
        switch (illuminant) {
            case CameraCharacteristics.SENSOR_REFERENCE_ILLUMINANT1_STANDARD_A:
            case CameraCharacteristics.SENSOR_REFERENCE_ILLUMINANT1_TUNGSTEN: return 2856;
            case CameraCharacteristics.SENSOR_REFERENCE_ILLUMINANT1_D50: return 5003;
            case CameraCharacteristics.SENSOR_REFERENCE_ILLUMINANT1_D55: return 5503;
            case CameraCharacteristics.SENSOR_REFERENCE_ILLUMINANT1_D65:
            case CameraCharacteristics.SENSOR_REFERENCE_ILLUMINANT1_DAYLIGHT: return 6504;
            case CameraCharacteristics.SENSOR_REFERENCE_ILLUMINANT1_D75: return 7504;
            case CameraCharacteristics.SENSOR_REFERENCE_ILLUMINANT1_COOL_WHITE_FLUORESCENT: return 4150;
            case CameraCharacteristics.SENSOR_REFERENCE_ILLUMINANT1_STANDARD_B: return 4874;
            case CameraCharacteristics.SENSOR_REFERENCE_ILLUMINANT1_STANDARD_C: return 6774;
            default: return def;
        }
    }

    private final CameraCaptureSession.CaptureCallback captureCallback = new CameraCaptureSession.CaptureCallback() {
        @Override
        public void onCaptureCompleted(CameraCaptureSession session, CaptureRequest request, TotalCaptureResult r) {
            Integer iso = r.get(CaptureResult.SENSOR_SENSITIVITY);
            Long exp = r.get(CaptureResult.SENSOR_EXPOSURE_TIME);
            Float fd = r.get(CaptureResult.LENS_FOCUS_DISTANCE);
            Integer af = r.get(CaptureResult.CONTROL_AF_STATE);
            Integer ae = r.get(CaptureResult.CONTROL_AE_STATE);
            Integer awb = r.get(CaptureResult.CONTROL_AWB_MODE);
            if (iso != null) rIso = iso;
            if (exp != null) rExp = exp;
            if (fd != null) rFocus = fd;
            rAf = af == null ? -1 : af;
            rAe = ae == null ? -1 : ae;
            if (awb != null && awb == CaptureResult.CONTROL_AWB_MODE_AUTO) {
                ColorSpaceTransform ccm = r.get(CaptureResult.COLOR_CORRECTION_TRANSFORM);
                if (ccm != null) autoCcm = ccm;
            }
            long now = SystemClock.uptimeMillis();
            if (now - lastStatus >= 250) {
                lastStatus = now;
                Listener l = listener;
                if (l != null) l.onStatus(status(now));
            }
        }
    };

    /** Clockwise degrees the PC must turn the picture to make it upright. */
    private int uprightRotation() {
        int a = snapped;
        return front ? (sensorOrientation - a + 360) % 360 : (sensorOrientation + a) % 360;
    }

    private String status(long now) {
        if (now - lastBattery > 5000 || battery < 0) {
            lastBattery = now;
            Intent bi = app.registerReceiver(null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
            if (bi != null) {
                int lvl = bi.getIntExtra(BatteryManager.EXTRA_LEVEL, -1), scale = bi.getIntExtra(BatteryManager.EXTRA_SCALE, 100);
                battery = lvl >= 0 && scale > 0 ? lvl * 100 / scale : -1;
                batteryTemp = bi.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, -10) / 10;
                int plugged = bi.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0);
                charging = plugged != 0;
            }
        }
        StringBuilder s = new StringBuilder(160);
        s.append("iso=").append(rIso).append(" exp=").append(rExp)
         .append(" fd=").append(String.format(Locale.US, "%.3f", rFocus))
         .append(" af=").append(rAf).append(" ae=").append(rAe)
         .append(" torch=").append(torchOn() ? 1 : 0)
         .append(" bat=").append(battery).append(" temp=").append(batteryTemp).append(" chg=").append(charging ? 1 : 0);
        int angle = deviceAngle;
        if (reportOrientation && angle >= 0) {
            int lvl = angle - snapped;
            if (lvl > 180) lvl -= 360;
            if (lvl < -180) lvl += 360;
            s.append(" rot=").append(uprightRotation()).append(" level=").append(lvl);
        } else {
            s.append(" rot=-1");
        }
        return s.toString();
    }

    /** What the open camera can do, for the PC's pro controls. */
    private String caps() {
        StringBuilder s = new StringBuilder(256);
        s.append("front=").append(front ? 1 : 0)
         .append(" manual=").append(manualSensor ? 1 : 0)
         .append(" wbk=").append(manualPost && ch.get(CameraCharacteristics.SENSOR_COLOR_TRANSFORM1) != null ? 1 : 0);
        if (isoRange != null) s.append(" iso=").append(isoRange.getLower()).append('-').append(isoRange.getUpper());
        if (expRange != null) s.append(" exp=").append(expRange.getLower()).append('-').append(expRange.getUpper());
        if (evRange != null) s.append(" ev=").append(evRange.getLower()).append(',').append(evRange.getUpper())
                .append(" evstep=").append(String.format(Locale.US, "%.4f", evStep));
        s.append(" mf=").append(String.format(Locale.US, "%.2f", minFocus))
         .append(" zoom=").append(String.format(Locale.US, "%.2f,%.2f", zoomMin, zoomMax))
         .append(" torch=").append(hasFlash ? 1 : 0)
         .append(" ois=").append(hasOis ? 1 : 0)
         .append(" eis=").append(hasEis ? 1 : 0)
         .append(" point=").append(maxAfRegions > 0 || maxAeRegions > 0 ? 1 : 0)
         .append(" fps=").append(fpsList())
         .append(" size=").append(width).append('x').append(height);
        return s.toString();
    }

    /** Frame rates usable at each stream size: "720:24,25,30,60;1080:24,30". */
    private String fpsList() {
        Range<Integer>[] ranges = ch.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES);
        StreamConfigurationMap map = ch.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
        TreeSet<Integer> uppers = new TreeSet<>();
        if (ranges != null) for (Range<Integer> r : ranges) uppers.add(r.getUpper());
        StringBuilder s = new StringBuilder();
        int[][] sizes = { { 640, 480 }, { 1280, 720 }, { 1920, 1080 } };
        for (int[] sz : sizes) {
            double maxFps = 30;
            if (map != null) {
                try {
                    long minDur = map.getOutputMinFrameDuration(SurfaceTexture.class, new Size(sz[0], sz[1]));
                    if (minDur > 0) maxFps = 1e9 / minDur;
                } catch (IllegalArgumentException ignored) { }
            }
            if (s.length() > 0) s.append(';');
            s.append(sz[1]).append(':');
            boolean first = true;
            for (int u : uppers) {
                if (u < 15 || u > maxFps + 0.5) continue;
                if (!first) s.append(',');
                s.append(u);
                first = false;
            }
        }
        return s.toString();
    }

    /** Short description for the phone's settings screen. */
    String supportSummary() {
        if (mgr == null) return "";
        try {
            for (String id : mgr.getCameraIdList()) {
                CameraCharacteristics c = mgr.getCameraCharacteristics(id);
                Integer facing = c.get(CameraCharacteristics.LENS_FACING);
                if (facing == null || facing != CameraCharacteristics.LENS_FACING_BACK) continue;
                int[] caps = c.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES);
                boolean ms = contains(caps, CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR);
                boolean mp = contains(caps, CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_MANUAL_POST_PROCESSING);
                return ms && mp ? "Full manual control (ISO, shutter, Kelvin, focus)"
                        : ms ? "Manual ISO, shutter and focus; white balance presets"
                        : "Automatic camera; exposure compensation, presets and zoom";
            }
        } catch (CameraAccessException | RuntimeException ignored) { }
        return "Unknown";
    }
}

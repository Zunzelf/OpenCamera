package net.majorkernelpanic.streaming.video;

import android.hardware.Camera.CameraInfo;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaFormat;
import android.os.Build;
import android.view.Surface;

import net.majorkernelpanic.streaming.hw.EncoderDebugger;
import net.majorkernelpanic.streaming.rtp.MediaCodecInputStream;

import java.io.IOException;

/**
 * H.264 stream fed by a Camera2 source straight into the encoder's input
 * surface (no GL pass, no Camera1). The camera itself lives in the app
 * (see {@link Source}) so it can expose manual controls while streaming.
 */
public class Camera2H264Stream extends H264Stream {

	/** Opens a camera and renders it into the encoder input until stop(). */
	public interface Source {
		/** Blocks until frames are flowing into {@code encoderInput}, or throws. */
		void start(boolean front, int width, int height, int fps, Surface encoderInput) throws IOException;
		void stop();
		/** Lets the app scale the bitrate the PC asked for (phone setting). */
		int bitrateFor(int requestedBitsPerSecond);
	}

	private static volatile Source sSource;

	public static void setSource(Source source) { sSource = source; }

	public static boolean available() { return sSource != null && Build.VERSION.SDK_INT >= 21; }

	private final boolean mFront;
	private Surface mInput;

	public Camera2H264Stream(int cameraFacing) {
		super(cameraFacing);
		mFront = cameraFacing == CameraInfo.CAMERA_FACING_FRONT;
	}

	// Camera1 is never opened by this stream.
	@Override protected synchronized void createCamera() { }
	@Override protected synchronized void updateCamera() { }
	@Override protected synchronized void destroyCamera() { }
	@Override public synchronized void startPreview() { }
	@Override public synchronized void stopPreview() { }
	@Override public synchronized void setFlashState(boolean state) { }

	@Override
	protected void encodeWithMediaCodec() throws RuntimeException, IOException {
		final Source src = sSource;
		if (src == null) throw new IOException("No camera source");

		EncoderDebugger debugger = EncoderDebugger.debug(mSettings, mQuality.resX, mQuality.resY);
		mMediaCodec = MediaCodec.createByCodecName(debugger.getEncoderName());
		MediaFormat f = MediaFormat.createVideoFormat("video/avc", mQuality.resX, mQuality.resY);
		f.setInteger(MediaFormat.KEY_BIT_RATE, src.bitrateFor(mQuality.bitrate));
		f.setInteger(MediaFormat.KEY_FRAME_RATE, mQuality.framerate);
		f.setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface);
		f.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1);
		if (Build.VERSION.SDK_INT >= 23) f.setInteger(MediaFormat.KEY_PRIORITY, 0); // realtime
		mMediaCodec.configure(f, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
		final Surface input = mMediaCodec.createInputSurface();
		mMediaCodec.start();
		try {
			src.start(mFront, mQuality.resX, mQuality.resY, mQuality.framerate, input);
		} catch (IOException | RuntimeException e) {
			try { mMediaCodec.stop(); } catch (Exception ignored) { }
			mMediaCodec.release();
			mMediaCodec = null;
			input.release();
			throw e;
		}
		mInput = input;
		mPacketizer.setInputStream(new MediaCodecInputStream(mMediaCodec));
		mPacketizer.start();
		mStreaming = true;
	}

	@Override
	public synchronized void stop() {
		final Source src = sSource;
		// Camera first, so no frame goes into an encoder that is shutting down.
		if (src != null && mStreaming) src.stop();
		super.stop();
		if (mInput != null) {
			mInput.release();
			mInput = null;
		}
	}
}

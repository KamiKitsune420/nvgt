package com.samtupy.nvgt;

import android.app.Activity;
import android.content.Context;
import android.media.AudioAttributes;
import android.os.Build;
import android.os.VibrationAttributes;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.os.VibratorManager;

// Vibrates the phone itself (not a game controller, joystick.vibrate does that).
// Scripts reach this with android_vibrate(), android_vibrate_pattern(), android_vibrate_effect() and android_cancel_vibration().
// Needs the VIBRATE permission, which NVGT's manifest has; Android doesn't ask the user for it.
public final class Haptics {
	// The values of the script's android_haptic_effect enum.
	private static final int EFFECT_CLICK = 0;
	private static final int EFFECT_DOUBLE_CLICK = 1;
	private static final int EFFECT_TICK = 2;
	private static final int EFFECT_HEAVY_CLICK = 3;

	private static Vibrator getVibrator(Activity activity) {
		if (activity == null) return null;
		Vibrator vibrator;
		if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
			VibratorManager manager = (VibratorManager)activity.getSystemService(Context.VIBRATOR_MANAGER_SERVICE);
			vibrator = manager != null ? manager.getDefaultVibrator() : null;
		} else vibrator = (Vibrator)activity.getSystemService(Context.VIBRATOR_SERVICE);
		return vibrator != null && vibrator.hasVibrator() ? vibrator : null;
	}

	// Vibrations are marked as belonging to a game, so they follow the phone's media vibration setting rather than the touch feedback one.
	private static boolean play(Vibrator vibrator, VibrationEffect effect) {
		if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) vibrator.vibrate(effect, VibrationAttributes.createForUsage(VibrationAttributes.USAGE_MEDIA));
		else vibrator.vibrate(effect, new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_GAME).setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION).build());
		return true;
	}

	// A strength is 1 to 255, anything else means the phone's default.
	private static int amplitude(int strength) {
		return strength >= 1 && strength <= 255 ? strength : VibrationEffect.DEFAULT_AMPLITUDE;
	}

	public static boolean canVibrate(Activity activity) {
		try {
			return getVibrator(activity) != null;
		} catch (Exception e) {
			return false;
		}
	}

	// Whether the phone can vibrate softer or harder. If not, any strength above 0 is simply on.
	public static boolean hasStrengthControl(Activity activity) {
		try {
			Vibrator vibrator = getVibrator(activity);
			return vibrator != null && vibrator.hasAmplitudeControl();
		} catch (Exception e) {
			return false;
		}
	}

	public static boolean vibrate(Activity activity, int duration, int strength) {
		try {
			Vibrator vibrator = getVibrator(activity);
			if (vibrator == null || duration < 1) return false;
			return play(vibrator, VibrationEffect.createOneShot(duration, amplitude(strength)));
		} catch (Exception e) {
			return false;
		}
	}

	// timings are the lengths of each step in milliseconds. strengths gives each step a strength, 0 being a pause and -1 the phone's default.
	// Without strengths the steps alternate, starting with a vibration: vibrate, pause, vibrate, pause...
	// repeat is the step to loop back to when the pattern ends, or -1 to play it once.
	public static boolean vibratePattern(Activity activity, int[] timings, int[] strengths, int repeat) {
		try {
			Vibrator vibrator = getVibrator(activity);
			if (vibrator == null || timings == null || timings.length < 1) return false;
			if (strengths != null && strengths.length > 0 && strengths.length != timings.length) return false;
			boolean alternate = strengths == null || strengths.length < 1;
			long[] steps = new long[timings.length];
			int[] amplitudes = new int[timings.length];
			boolean vibrates = false;
			for (int i = 0; i < timings.length; i++) {
				if (timings[i] < 0) return false;
				steps[i] = timings[i];
				if (alternate) amplitudes[i] = i % 2 == 0 ? VibrationEffect.DEFAULT_AMPLITUDE : 0;
				else amplitudes[i] = strengths[i] == 0 ? 0 : amplitude(strengths[i]);
				if (steps[i] > 0 && amplitudes[i] != 0) vibrates = true;
			}
			if (!vibrates) return false;
			if (repeat < 0 || repeat >= timings.length) repeat = -1;
			return play(vibrator, VibrationEffect.createWaveform(steps, amplitudes, repeat));
		} catch (Exception e) {
			return false;
		}
	}

	// The phone's own short, crisp effects. Before Android 10 these don't exist, so plain vibrations stand in for them.
	public static boolean vibrateEffect(Activity activity, int effect) {
		try {
			Vibrator vibrator = getVibrator(activity);
			if (vibrator == null) return false;
			if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
				int id;
				switch (effect) {
					case EFFECT_CLICK: id = VibrationEffect.EFFECT_CLICK; break;
					case EFFECT_DOUBLE_CLICK: id = VibrationEffect.EFFECT_DOUBLE_CLICK; break;
					case EFFECT_TICK: id = VibrationEffect.EFFECT_TICK; break;
					case EFFECT_HEAVY_CLICK: id = VibrationEffect.EFFECT_HEAVY_CLICK; break;
					default: return false;
				}
				return play(vibrator, VibrationEffect.createPredefined(id));
			}
			switch (effect) {
				case EFFECT_CLICK: return play(vibrator, VibrationEffect.createOneShot(20, VibrationEffect.DEFAULT_AMPLITUDE));
				case EFFECT_DOUBLE_CLICK: return play(vibrator, VibrationEffect.createWaveform(new long[] {0, 30, 100, 30}, -1));
				case EFFECT_TICK: return play(vibrator, VibrationEffect.createOneShot(10, VibrationEffect.DEFAULT_AMPLITUDE));
				case EFFECT_HEAVY_CLICK: return play(vibrator, VibrationEffect.createOneShot(40, VibrationEffect.DEFAULT_AMPLITUDE));
				default: return false;
			}
		} catch (Exception e) {
			return false;
		}
	}

	public static boolean cancel(Activity activity) {
		try {
			Vibrator vibrator = getVibrator(activity);
			if (vibrator == null) return false;
			vibrator.cancel();
			return true;
		} catch (Exception e) {
			return false;
		}
	}
}

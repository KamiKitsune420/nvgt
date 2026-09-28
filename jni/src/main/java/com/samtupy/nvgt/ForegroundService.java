package com.samtupy.nvgt;

import android.app.Activity;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.IBinder;
import android.os.PowerManager;

// A foreground service: an ongoing notification that tells Android the app is doing something the user wants to keep going
// (a live connection, audio), so it keeps running and keeps its network when it's in the background or the screen is off.
// Scripts start and stop it with android_start_foreground_service() and android_stop_foreground_service().
// The app's AndroidManifest.xml has to declare the service (android:foregroundServiceType="mediaPlayback") and the
// FOREGROUND_SERVICE, FOREGROUND_SERVICE_MEDIA_PLAYBACK, WAKE_LOCK and POST_NOTIFICATIONS permissions.
public class ForegroundService extends Service {
	private static final String CHANNEL_ID = "nvgt_foreground";
	private static final int NOTIFICATION_ID = 1422;
	private PowerManager.WakeLock wakeLock;
	private WifiManager.WifiLock wifiLock;

	public static boolean start(Activity activity, String title, String text) {
		if (activity == null) return false;
		try {
			Intent intent = new Intent(activity, ForegroundService.class);
			intent.putExtra("title", title);
			intent.putExtra("text", text);
			activity.startForegroundService(intent);
			return true;
		} catch (Exception e) {
			return false;
		}
	}

	public static boolean stop(Activity activity) {
		if (activity == null) return false;
		try {
			return activity.stopService(new Intent(activity, ForegroundService.class));
		} catch (Exception e) {
			return false;
		}
	}

	@Override
	public int onStartCommand(Intent intent, int flags, int startId) {
		String title = intent != null ? intent.getStringExtra("title") : null;
		String text = intent != null ? intent.getStringExtra("text") : null;
		if (title == null || title.isEmpty()) title = getApplicationInfo().loadLabel(getPackageManager()).toString();
		if (text == null) text = "";
		NotificationManager manager = (NotificationManager)getSystemService(Context.NOTIFICATION_SERVICE);
		if (manager != null && manager.getNotificationChannel(CHANNEL_ID) == null) {
			NotificationChannel channel = new NotificationChannel(CHANNEL_ID, "Running in the background", NotificationManager.IMPORTANCE_LOW);
			channel.setShowBadge(false);
			manager.createNotificationChannel(channel);
		}
		// Tapping the notification brings the app back.
		PendingIntent open = null;
		Intent launch = getPackageManager().getLaunchIntentForPackage(getPackageName());
		if (launch != null) {
			launch.setFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_RESET_TASK_IF_NEEDED);
			open = PendingIntent.getActivity(this, 0, launch, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
		}
		Notification.Builder builder = new Notification.Builder(this, CHANNEL_ID)
			.setContentTitle(title)
			.setContentText(text)
			.setSmallIcon(getApplicationInfo().icon)
			.setOngoing(true)
			.setCategory(Notification.CATEGORY_SERVICE);
		if (open != null) builder.setContentIntent(open);
		Notification notification = builder.build();
		try {
			if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) startForeground(NOTIFICATION_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK);
			else startForeground(NOTIFICATION_ID, notification);
		} catch (Exception e) {
			// Android refused (for example, the manifest doesn't declare the service type). Nothing more can be done here.
			stopSelf();
			return START_NOT_STICKY;
		}
		// Keep the processor and Wi-Fi awake while the screen is off, so the connection doesn't stall.
		if (wakeLock == null) {
			PowerManager power = (PowerManager)getSystemService(Context.POWER_SERVICE);
			if (power != null) {
				wakeLock = power.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, getPackageName() + ":foreground");
				wakeLock.setReferenceCounted(false);
				wakeLock.acquire();
			}
		}
		if (wifiLock == null) {
			WifiManager wifi = (WifiManager)getApplicationContext().getSystemService(Context.WIFI_SERVICE);
			if (wifi != null) {
				int mode = Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q ? WifiManager.WIFI_MODE_FULL_LOW_LATENCY : WifiManager.WIFI_MODE_FULL_HIGH_PERF;
				wifiLock = wifi.createWifiLock(mode, getPackageName() + ":foreground");
				wifiLock.setReferenceCounted(false);
				wifiLock.acquire();
			}
		}
		// If Android kills the app, don't bring the notification back on its own.
		return START_NOT_STICKY;
	}

	// Swiping the app away from recent apps ends it, notification and all.
	@Override
	public void onTaskRemoved(Intent rootIntent) {
		stopSelf();
		super.onTaskRemoved(rootIntent);
	}

	@Override
	public void onDestroy() {
		if (wakeLock != null && wakeLock.isHeld()) wakeLock.release();
		wakeLock = null;
		if (wifiLock != null && wifiLock.isHeld()) wifiLock.release();
		wifiLock = null;
		super.onDestroy();
	}

	@Override
	public IBinder onBind(Intent intent) {
		return null;
	}
}

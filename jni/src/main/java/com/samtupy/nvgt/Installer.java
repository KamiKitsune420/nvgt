package com.samtupy.nvgt;

import android.app.Activity;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageInstaller;
import android.os.Build;
import java.io.File;
import java.io.FileInputStream;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * Hands an APK the app has downloaded to the system's package installer, which is how an app that is not from a store updates itself.
 * The system asks the user before anything is installed, and the first time sends them to allow this app to install apps at all. The app's manifest needs the REQUEST_INSTALL_PACKAGES permission (android_permissions in the build configuration).
 * An update only goes on over the copy that is installed when both are signed with the same key. When it does, the system closes the app to replace it, so a script never sees an install succeed.
 */
public final class Installer {
	private static final String ACTION = "com.samtupy.nvgt.INSTALL_STATUS";
	private static volatile String status = ""; // "", "preparing", "asking", "cancelled", or "failed: " and why.
	private static BroadcastReceiver receiver;

	public static String getStatus() {
		return status;
	}

	public static boolean install(final Activity activity, final String path) {
		if (activity == null || path == null || status.equals("preparing")) return false;
		final File apk = new File(path);
		if (!apk.isFile() || !apk.canRead()) {
			status = "failed: the file could not be read";
			return false;
		}
		status = "preparing";
		final Context context = activity.getApplicationContext();
		listen(context, activity);
		// Copying a whole APK into the installer takes a while, so not on the script's thread.
		new Thread(() -> {
			PackageInstaller installer = context.getPackageManager().getPackageInstaller();
			PackageInstaller.Session session = null;
			try {
				PackageInstaller.SessionParams params = new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
				params.setSize(apk.length());
				int id = installer.createSession(params);
				session = installer.openSession(id);
				try (InputStream in = new FileInputStream(apk); OutputStream out = session.openWrite("app.apk", 0, apk.length())) {
					byte[] buffer = new byte[1 << 16];
					for (int n; (n = in.read(buffer)) > 0; ) out.write(buffer, 0, n);
					session.fsync(out);
				}
				// The installer fills in what happened, so the intent has to be one it may change, and from Android 14 such an intent has to name the app it is for.
				Intent result = new Intent(ACTION).setPackage(context.getPackageName());
				int flags = PendingIntent.FLAG_UPDATE_CURRENT | (Build.VERSION.SDK_INT >= 31 ? PendingIntent.FLAG_MUTABLE : 0);
				session.commit(PendingIntent.getBroadcast(context, id, result, flags).getIntentSender());
			} catch (Exception e) {
				if (session != null) session.abandon();
				status = "failed: " + (e.getMessage() != null ? e.getMessage() : e.toString());
				android.util.Log.e("Installer", "could not hand the package to the installer", e);
			} finally {
				if (session != null) session.close();
			}
		}, "nvgt-install").start();
		return true;
	}

	// Hears what the installer has to say: first that the user has to be asked, which it leaves to the app to do, and then how it went.
	private static synchronized void listen(Context context, final Activity activity) {
		if (receiver != null) return;
		receiver = new BroadcastReceiver() {
			@Override public void onReceive(Context c, Intent intent) {
				int code = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE);
				if (code == PackageInstaller.STATUS_PENDING_USER_ACTION) {
					Intent confirm = Build.VERSION.SDK_INT >= 33 ? intent.getParcelableExtra(Intent.EXTRA_INTENT, Intent.class) : (Intent)intent.getParcelableExtra(Intent.EXTRA_INTENT);
					if (confirm == null) {
						status = "failed: the installer gave nothing to ask with";
						return;
					}
					status = "asking";
					try {
						activity.startActivity(confirm);
					} catch (Exception e) {
						status = "failed: " + e.getMessage();
					}
				} else if (code == PackageInstaller.STATUS_SUCCESS) status = ""; // Only seen when what was installed is some other app.
				else if (code == PackageInstaller.STATUS_FAILURE_ABORTED) status = "cancelled";
				else {
					String why = intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE);
					status = "failed: " + (why != null ? why : "error " + code);
				}
			}
		};
		IntentFilter filter = new IntentFilter(ACTION);
		if (Build.VERSION.SDK_INT >= 33) context.registerReceiver(receiver, filter, Context.RECEIVER_NOT_EXPORTED);
		else context.registerReceiver(receiver, filter);
	}
}

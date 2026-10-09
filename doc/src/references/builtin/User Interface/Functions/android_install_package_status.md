# android_install_package_status
Says how the install started with android_install_package is going.

`string android_install_package_status();`

## Returns:
string: an empty string when nothing is going on, "preparing" while the file is being copied to the installer, "asking" once Android has put its question to the user, "cancelled" if they said no or left the question, or "failed: " followed by Android's reason, for example a version code lower than the one installed or a file that is not a whole APK. Always empty on a platform other than Android.

## Remarks:
An update to the running app that succeeds is never seen here: Android closes the app to replace it.

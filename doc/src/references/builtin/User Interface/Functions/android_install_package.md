# android_install_package
Hands an APK file to Android's package installer, which is how an app that does not come from a store installs an update to itself.

`bool android_install_package(const string&in path);`

## Arguments:
* const string&in path: the APK file, somewhere the app can read, such as its own files directory.

## Returns:
bool: true if the file is being handed over, false if it can't be read, another one is still being handed over, or on a platform other than Android.

## Remarks:
This returns at once. The file is copied to the installer on another thread, and then Android asks the user whether to install it. Follow it with android_install_package_status.

Nothing is ever installed without the user saying yes. The first time, Android sends them to its settings to allow your app to install apps at all, and they come back to the app with the request cancelled, so offer the update again.

Your app needs the REQUEST_INSTALL_PACKAGES permission in its manifest, which `"android_permissions": "REQUEST_INSTALL_PACKAGES"` in the build section of its configuration adds. Google does not allow this in apps on the Play Store, which updates apps itself.

Android only installs an update over the copy that is there when both are signed with the same key and the new one's version code is not lower (`product_version_code` in the build configuration). When an update to the running app goes on, Android closes the app to replace it and does not start it again.

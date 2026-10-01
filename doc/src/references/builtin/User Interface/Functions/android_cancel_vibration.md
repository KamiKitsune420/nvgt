# android_cancel_vibration
Stops any vibration that the game has started on the phone.

`bool android_cancel_vibration();`

## Returns:
bool: true on success, false otherwise or if called on a platform other than Android.

## Remarks:
This is mostly needed to stop a pattern that was started with a repeat index in android_vibrate_pattern, as other vibrations end on their own.

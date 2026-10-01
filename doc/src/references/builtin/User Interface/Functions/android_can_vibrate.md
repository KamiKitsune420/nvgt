# android_can_vibrate
Determines whether the phone has a vibrator.

`bool android_can_vibrate();`

## Returns:
bool: true if the device can vibrate, false otherwise or if called on a platform other than Android.

## Remarks:
Some tablets and Chromebooks have no vibrator at all. This does not tell you whether the user has turned vibration off in their settings, which a game cannot find out.

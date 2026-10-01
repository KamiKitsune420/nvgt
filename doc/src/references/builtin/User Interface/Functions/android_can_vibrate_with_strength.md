# android_can_vibrate_with_strength
Determines whether the phone can vibrate softer or harder, rather than only on and off.

`bool android_can_vibrate_with_strength();`

## Returns:
bool: true if the strength arguments of android_vibrate and android_vibrate_pattern have an effect on this device, false otherwise or if called on a platform other than Android.

## Remarks:
On a phone where this returns false, any strength other than 0 is simply on. Patterns still work, but pulses can only be told apart by how long they are.

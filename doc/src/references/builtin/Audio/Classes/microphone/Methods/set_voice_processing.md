# set_voice_processing
Asks the system to clean up what the microphone captures for voice chat, most importantly by removing the echo of the device's own speaker.

`bool microphone::set_voice_processing(bool enabled);`

## Arguments:
* bool enabled: Whether voice processing should be on.

## Returns:
bool: true if the microphone is now in the requested state, false otherwise.

## Remarks:
At present this only has an effect on Android, where it tells the system to treat the recording as one side of a call. That switches on whatever the device provides for calls, usually echo cancellation, noise suppression and automatic gain. On other platforms this returns false when asked to enable it and nothing changes.

How well it works depends on the device, and it can change how the recording sounds, so leave it off for anything other than speech.

The recording device is reopened when this changes, so a moment of audio is lost.

Without this, voice chat through a phone's speaker feeds back, because the microphone picks up the other players' voices and sends them back to them. Players who use headphones don't have that problem.

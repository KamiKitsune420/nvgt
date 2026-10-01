# microphone
This class captures audio from a recording device such as a microphone, and feeds it into the sound system like any other source of audio.

`microphone(int device = -1, audio_engine@ engine = sound_default_engine);`

## Arguments:
* int device = -1: The index of the recording device to use in the array returned by get_sound_input_devices(), or -1 for the system's default device.
* audio_engine@ engine = sound_default_engine: The audio engine that the captured audio will be part of.

## Remarks:
A microphone is an audio node, so what it captures goes wherever you attach it with attach_output_bus. Attach it to a mixer to hear it, or to an audio_wav_encoder or audio_opus_encoder to record it to a file or a stream.

Recording begins as soon as the object is created. An exception is thrown if the device can't be opened.

### Android
Android only lets an app record once the user has agreed to it, which takes 2 things.

* The app must declare that it wants to record. Add RECORD_AUDIO to the build.android_permissions configuration option, for example with the line `build.android_permissions = RECORD_AUDIO` in a file called mygame.properties next to mygame.nvgt. Without this Android refuses without asking the user.
* The user must agree. The first time a microphone object is created, Android asks them and your script waits for their answer. If they refuse, an exception is thrown. You can ask earlier yourself, at a moment of your choosing, with android_request_permission("android.permission.RECORD_AUDIO").

To keep recording while the app is in the background or the screen is off, set the build.android_foreground_service configuration option as well and call android_start_foreground_service after the user has agreed to recording.

For voice chat, see the set_voice_processing method.

# Android Vibration Tutorial
On Android, an NVGT game can vibrate the phone it is running on. In an audio game this is a second channel of feedback that needs no sound at all: a tick as the player moves through a menu, a thump when they take damage, or a pattern of pulses that tells them which way to turn.

This tutorial covers the functions involved and how to use them well. Each has its own page in the User Interface section of the reference.

## What you need
Nothing extra. NVGT's Android apps already carry the permission that vibration needs, and Android never asks the player to approve it.

These functions only do anything on Android. Everywhere else they exist but return false, so you can call them freely without wrapping your code in `#if android`. To vibrate a game controller rather than the phone, use the vibrate method of the joystick object, which works on every platform.

## Checking what the device can do
Not every device can vibrate, and those that can differ in how well.

* android_can_vibrate() tells you whether there is a vibrator at all. Some tablets and Chromebooks have none.
* android_can_vibrate_with_strength() tells you whether the device can vibrate softer and harder. Many cheaper phones can only switch their vibrator on and off.

```NVGT
void main() {
	if (!android_can_vibrate()) alert("vibration", "This device can't vibrate.");
	else if (!android_can_vibrate_with_strength()) alert("vibration", "This device can vibrate, but only at one strength.");
	else alert("vibration", "This device can vibrate at different strengths.");
}
```

## A single vibration
android_vibrate takes a length in milliseconds and, optionally, a strength from 1 (softest) to 255 (hardest). Leave the strength out to use the phone's default.

```NVGT
android_vibrate(200); // A fifth of a second.
android_vibrate(200, 80); // The same, but soft.
```

## The phone's own effects
Android has a few short effects built in, which the phone's manufacturer has tuned to feel clean on that particular hardware. For feedback that happens often they are a much better choice than a very short call to android_vibrate, which on many phones feels mushy.

* ANDROID_HAPTIC_TICK: very light, for moving through a menu or list.
* ANDROID_HAPTIC_CLICK: for confirming something.
* ANDROID_HAPTIC_DOUBLE_CLICK: two clicks in a row.
* ANDROID_HAPTIC_HEAVY_CLICK: stronger, for something important.

```NVGT
android_vibrate_effect(ANDROID_HAPTIC_TICK);
```

## Patterns
android_vibrate_pattern plays several steps one after another. You give it an array with the length of each step in milliseconds.

With only that array, the steps alternate, starting with a vibration: vibrate, pause, vibrate, pause and so on. This plays three short pulses:

```NVGT
android_vibrate_pattern({80, 60, 80, 60, 80});
```

For more control, pass a second array of the same length giving the strength of each step. 0 is a pause, 1 to 255 is a vibration of that strength, and -1 is the phone's default strength. This plays a soft pulse, a pause, then a long hard one:

```NVGT
android_vibrate_pattern({100, 100, 400}, {60, 0, 255});
```

The third argument makes a pattern loop. It is the index of the step to return to when the pattern ends. A looping pattern keeps going until you call android_cancel_vibration or start another vibration, so remember to stop it. This is a heartbeat that runs for five seconds:

```NVGT
android_vibrate_pattern({60, 120, 60, 700}, {255, 0, 255, 0}, 0);
wait(5000);
android_cancel_vibration();
```

## Designing patterns that work everywhere
On a phone that can't change strength, every strength other than 0 is simply on. A pattern that relies on the difference between a soft pulse and a hard one will feel like two identical pulses there.

The safest approach is to make patterns differ by their timing: the number of pulses and how long each one is. One pulse for left and two for right works on every phone, whereas soft for left and hard for right does not. If you do want to use strength, check android_can_vibrate_with_strength and fall back to a timing based pattern when it returns false.

```NVGT
void signal_danger() {
	if (android_can_vibrate_with_strength()) android_vibrate(400, 255);
	else android_vibrate_pattern({150, 50, 150, 50, 150});
}
```

## Things to be aware of
* Starting a vibration replaces any that is still playing. Vibrations don't queue up or mix.
* The player can turn vibration off in their phone's settings, and Android may silence it in battery saver or do not disturb mode. Your game is not told when this happens, so the functions can return true even though nothing is felt. Never make vibration the only way to learn something important.
* Android won't usually let an app vibrate while it is in the background, so vibration can't stand in for a notification.
* Vibration uses battery. A pattern left looping for a long time will be noticed.
* It is worth giving players a setting to turn vibration off inside your game, as some find it unpleasant or distracting.

## A complete example
This is a small menu that ticks as you move through it and clicks when you choose something.

```NVGT
void main() {
	show_window("vibration menu");
	string[] items = {"start game", "options", "exit"};
	int position = 0;
	screen_reader_speak(items[position], true);
	while (true) {
		wait(5);
		if (key_pressed(KEY_DOWN) and position < items.length() - 1) {
			position++;
			android_vibrate_effect(ANDROID_HAPTIC_TICK);
			screen_reader_speak(items[position], true);
		}
		if (key_pressed(KEY_UP) and position > 0) {
			position--;
			android_vibrate_effect(ANDROID_HAPTIC_TICK);
			screen_reader_speak(items[position], true);
		}
		if (key_pressed(KEY_RETURN)) {
			android_vibrate_effect(ANDROID_HAPTIC_CLICK);
			if (position == 2) exit();
			screen_reader_speak("you chose " + items[position], true);
		}
		if (key_pressed(KEY_ESCAPE) or key_pressed(KEY_AC_BACK)) exit();
	}
}
```

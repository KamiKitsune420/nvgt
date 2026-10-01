# audioform_screen_keyboard
Set whether forms bring up the on-screen keyboard for their input boxes on devices that have one, such as Android phones.

`bool audioform_screen_keyboard = true;`

## Remarks:
While this is true, the on-screen keyboard appears when an input box that can be edited gains focus, and goes away when focus moves to another kind of control or the form is reset or destroyed. Read-only input boxes don't bring it up.

If the user puts the keyboard away themselves, it stays away until they double tap the input box or move to a different one.

This also switches on text input for a hardware keyboard that is connected to such a device, which is needed before anything can be typed into an input box at all.

On computers this does nothing, as text input is always available there.

If your game stops monitoring a form without resetting or destroying it while an input box has focus, the keyboard stays up. Call stop_text_input() yourself in that case.

Set this to false if you would rather control the keyboard yourself with start_text_input() and stop_text_input().

# audio_form
This class facilitates the easy creation of user interfaces that convey their usage entirely through audio.

## Notes:
* many of the methods in this class only work on certain types of controls, and will return false and set an error value if used on invalid types of controls. This will generally be indicated in the documentation for each function.
* Exceptions are not used here. Instead, we indicate errors through `audio_form::get_last_error()`.
* An audio form object can have up to 50 controls.
* On a touch screen a form can be operated with gestures: swipe right or left to move between controls, swipe up or down for the arrow keys, and double tap to activate the control that has focus. See the audioform_touch_navigation global property for the full list.
* On devices with an on-screen keyboard, such as Android phones, it appears by itself when an editable input box gains focus. See the audioform_screen_keyboard global property.

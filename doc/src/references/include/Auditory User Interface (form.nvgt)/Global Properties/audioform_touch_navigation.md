# audioform_touch_navigation
Set whether forms can be operated with gestures on a touch screen.

`bool audioform_touch_navigation = true;`

## Remarks:
While this is true, any form that is being monitored responds to the following gestures.

* Swipe right or left: move to the next or previous control.
* Swipe up or down: the up or down arrow key, which moves through the items of a list, the values of a slider or the lines of an input box.
* Double tap: activate the control that has focus. This presses a button or link, toggles a checkbox, checks an item in a multiselect list or brings up the on-screen keyboard in an input box. On anything else it presses the form's default button.
* Swipe left or right with 2 fingers: the left or right arrow key, which moves through the text of an input box by character.
* Swipe up or down with 2 fingers: the home or end key.
* Tap with 2 fingers: stop speech.

Each gesture presses the keys that do the same job, so a form behaves by touch exactly as it does with a keyboard. Gestures only count while a form's monitor method is being called, they do nothing during the rest of your game.

Only touch screens are listened to, not the trackpad of a laptop.

Set this to false if your game already turns gestures into key presses in its forms, for example with touch.nvgt, to stop each gesture from being handled twice.

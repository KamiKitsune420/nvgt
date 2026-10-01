# audioform_touch_swipe_distance
Set how far a finger must travel across a touch screen before a form treats the movement as a swipe.

`float audioform_touch_swipe_distance = 0.1;`

## Remarks:
The distance is a fraction of the screen, where 1.0 is its full width or height. The default of 0.1 is a tenth of the screen.

Raise this if players move between controls by accident when they mean to tap, and lower it if swipes are not being noticed.

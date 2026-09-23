# Touchpad gestures

Every touchpad gesture that settles on a step runs the same physics: the
three-finger workspace switch, the four-finger overview open and close, and the
filmstrip inside the overview. `GesturePhysics` in `src/input/gesture_physics.h`
holds it, and each gesture feeds it a `SwipeTracker`. The strip scroll and the
overview both reuse the tracker's projection, so one model covers what used to be
a hand-rolled accumulator per gesture.

## Position

A gesture accumulates the travel libinput reports, divided by the distance one
step takes: `kSwipeWorkspacePx` for a workspace, the full overview distance for
the overview open and close, and the viewport for the strip. The resulting
fractional position drives the scene directly, so the content follows the
fingers rather than moving when a counter passes a mark.

The 16 px axis lock that decides which of those a gesture is costs travel of its
own, and that travel is the gesture's first reading rather than a head start the
tracker never sees. Without that, a slide jumps by the lock distance on its first
frame and the swipe reads as if it caught.

Past either end the position is rubber-banded rather than clamped: extra travel
approaches 0.05 of a step without reaching it, so the content keeps moving under
the fingers while still reading as an end.

## Release

Nothing here is a threshold. On release the gesture feeds the tracker one
zero-delta sample at the release time, which drains the speed of fingers that
came to rest before letting go, and the projection decides the landing:

- `SwipeTracker::projectedEndPos()` is where the travel would coast to a stop
  under the tracker's exponential deceleration, so distance and speed decide
  together. A drag that stops before the release falls back where it started; a
  flick of the same distance crosses.
- The projection is rubber-banded into the steps that exist and rounded onto one
  of them, which is why a release never lands between workspaces.
- The speed the settle starts from is the release speed scaled by the rubber-band
  derivative at the release point, so what the settle carries is the speed that
  was visible, not the finger speed the damping hid.

`GesturePhysics::release` is that whole decision and is a pure function over a
`SwipeTracker`. That is what `tests/unit/gesture_physics.cpp` covers, and it has
to be: the headless backend has no touchpad and `zwlr_virtual_pointer_v1` carries
no gesture events, so no harness check can reach a gesture state machine.

## Settling

`GesturePhysics::release` returns the step to land on together with the speed to
carry into it. `WorkspaceGroup::slideSettle` hands both to
`AnimatedValue::settleSpring`, and `Overview::startAnimation` does the same for
the overview zoom, so a released gesture continues at the speed it was released
at instead of stopping dead and starting over from rest.
`AnimatedValue::finishSpringTail` ends the spring once it can no longer move a
pixel.

Callers that are not a gesture — keybindings, IPC actions, a workspace
activation taking over an in-flight switch — pass zero velocity and get the same
spring from rest.

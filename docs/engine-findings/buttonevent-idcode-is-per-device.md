# `ButtonEvent::GetIDCode()` is numbered per device, not in one scan-code space

## TL;DR

Every `RE::ButtonEvent` that reaches an input sink carries an ID code, but that number only means something
together with the event's `GetDevice()`. The keyboard reports DirectInput scan codes; the mouse reports its own
button index starting at zero; the gamepad reports a third numbering again. The three spaces overlap, and nothing
in the event type tells them apart.

So a hotkey stored as a scan code has to be matched against keyboard events **only**:

```cpp
if (e->GetDevice() != RE::INPUT_DEVICE::kKeyboard)
    continue;                       // before any comparison against a stored scan code
const std::uint32_t dxsc = btn->GetIDCode();
```

Without that gate, an ordinary mouse action fires a hotkey the player never pressed.

## The collisions that actually bite

From `RE/B/BSWin32MouseDevice.h`:

```text
kLeftButton=0  kRightButton=1  kMiddleButton=2  kButton3..7=3..7  kWheelUp=8  kWheelDown=9
```

Lined up against the DirectInput scan codes for the number row (`DIK_1` = 0x02 … `DIK_0` = 0x0B):

| Mouse input  | ID code | Keyboard key with the same code |
| ------------ | ------- | ------------------------------- |
| Left button  | 0       | — (no key is scan code 0)       |
| Right button | 1       | `Esc` (`DIK_ESCAPE`)            |
| Middle       | 2       | `1`                             |
| Button 3–7   | 3–7     | `2` `3` `4` `5` `6`             |
| Wheel up     | 8       | `7`                             |
| Wheel down   | 9       | `8`                             |

The wheel is the dangerous one, because it is the only entry a player triggers continuously rather than
deliberately. Each notch arrives as a separate `ButtonEvent` with `value = 1` and `heldDownSecs = 0`, so
`IsDown()` — which is `IsPressed() && HeldDuration() == 0` — returns true for every single notch.

## What it looks like when you get it wrong

A tester bound the dashboard to `Shift+8` and reported that it "pops up every time I scroll down while running".
Both halves matched by accident: sprint holds Shift, and each wheel-down notch reports ID code 9, which is
`DIK_8`. Third person made it constant, because that is where scrolling zooms the camera.

In the plugin log it shows up as show/hide pairs far too fast to be a keypress:

```text
03:19:33.635 DashboardUIManager: shown
03:19:33.716 DashboardUIManager: hidden      <- 81 ms
03:19:33.935 DashboardUIManager: shown
03:19:34.032 DashboardUIManager: hidden      <- 97 ms
```

A deliberate press-and-press-again is 400 ms or more apart. Sub-100 ms toggle pairs mean consecutive wheel notches.

## The false lead

Rebinding looks like it should fix it, and it does not. The tester rebound three times — through MCM Helper,
through our own in-game capture mode, and by editing the INI — and the log shows all three landing on
`DXSC=9 mods=1`, the same value every time, because they kept pressing `Shift+8`. Nothing about the symptom points
at the key they chose, so "I already tried rebinding" is a true statement that rules nothing out.

Worse, our capture mode read `GetIDCode()` from whatever device fired, so scrolling during a rebind would have
*bound the wheel itself* — storing a value that then collides forever. The device gate has to sit ahead of the
capture branch, not just the match branch.

## The `dxsc == 0` guard is not a device check

`DashboardUIManager`'s sink already skipped ID code 0, which incidentally swallowed every left mouse click. That
made the bug look narrower than it was and hid the shape of the problem: the guard is a sane sanity check for the
keyboard (no key has scan code 0), but it is not a substitute for asking which device spoke.

## MCM Helper is not affected the same way

SkyUI-style keymap controls — which MCM Helper's `"type": "keymap"` inherits — report mouse buttons at `256 + N`
and gamepad buttons at `266 + N`, so a mouse binding made through the MCM lands far outside DIK range and simply
never matches. Only raw `GetIDCode()` reads from an input sink see the overlapping numbering.

## Applies to

Any `RE::BSTEventSink<RE::InputEvent*>` that compares `GetIDCode()` against a stored binding. In this repo the
only such site is `HotkeySink::ProcessEvent` in `src/DashboardUIManager.cpp`; if a second one appears, it needs
the same gate.

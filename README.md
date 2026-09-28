# hypr-kinetic-scroll

Momentum ("kinetic", "inertial") scrolling for touchpads, implemented inside
the Hyprland compositor so it works the same in every application, terminal
included, not only in browsers that fling on their own.

Flick, and the content keeps moving at the speed your finger had when it left
the pad, one step per display frame, slowing along a time-based curve close to
macOS. Touch the pad and it stops. Swipe again and the new swipe takes over.

Fork of [savonovv/hypr-kinetic-scroll](https://github.com/savonovv/hypr-kinetic-scroll)
with rewritten physics, gesture ownership, and a measured core.

## How it works

- **Launch on the real lift.** Momentum starts synchronously on libinput's
  finger-lift event. No timers, no hitch.
- **Launch at the finger's end speed.** The velocity is the mean speed of the
  touchpad reports in the last `velocity_window_ms` before the last report, so
  a flick that accelerates to the end launches at its end speed, independent of
  the pad's report rate. The motionless gap before the lift is lift mechanics
  up to `lift_tail_grace_ms`; beyond that it is hesitation and the launch fades
  to nothing at `lift_tail_fade_ms`, so "scroll, pause, lift" launches nothing.
- **One step per frame.** Momentum is emitted from the compositor's render hook
  for the monitor showing the target window, integrating an exponential decay
  over the real elapsed time. Timer jitter changes neither speed nor distance.
  A timer only steps in as a watchdog when no frame arrives.
- **Owns the gesture.** While momentum runs the client never sees the finger
  lift (delivered, GTK4 and Chromium would fling on top of ours), synthetic
  events carry the finger axis source, and the plugin sends the `axis_stop` it
  owes when momentum ends. Every opened axis sequence is closed, including
  across a re-touch hand-off.
- **Stops when it should.** Touchpad contact, a new swipe, the target window or
  surface changing or going away, a mouse click or focus change (optional),
  input capture, disabling, unload.
- **Per-app policy** evaluated once at gesture start: explicit rules, a
  disabled-class list, and a browser heuristic (browsers keep their native
  fling by default). Scroll factors follow Hyprland's own precedence:
  `input:touchpad:scroll_factor` / `input:scroll_factor`, per-device factor,
  window rule (`scroll_touchpad` / `scroll_mouse`).
- **Cheap.** Config is bound once; the hot paths cost a few microseconds per
  event and per frame (see `bench/`).

## Install

The plugin reads Hyprland's internal objects, so it must be built against the
running Hyprland. hyprpm does that and rebuilds after updates:

```bash
hyprpm add https://github.com/mihap/hypr-kinetic-scroll
hyprpm enable hypr-kinetic-scroll
hyprpm reload -n
```

Or from a clone: `make reinstall` (same steps; asks for sudo), `make update`
after a Hyprland update, `make reload` to load the installed build.

A build refuses to load into a Hyprland it was not built against (ABI guard; a
notification names both versions). Rebuild rather than bypass.

### Requirements

Hyprland headers (installed by hyprpm, or the `hyprland` package on Arch),
their `pkg-config` dependencies including `libeis-1.0`, and a C++23 compiler.

## Configuration

Defaults are tuned for a macOS-like feel and need no changes. All options, with
their defaults:

```ini
plugin:kinetic-scroll:enabled = 1
plugin:kinetic-scroll:decel = 0.967            # velocity multiplier per 16 ms; lower = shorter fling
plugin:kinetic-scroll:delta_multiplier = 1.0   # scales the launch velocity; 1.0 = the finger's own speed
plugin:kinetic-scroll:min_velocity = 0.5       # scroll units per 16 ms; below it momentum stops / a lift launches nothing
plugin:kinetic-scroll:velocity_window_ms = 32  # reports within this many ms before the last report set the launch speed
plugin:kinetic-scroll:lift_tail_grace_ms = 28  # motionless ms before the lift that cost nothing
plugin:kinetic-scroll:lift_tail_fade_ms = 80   # motionless ms before the lift at which the launch has faded to nothing
plugin:kinetic-scroll:interval_ms = 16         # watchdog interval when no compositor frame arrives
plugin:kinetic-scroll:disable_in_browser = 1   # browsers (firefox, chrom*, brave, vivaldi, opera, librewolf, zen) keep their own fling
plugin:kinetic-scroll:stop_on_target_change = 1
plugin:kinetic-scroll:disabled_classes =       # exact window classes, comma or space separated
plugin:kinetic-scroll:stop_on_click = 0
plugin:kinetic-scroll:stop_on_focus = 0
plugin:kinetic-scroll:debug = 0                # log to /tmp/hypr-kinetic-scroll.log
plugin:kinetic-scroll:metrics_file =           # per-gesture JSON metrics, see bench/README.md
```

### Lua configs (Omarchy and other `hyprland.lua` setups)

Colons become nesting and dashes become underscores:

```lua
hl.config({
  plugin = {
    kinetic_scroll = {
      decel = 0.967,
      delta_multiplier = 1.0,
    },
  },
})
```

The same form works at runtime for tuning:

```bash
hyprctl eval 'hl.config({ plugin = { kinetic_scroll = { decel = 0.975 } } })'
```

(`hyprctl keyword` does not work with Lua configs.)

### Per-app rules

Classes are exact matches; find them with `hyprctl clients`. An explicit rule
wins over the disabled-class list and the browser heuristic.

INI:

```ini
plugin:kinetic-scroll:disabled_classes = org.telegram.desktop, steam
```

Lua:

```lua
hl.plugin.kinetic_scroll.disable("org.telegram.desktop")
hl.plugin.kinetic_scroll.enable("firefox")          -- override the browser heuristic

hl.plugin.kinetic_scroll.disable_default()          -- allow-list mode
hl.plugin.kinetic_scroll.enable("com.mitchellh.ghostty")

hl.plugin.kinetic_scroll.enable_default()
hl.plugin.kinetic_scroll.reset_rules()
```

Legacy INI keyword: `kinetic-scroll-rule disable firefox` / `enable firefox`.

## Verify

1. `hyprctl plugin list` shows `hypr-kinetic-scroll`.
2. Two-finger flick and lift: the content keeps moving and slows down.
3. Touch the pad during momentum: it stops at once.
4. Scroll, hold the fingers still for half a second, lift: nothing moves.
5. Flick during momentum: the new flick takes over without a jump.

## Measuring

`metrics_file` records one JSON line per gesture: hot-path cost, lift-to-first-
emit latency, frame cadence, launch velocity, travel, and gesture-ownership
counters. `bench/analyze.py` compares runs; `bench/scrollback.txt` is a
deterministic fixture. Protocol and how to read the numbers: `bench/README.md`.

## Development

```bash
make            # build hypr-kinetic-scroll.so
make test       # compositor-free tests: physics and the gesture state machine (tests/)
make dev-load   # build and hot-load into the running Hyprland from a fresh /tmp path
make unload-all # unload every loaded copy
make SKIP_VERSION_CHECK=1   # bypass the ABI guard (development only)
```

Layout:

- `physics.hpp`: launch-velocity estimator and decay integrator. No Hyprland
  dependencies; deterministic and unit-tested (`tests/physics_test.cpp`).
- `gesture.hpp` / `gesture.cpp`: the gesture state machine and per-app
  policy. Every decision lives here: which events are ours, when momentum
  launches, what is emitted per frame, which `axis_stop` events are owed and
  when they are settled, how a gesture ends. It talks to the compositor only
  through `Gesture::IHost`, so `tests/gesture_test.cpp` drives it with a
  scripted host: flicks, hand-offs, focus changes, destroyed targets, capture,
  unload, watchdog cadence, smooth mice.
- `kinetic.cpp` / `kinetic.hpp`: the Hyprland side (`IHost`): pointer devices,
  focus and target references, seat calls, event-loop timer, frames, config
  handles, debug log. No decisions.
- `metrics.*`: optional instrumentation, written from an event-loop idle callback.
- `main.cpp`: plugin entry, config registration, Lua functions, event hooks.

Plugin-owned globals must keep ordinary linkage (no `inline` variables in
headers): `inline` globals become `STB_GNU_UNIQUE` symbols, glibc then marks
the .so `NODELETE`, and a rebuilt plugin hot-loaded into the same session binds
to the previous build's objects. That crashed the compositor once.

## Troubleshooting

- **A swipe is sometimes ignored entirely** (no scrolling at all, not just no
  momentum). That happens before the plugin: libinput's thumb heuristic
  suppresses one touch when the two fingers land far apart vertically (more
  than 25 mm) with the lower one near the bottom edge, and disable-while-typing
  drops touches that start right after a keypress. Keep the fingers level and
  away from the bottom edge; consider `input:touchpad:disable_while_typing =
  false`. Hyprland's log shows libinput's decision for each touch.
- **Some touchpads report scrolls as mouse events with smooth deltas.** Those
  are handled too; they launch after 50 ms of silence since they have no lift event.
- **Version mismatch on load**: rebuild against the running Hyprland
  (`make update` with hyprpm, or `make`).

## License

MIT

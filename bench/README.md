# Measuring hypr-kinetic-scroll

The plugin runs on Hyprland's main thread. Two things are worth measuring:
how much time it steals there, and whether the fling behaves as designed.
Both come from the same instrumentation in `metrics.hpp`, which is meant to
stay byte-identical between builds that are compared.

## Enable

```
hyprctl eval 'hl.config({ plugin = { kinetic_scroll = { metrics_file = "/tmp/hks-before.jsonl" } } })'
```

Set it to `""` to stop. The path is re-read at the start of every gesture.
One JSON object per gesture is appended, whether or not momentum launched.

## Protocol for a before/after run

1. Same window (Ghostty, scrollback long enough), same monitor, same config.
2. 20 flicks: 10 fast, 10 medium. Let each fling run out; don't interrupt.
3. 5 "scroll, pause, lift" gestures. These must *not* launch.
4. 5 flicks interrupted by touching the pad.
5. A few mouse-wheel scrolls, so idle-path cost is sampled.
6. Repeat with the other build into a different file, then:

```
python3 bench/analyze.py /tmp/hks-before.jsonl /tmp/hks-after.jsonl
```

## Reading the table

Cost (lower is better):

- `axis cost / event`: ns spent in `onAxis` per touchpad event of a gesture.
- `step cost / frame`: ns per momentum frame, including target checks.
- `idle axis`: cost of axis events that don't belong to a gesture (wheel etc).
  This runs for every scroll event on the system, forever.

Behaviour:

- `lift -> launch`, `lift -> first emit`: hitch at finger lift. Should be
  near 0 ms and one frame respectively.
- `render dt mean/stddev/max`: cadence of momentum frames. Mean should equal
  the monitor's frame time, stddev near 0.
- `timer share`: fraction of frames from the watchdog timer. Should be ~0.
- `launch |v|`, `travel`, `fling duration`: physics, should not change with
  a pure optimization refactor. If they do, the refactor changed behaviour.
- `lift tail`: ms between last motion sample and libinput's stop event.
  Hardware property; useful for tuning the velocity window.

The stop-reason histogram at the bottom checks the protocol: pause-lifts
should show `belowThreshold` or `gestureIdle`, interruptions `touchpadContact`.

## Raw samples

Each line carries the gesture's raw input samples (`[libinput_ms, dv, dh]`)
so the velocity and decay code can later be replayed offline, deterministic
and compositor-free, against recorded gestures.

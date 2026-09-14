# GPU contention with an uncapped target app

2026-09-14

## The observation

Same preset, same resolution, same two full-resolution passes, two zones:

| Zone | Game fps | Captured | Presented | gpu (wait) per frame |
|------|----------|----------|-----------|----------------------|
| Silvermoon | ~60 (GPU-bound scene) | 60 | 44 | 21.7 ms |
| Haranar | ~140 (light scene, uncapped) | 130 | 15 | 65-80 ms |

The neural work is a fixed cost and cannot care what is on screen, so the 3x
swing is not the pipeline getting slower. Capping the game's foreground frame
rate to 60 restored the presented rate to ~45 in Haranar, which confirms the
cause is the game taking GPU the overlay needs -- but capping the game per title
is exactly what a generic overlay must not require.

## Two fixes tried

**Adaptive capture interval.** `MinUpdateInterval` was pinned at 1 ms, so
capture requested ~130 fps while the overlay presented 15, and every discarded
frame still cost a full-resolution copy into the ring. Driving the interval from
the present rate (about 2x, clamped 1-33 ms) cut requests from ~130 to ~30.

Result: **no change to the presented rate.** The discarded copies were not the
bottleneck. Kept anyway, on its own merits -- it stops real GPU work, bandwidth
and power going into frames nobody consumes -- but its comments now claim only
that.

**HIGH command queue priority.** `D3D12_COMMAND_QUEUE_PRIORITY_HIGH` instead of
the default, on the theory that the overlay was losing a scheduling fight to an
uncapped game and would win it with a higher priority.

Result: **no measurable difference.** Reverted. Whatever governs this is not
reachable through queue priority -- plausibly because the contention is for
memory bandwidth and clocks rather than for scheduling slots, or because queue
priority does not arbitrate across processes the way it does within one.

## Why neither result was conclusive about the cause

The `gpu` figure in the log was `gpuWaitMs`: wall-clock time spent waiting on a
fence. That includes time queued behind whatever else the GPU is doing, so
"21.7 ms capped, 70 ms uncapped" is consistent with two different worlds:

1. Our work costs ~10 ms and we spend 60 ms queued. Contention for scheduling.
2. Our work genuinely costs 70 ms because it executes with fewer resources --
   less bandwidth, lower effective clocks, a colder cache.

Those want different fixes, and nothing measured so far separates them.

## What was added instead

D3D12 timestamp queries bracketing `cmdList2` (the neural passes and the
compose), resolved into a readback buffer and read one frame late so nothing
stalls. The performance line now carries both:

```
per frame: gpu work 12.4, gpu wait 70.1, cpu 1.1, idle 0.0, present 0.0 ms
```

The gap between work and wait is the contention, measured rather than inferred.

Phase one and the wait on the optical-flow queue sit outside the span
deliberately, so a slow NVOFA does not read as expensive neural work.

## What the numbers will mean

- **Work stays low, wait balloons.** Case 1. Our work is cheap and we are
  queued. Nothing inside the pipeline recovers it, because the problem is not
  our cost. Capping the app is then the only lever that works from outside its
  process, which promotes the NVAPI driver-profile frame limit from "maybe" to
  "necessary" -- see the adaptive quality plan.
- **Work itself balloons.** Case 2. The work is genuinely more expensive under
  contention. Reducing it helps proportionally, and the adaptive quality ladder
  is the right answer.

Measure before building either.

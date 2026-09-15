# Adaptive quality: a frame-rate target instead of a pass configuration

2026-09-14

**Status: built, tried, reverted.** Kept as a record of why, because the idea
will look obvious again later and the reasons it did not work are not.

## What was built

A `QualityLadder` of five rungs (two full-resolution passes down to one at
75%), a `target_fps` setting, and a servo in the render loop that stepped down
when the presented rate missed the target and back up when it had headroom. It
serviced on the presented rate rather than on measured frame time, deliberately:
GPU timestamps include preemption, so a frame-time servo would have cut quality
to fight interruptions that cutting quality does not reduce.

## Why it was reverted

**It descended to the bottom rung and stayed there.** The guard meant to stop
that -- "do not step down when the app is the limit" -- tested whether the
presented rate was close to the captured rate. But the adaptive capture
interval, added the same day, deliberately requests frames at about twice the
present rate. So captured is always ~2x presented, the guard never fires, and
the ladder reads a GPU-contended overlay as one that needs less quality. Two
features that are individually reasonable and jointly wrong.

**Turning the target off did not restore the configured setup.** `ApplySettings`
only re-requests a pass setup when the setup *fields* change. Switching the
ladder off changes none of them, so the overlay kept running the rung it had
descended to until something else was edited and edited back. A real bug, and
a symptom of the ladder's state living beside the config rather than in it.

**It could not have worked anyway.** The constraint is GPU share, not the cost
of the work: two passes measured ~11 ms of actual GPU time
(docs/spikes/2026-09-14-gpu-contention.md) while presenting at 16 fps. Dropping
to one pass saves ~8 ms of work in a frame that is losing ~35 ms to preemption.
The ladder was spending quality against a cost that was not the problem.

## What would have to be true for this to be worth revisiting

- A way to distinguish "we are slow because our work is expensive" from "we are
  slow because we are being preempted", live and cheaply. The per-pass
  histogram behind the record hotkey shows the difference, but only over
  thousands of frames -- far too slow to servo on.
- Or a cheaper configuration that meaningfully widens the range of apps the
  overlay can run over, which would make a ladder worth having for reach rather
  than for frame rate.

Until one of those exists, the manual controls are better: they are
predictable, they do what the operator asked, and they do not silently trade
away the quality the operator chose.

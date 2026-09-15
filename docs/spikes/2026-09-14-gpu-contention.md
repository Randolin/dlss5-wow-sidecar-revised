# GPU contention with an uncapped target app

2026-09-14

## The observation

Same preset, same resolution, same two full-resolution passes at 4K, one
machine (RTX 5090), two states of the same game:

| Target app | Presented | gpu work | Per-pass |
|------------|-----------|----------|----------|
| capped at 60 fps | ~34 | ~25 ms | 8-14 ms |
| uncapped (~140 fps) | ~16 | ~50 ms | 19-32 ms |

Identical work, twice the wall-clock cost. Capping the game restores the
overlay; nothing else tried does.

## What the frame time is made of

Per-stage GPU timestamps inside the neural command list showed the model is
essentially the entire frame -- the input copy, the chained compose between
passes and the final compose are together under half a millisecond. There is
nothing to optimise in the sidecar's own compute.

But the per-pass times were **bimodal**, snapping between two values for
identical work rather than scattering, which a histogram over every frame
settled.

## The measurement that settled it

Two recordings, same spot, standing still (motion median 0.1 px/frame), two
passes with `model_scale` 0.5 and `final_pass_full` -- so pass one runs at
1920x1080 and pass two at 3840x2160. Per-pass GPU time, 1 ms buckets:

| Bucket | Capped at 60 | Uncapped (~140) |
|--------|--------------|-----------------|
| 3 ms   | 372          | 208             |
| 8 ms   | 175          | 103             |
| 13 ms  | 306          | 4               |
| 18 ms  | --           | 48              |
| 23 ms  | --           | 157             |
| 28 ms  | --           | 85              |

Peaks at a regular 5 ms interval, and **the leftmost peaks are identical in
both runs**. The base cost does not move with load; only the weight shifts
rightward.

Two pass sizes explain peaks at 3 and 8. They cannot explain 18, 23 and 28 --
there are only two passes. Those are the same work plus one, two, three or
four interruptions of ~5 ms each.

**The two passes really cost about 11 ms together: 3 for the half-resolution
pass and 8 for the full-resolution one.** That is roughly 90 fps of neural
work. Everything above it is time spent preempted while the app runs.

This corrects an earlier conclusion recorded here. `gpu work` from the
timestamps was read as proof that the work genuinely costs 45 ms; it is not.
D3D12 timestamps are GPU wall clock, so an interleave lands inside the measured
span. The work was always cheap.

## Three fixes tried, all negative

**Adaptive capture interval.** `MinUpdateInterval` was pinned at 1 ms, so
capture requested ~130 fps while the overlay presented 15, and every discarded
frame still cost a full-resolution copy into the ring. Driving the interval
from the present rate cut requests from ~130 to ~30.

No change to the presented rate. Kept anyway, on its own merits -- it stops
real GPU work, bandwidth and power going into frames nobody consumes -- but it
is not a frame-rate fix and its comments say so.

**HIGH D3D12 command queue priority.** No measurable difference. Reverted.
`D3D12_COMMAND_QUEUE_PRIORITY` arbitrates between queues *within* one process;
the contention here is between two processes, so the setting was wired to
nothing.

**HIGH WDDM process scheduling class**
(`D3DKMTSetProcessSchedulingPriorityClass`, resolved from gdi32). This is the
process-level equivalent and the correct layer in principle. The raise was
granted -- the log confirmed `normal -> high` -- and the presented rate was
unchanged: still ~34 capped, ~16 uncapped, in the same run with the raise
active. Reverted.

## What this leaves

The GPU is one non-preemptible resource shared by two processes, one of which
does not know the overlay exists. Preemption is not something a user-mode
application can decline, and the 5 ms quantum looks like a scheduling
granularity rather than anything of ours. There are only two ways to get a
larger share: take less, or make the app take less. No scheduling hint reached
from outside the app appears to move the split.

- **Take less.** Reduce what the model costs. Real, but the headroom is
  smaller than it looked: the work is already down to ~11 ms.
- **Make the app take less.** Cap its frame rate. Doing that from outside the
  app means the NVAPI driver-profile limit, which works generically but writes
  to the user's driver profile and must be restored on exit and after a crash.
  Deferred; it wants an explicit opt-in.

## What the ceiling actually is

With the GPU to itself, this configuration would present around 90 fps, not the
~22 the wall-clock timings implied. The distance between what the overlay gets
and that ceiling is entirely how much GPU the app leaves it.

That matters for the adaptive quality ladder: servoing on measured frame time
would cut quality to fight interruptions, and cutting quality does not reduce
interruptions. A ladder has to be driven by the presented rate against a target,
not by how long a pass appeared to take.

## The honest ceiling for the tool

The sidecar needs GPU headroom to exist. An app that is already GPU-bound at 60
fps has none, and capping it creates none -- there is nothing to reclaim. For
those titles the overlay cannot run at this quality, and no amount of
engineering changes that: the neural work has to fit somewhere.

What *does* widen the range of usable apps is lowering the model's cost, since
a cheaper configuration fits in a smaller gap. That makes the quality ladder
less a compromise on frame rate and more the thing that decides which apps the
tool works on at all.

## Loose end, now closed

The bimodal per-pass times were the thread worth pulling, and they resolved
into the quantised distribution above rather than into a model with two speeds.
The histogram that settled it is behind the record hotkey; a recording and a
dump reproduce it in about thirty seconds.

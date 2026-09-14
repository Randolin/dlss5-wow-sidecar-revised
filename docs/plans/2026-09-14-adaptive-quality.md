# Adaptive quality: a frame-rate target instead of a pass configuration

2026-09-14

## The problem this solves

The neural work costs what it costs -- roughly 22 ms per frame at 4K for two
full-resolution passes on a 5090 -- and it shares one GPU with the app it is
overlaying. An uncapped game expands until the GPU is full, so the presented
rate depends on how much room the game happens to leave, which varies by title,
by zone, and by what the player is looking at.

Today the operator resolves that by hand: pick a pass count and a model scale,
watch the frame rate, adjust. That is tolerable for one known game and wrong for
a tool meant to run over anything. The setting the operator actually has an
opinion about is "how smooth should this be", not "how many passes at what
resolution".

Two changes already landed that this plan assumes:

- Capture requests are driven from the present rate rather than pinned at 1 ms,
  so the pipeline no longer pays for full-resolution copies it discards.
- The D3D12 queue runs at HIGH priority, so the overlay's work is scheduled
  rather than squeezed when the game is uncapped.

Neither creates headroom. They stop waste and settle who wins contention. This
plan spends whatever headroom exists, automatically.

## The shape

The manager's Tuning page grows a target frame rate: Off (the current manual
behaviour), 30, 60, 120. The runtime measures its own GPU time per frame -- it
already does, for the HUD -- and moves along a quality ladder to fit the budget.

The ladder, most expensive first. Each rung is a `NrPassSetup`, which the
direct pass can already hot-swap live without dropping a frame:

| Rung | Passes | modelScale | finalPassFull |
|------|--------|------------|---------------|
| 0    | 2      | 1.00       | false         |
| 1    | 2      | 0.75       | true          |
| 2    | 2      | 0.50       | true          |
| 3    | 1      | 1.00       | n/a           |
| 4    | 1      | 0.75       | n/a           |

The rungs below the operator's chosen pass count are reachable; the rungs above
it are not. The target is a ceiling on quality as well as a floor on frame rate:
if rung 0 already fits, nothing moves.

## The control law

Budget per frame is `1000 / target` minus a reserve for the app's own frame.
The reserve is the open question (see below).

- Measure GPU ms per frame over the existing reporting window, not per frame.
  One slow frame is a hitch, not a trend.
- Over budget for two consecutive windows: step down one rung.
- Under half the budget for four consecutive windows: step up one rung.

Asymmetric on purpose. Dropping quality to recover smoothness should happen
quickly; raising it should be slow and reluctant, because a ladder that climbs
eagerly will oscillate at exactly the point where a rung is marginal, and a
visible quality change every few seconds is worse than sitting one rung low.

Never move while the overlay is hidden or paused, and reset the counters on a
target change, a preset change, or a rebuild.

## What to build

1. `float targetFps` in `NrSettings` (0 = off), read and written by Config,
   exposed in the manager beside the pass controls rather than under advanced
   tuning -- this is the setting most operators should touch.
2. A `QualityLadder` in `src/common/neural/`: pure, holds the rung table and
   the hysteresis counters, takes (gpuMs, budgetMs) and returns an optional new
   rung. Unit-testable without a device, which matters because the interesting
   behaviour is the oscillation it must not do.
3. Pipeline calls it from the same block that already assembles the HUD model,
   and hands any rung change to `DirectNrPass::RequestPassSetup`.
4. The HUD shows the live rung when a target is set, so a quality change is
   legible rather than mysterious.

## Open questions

**The reserve.** How much of the frame budget belongs to the app? It is not
measurable from outside the app's process -- we can see our own GPU time and the
capture rate, but not the app's frame cost. A fixed fraction is a guess. An
alternative is to servo on the presented rate directly rather than on GPU time:
if we are not hitting the target, step down, regardless of where the time went.
That needs no reserve and no model of the app at all, and it is probably the
right first version. GPU time then becomes a diagnostic rather than the input.

**Interaction with a frame-capped app.** If the app is capped below our target,
we can never hit the target and the ladder would descend to the bottom rung
chasing a rate that is not available. The capture rate tells us the ceiling:
never step down when the presented rate is already within a frame or two of the
captured rate, because the app, not us, is the limit.

**Whether rung 0 should be reachable at all at 4K.** If two full-resolution
passes cannot hit 60 on a 5090, the honest default for a 60 target is to start
at rung 1 or 2 rather than descend into it on every launch.

## Not in this plan

Driver-level frame limiting of the target app through NVAPI. It would cap the
app generically, from outside, with no in-game change -- the most effective
lever available and the most invasive, since it writes to the user's driver
profile and must be restored on exit and after a crash. Worth revisiting behind
an explicit opt-in once the ladder exists, because the ladder makes it less
necessary.

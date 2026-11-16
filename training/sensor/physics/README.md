# physics/ — where the thresholds come from

Nothing in this folder is fitted to data. It takes published safety limits,
pushes them through the sensor's own response law, and emits
`firmware/sensor_node/physics_thresholds.h`. That is why the system can ship
with defensible thresholds before you have recorded a single session.

```
cd training
python -m physics.derive_thresholds --report        # show the working
python -m physics.derive_thresholds --sensitivity   # is it robust to sigma?
python -m physics.derive_thresholds --emit ../firmware/sensor_node/physics_thresholds.h
```

The emit refuses if the constraint set is not satisfied. That refusal is the
point of the exercise; a derivation that always succeeds is not checking
anything.

## The one identity everything rests on

A metal-oxide element obeys a power law over its useful range:

```
Rs / R0 = A · C^(−B)
```

`A` carries the heater voltage, the load resistor, the element's age and its
absolute resistance. On a 3.3 V build with an unknown breakout, `A` is
unknowable — which is why no ppm reading from this hardware is defensible.

`B` is the slope in log-log space. It is set by the surface reaction order and
is a property of the chemistry, not of the operating point.

The firmware works in `dev = ln(baseline / (Rs/R0))`, and in that quantity:

```
dev = ln( A·C_base^(−B) / (A·C^(−B)) ) = B · ln( C / C_base )
```

**A cancels exactly.** So "10 % of the lower explosive limit" becomes a z-score
of `dev` without anyone ever knowing what R0 is. Losing absolute calibration
costs the ppm readout and nothing else.

## Files

| file | what it holds |
|---|---|
| `gas_data.py` | Fuel properties, sensor power-law fits, published exposure limits. Every number is a physical property, a datasheet fit, or a standard. |
| `room.py` | Single-zone mass balance with a stratification correction, plus thermal and humidity models. |
| `sensors.py` | Concentration → what the firmware sees. Cross-sensitivity, humidity response, drift, and the baseline's freeze rule. |
| `scenarios.py` | 17 situations built from source terms, with severities assigned from the published limits. |
| `derive_thresholds.py` | Anchors → contrasts → logistic fits → verification → header. |

## The three things this derivation found that a bench test would not have

**The baseline freeze was latching on noise.** `BASE_FREEZE_Z = 1.0` was being
tested against the instantaneous sample, so it froze roughly a third of the time
on nothing at all. Once frozen it stopped tracking the sensor's slow drift, and
the drift accumulated into the deviation until every channel wandered up into a
false alarm. It only appears after the sensor has been running long enough to
drift, so on a bench it would have looked like "it works for an hour and then
goes strange overnight" — the hardest class of bug to chase. The freeze decision
now runs on a 30-second smoothed value.

**The gate-based gas discriminator failed at exactly the wrong moment.** Placing
a decision boundary between two classes in ratio space looks fine at the anchors
and degrades as the event grows, because the three channels saturate at
different rates and their ratios converge. Replaced with linear contrasts, which
subtract a signature rather than testing a boundary.

**The CO level and dose knees are one two-parameter problem, not two.** Anchoring
the level channel at 0.5 on 100 ppm — the obvious choice — makes the system
alarm at 1 minute for a level EN 50291 says must stay silent for 10. The level
knee has to sit well above 100 ppm so that 300 ppm alarms on level inside three
minutes while 100 ppm waits for dose. Fast response and nuisance immunity come
from different mechanisms; one threshold could never deliver both.

## What it does not claim

The cross-sensitivity matrix in `sensors.py` is read off datasheet curve
families and is approximate. The obscuration-to-volatile pairing in
`scenarios.py` is a modelling choice, not a measurement. The room is well-stirred
when a real kitchen is not.

None of that is hidden, and none of it is load-bearing in the way it might look.
What the derivation produces is a **starting point that is right to within a
factor of about two and wrong in a direction you can name** — which is a
categorically better place to begin than a number somebody guessed. Your own
data, once you have it, moves these numbers; `adapt.h` moves them at runtime
within a fence; and `calibrate_fusion.py` moves them offline. The physics is the
floor, not the answer.

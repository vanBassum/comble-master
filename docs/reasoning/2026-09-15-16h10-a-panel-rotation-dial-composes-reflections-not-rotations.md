---
id: 2026-09-15-16h10
date: 2026-09-15
time: "16:10"
title: A panel rotation dial composes reflections, not rotations
---

**Before:** the WT-SC01 Plus ran landscape on `LCD_SWAP_XY = true` with both mirrors
off, and the board file called those three constants "the orientation dial: if the UI
comes up sideways or upside down, flip one of them and nothing else". That reads the
dial as a rotation control with three switches, and it had never been wrong in practice
because only one orientation had ever been asked for.

**What changed it:** the UI was drawn portrait to match concept art, so the panel had to
turn. The obvious move — landscape is `swap_xy`, so portrait is the dial at rest — put
text on the glass that had to be held up to a mirror to read.

`swap_xy` on its own is a **transpose**, and a transpose is a reflection across the
diagonal, not a rotation. So the working landscape picture was never a rotated raw
panel; it was a reflected one, and the reflection had simply never been visible because
nothing asymmetric enough to notice had been compared against it. Turning the transpose
off does not undo the reflection, it *leaves* it. A real quarter turn is transpose plus
one mirror, which is why the two swap roles going from one orientation to the other:

```
landscape:  swap_xy = true,  no mirror
portrait:   swap_xy = false, one mirror
```

**And then it cost a second flash**, because "one mirror" is two choices 180° apart and
the first guess took the wrong arm. What settled it was not more algebra — it was Bas
reading the symptom: *maybe up was down and down was up on the display, that explains
why touch is now flipped in both directions.*

That is the part worth keeping. Touch is derived to match the display, so the two dials
fail in tellingly different ways:

- a tap wrong on **one** axis is a reflection — the TOUCH transform is wrong;
- a tap wrong on **both** axes is a half turn — the touch transform is right, and the
  DISPLAY took the wrong arm.

The second case is counter-intuitive enough to be worth writing down: the dial that is
wrong is the one whose symptom you are *not* looking at. A tap landing 180° out is not
evidence about touch at all.

**After:** the board file states the portrait pair as measured rather than derived, and
records the raw-frame relation the observation pins down (`tx = 319 - px`, `ty = py`),
which is *not* what solving the landscape pair by the obvious algebra produces — most
likely the order `esp_lcd_touch` composes swap and mirror in, and with which extents.
That derivation is now explicitly marked as a route not to take again.

The general claim, which outlives this panel: **an orientation dial is a group of
reflections, and only some products in it are rotations.** Two configurations that each
look upright are not therefore the same configuration, and one that looks upright is not
necessarily a rotation of another that does. Treating the three booleans as "flip one
until it looks right" works only until something else — here, the touch layer — has to
agree with the choice.

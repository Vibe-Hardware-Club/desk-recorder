# The dial is not a rotary encoder

If you are building on the Waveshare ESP32-S3 Knob Display and your dial only counts one way,
or one click registers as four, this is the page you were looking for.

**Short version:** it is not a quadrature encoder. The two contacts have no phase relationship
and never overlap. Each one is an independent direction switch. Poll them every 3ms, require
two consistent samples, and fire on release.

This repo's firmware already does all of that, so if you are only using the recorder you will
never meet the problem. This page is for the next person building something else on this board.

## How it presents

- The dial appears to work in one direction only, or reports the same direction both ways.
- Turning one detent produces three or four events.
- Interrupt-driven counting gives wildly inflated numbers: 20 clicks read as 79.
- Measuring the two channels shows them never closing at the same time, which looks like
  broken hardware or a missing pin.

## Why the obvious approach fails

A standard rotary encoder gives you two square waves 90 degrees out of phase, and direction
comes from which one leads. That overlap is the entire mechanism.

On this dial there is no overlap, by design. Across four controlled captures and more than 300
transitions during this project, the two contacts never closed together, not once. Reading that
as "direction is physically unavailable" is a perfectly reasonable conclusion from the data,
and it is wrong.

## What it actually is

From Waveshare's own `bidi_switch_knob.c`:

```c
process_knob_channel(pha_value, ..., KNOB_RIGHT, true,  knob);   // A closes = turned right
process_knob_channel(phb_value, ..., KNOB_LEFT,  false, knob);   // B closes = turned left
```

Two calls. One per channel. One tagged right, one tagged left. Each contact is its own
direction switch, and the mechanism decides which one you close depending on which way you
turn. There is nothing to decode.

Their timing matters as much as their logic:

- **Polled every 3ms**, not interrupt-driven.
- **Two consistent samples** before an edge is believed. Without the debounce, one detent on
  channel B bounces about four times.
- **The event fires on release**, when the contact returns high after being held low for at
  least one tick.

## Working implementation

From `firmware/knob/src/main.cpp` in this repo:

```c
/* One channel, exactly as their process_knob_channel() does it. */
static inline void knob_channel(uint8_t level, volatile uint8_t *prev,
                                volatile uint8_t *cnt, int delta) {
  if (level == 0) {
    if (level != *prev) *cnt = 0;
    else (*cnt)++;
  } else {
    if (level != *prev && ++(*cnt) >= KNOB_DEBOUNCE_TICKS) {
      *cnt = 0;
      detent_steps += delta;
    } else {
      *cnt = 0;
    }
  }
  *prev = level;
}
```

Called every 3ms with `KNOB_DEBOUNCE_TICKS` of 2, once per channel, with `delta` of +1 for A
and -1 for B. Pins are GPIO 8 (A) and GPIO 7 (B).

## The lesson underneath

An afternoon went into concluding this dial could not detect direction. Every measurement taken
was correct. The instrument was fine, the captures were clean, and each new one made the
conclusion feel safer. The theory sitting underneath them was simply wrong, and no amount of
further measuring would ever have revealed that, because the measurements were not the problem.

What settled it in about a minute was flashing the vendor's own factory firmware and watching
its timer count up clockwise and down anticlockwise, doing the thing that had just been proven
impossible.

So, two rules worth stealing:

1. **When a vendor ships driver source, read it before theorising about the hardware.** The
   answer was sitting in a file that had already been downloaded.
2. **When a vendor ships factory firmware, flashing it is the fastest possible test of "can
   this board do X at all".** Minutes, and it settles what hours of inference cannot.

Confidence that grows with each measurement is not the same thing as being right.

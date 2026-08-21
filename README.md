# Desk Recorder

A recorder that sits on your desk, listens to your meetings, and drops a readable transcript
into your Google Drive. Turn the dial to pick what the conversation is about, press to record,
press to stop.

Because they land in Drive, you can read them on your phone, or point Claude or ChatGPT at the
folder and ask what you agreed to.

The audio never goes to a transcription service. It goes to a private bucket on your own
Supabase account, and your own Mac does the transcription locally with Whisper.

<!-- TODO: photo of the device on a desk -->

## What you need

| | |
|---|---|
| The board | Waveshare ESP32-S3 Knob Display 1.8in, about £50. [Where to buy](#where-to-buy) |
| A microSD card | Any small one. One often ships in the slot |
| A USB-C cable | A data cable, not charge-only |
| A Mac | macOS only. The background worker uses launchd |
| Homebrew | Required. See below |
| A Supabase account | Free tier is plenty. The installer walks you through it |
| Google Drive for Desktop | Optional. Transcripts go there if you have it, a local folder if not |

**Homebrew is a real prerequisite, not a nicety.** Transcription uses whisper.cpp, and its
project publishes builds for Windows and Linux but not for macOS, so there is no
download-a-binary route. If you do not have Homebrew, install it from
[brew.sh](https://brew.sh) first. The installer checks, and stops with that link rather than
failing halfway through.

Everything else is free. The only cost is the board.

## Setup

Plug the board into your Mac, then:

```
git clone https://github.com/ORG/desk-recorder.git
cd desk-recorder
./setup.sh
```

It asks for your WiFi, where you want transcripts, and your Supabase project. Then it flashes
the board, configures it over the same cable you flashed it with, and installs the background
worker.

The last thing it does is record ten seconds, transcribe it, and print the result. If you can
read your own words on screen, everything works.

**If it cannot find your device:** unplug it, turn the USB-C plug over, and plug it back in.
This board has two chips behind one port and only one of them answers, depending on which way
up the plug goes in. It is a quirk of the hardware, not a fault, and it is in Waveshare's own
FAQ.

## Using it

- **Turn the dial** to choose a topic. **Press** to start recording, **press again** to stop.
- The screen shows REC and elapsed time while recording, and the device buzzes every minute,
  so it can never be running without you knowing.
- Transcripts appear about **30 minutes** after you stop: the worker waits 12 minutes to be
  sure the recording has ended, then Whisper takes roughly as long as the recording itself.
- **No WiFi is fine.** Recordings sit on the card and upload when the network comes back. A
  chunk is only deleted from the card once the server confirms it has stored it, byte for byte.

## Your topics

Edit `topics.txt`, one per line, then run `./server/deploy.sh`:

```
Work
Personal
Book notes
```

Then **hold the dial down for a second** on the device to pull in the new list. It will not
change until you do, because the list is cached on the card so the device still works when
your WiFi is down.

## How it fits together

```
device ── records to microSD ── uploads over WiFi ──→ your Supabase bucket
                                                            ↓
                            your Mac, every 5 minutes: Whisper transcribes
                                                            ↓
                            Google Drive / Voice Transcripts/<topic>/<date>.md
```

The device holds one token that can only reach your own upload endpoint. It holds no account
keys. Someone who picks it up off your desk gets nothing useful out of it.

Your WiFi password and that token are **not** in the firmware image. They are written into the
device's own memory over the USB cable during setup, which is why one published binary works
for everybody.

If Google Drive for Desktop is not running, transcripts are written but do not sync, and the
folder looks completely normal while that happens. The worker checks, warns in its log, and
drops a `SYNC-WARNING.txt` in your transcripts folder rather than letting it be silent.

## Coming later: who said what

Transcripts are currently one block of text. Speaker names, so a transcript reads
`Alex: ...` / `Sam: ...` instead of running together, work on the author's own desk and will be
added as an optional extra step. They need Python, PyTorch and a Hugging Face account, which is
too much to put in front of someone who just wants transcripts.

## Where to buy

Waveshare ESP32-S3-Knob-Touch-LCD-1.8. Official channels were sold out when this was built and
it came from a third-party Amazon listing for £52.79 without the battery. Check
[Waveshare's own store](https://www.waveshare.com) first.

Board variants differ. See [docs/HARDWARE.md](docs/HARDWARE.md) before you buy.

## Documentation

- [docs/HARDWARE.md](docs/HARDWARE.md) - pin map, board notes, the two-chips-one-port quirk
- [docs/THE-DIAL.md](docs/THE-DIAL.md) - why this dial is not a rotary encoder, and the day
  that cost. Read this if you are building anything else on this board.
- [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md) - every step the installer does, by hand

## Recording other people

If a conversation includes someone who is not you, tell them it is being recorded. Depending on
where you live that is the law, and everywhere it is basic manners.

## Support

This is a personal project shared as-is, not a product. Issues and pull requests are welcome
and may go unanswered. If something breaks,
[docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md) has every step the installer performs so you
can do it by hand.

## Credits and licence

MIT, except `firmware/knob/src/esp_lcd_sh8601.*`, which is Espressif's display driver under
Apache-2.0 and stays under its own licence.

Transcription is [whisper.cpp](https://github.com/ggml-org/whisper.cpp). Hardware and demo
sources are Waveshare's.

Built by **Vibe Hardware Club**.

<!-- TODO Fish: Skool group URL --> · <!-- TODO Fish: reels link -->

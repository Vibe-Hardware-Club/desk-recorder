# Troubleshooting

`setup.sh` drives the Supabase CLI, Homebrew, GitHub and a serial port, all of which change
without warning. When it breaks, this page is every step it performs, so a broken installer is
an inconvenience rather than a dead end.

Logs live in `~/.desk-recorder/`:

```
worker.log     what the worker did, every run that had something to say
worker.err     anything launchd captured on stderr
config         PROJECT_REF and DEST_DIR
token          your device token. Treat it like a password
```

---

## The board

### No device found / no serial port

**Turn the USB-C plug over.** This board has two chips behind one port and only one of them
answers, depending on plug orientation. It is in Waveshare's own FAQ and it is the first thing
to try, before suspecting the cable or the board.

Then check it is a **data** cable. Charge-only USB-C cables are common and look identical.

```bash
ls /dev/cu.usbmodem*
```

### The screen says NOT SET UP

The board has firmware but no WiFi or token yet. Run:

```bash
./scripts/configure.sh
```

### The screen is blank

On a reflective panel with no backlight, a dark room reads as a dead screen. Rule that out
first, with a torch if necessary.

### Flashing by hand

```bash
~/.desk-recorder/bin/esptool --port /dev/cu.usbmodem1101 --baud 921600 \
  write-flash 0x0 firmware/releases/desk-recorder-v1.bin
```

If flashing fails partway, the board may be left without working firmware. It is not bricked:
turn the plug over and flash again.

### Checking what the board thinks it has

```bash
./scripts/configure.sh          # prompts, sends, verifies
```

It reports lengths only, never values. `ready yes` means it has everything it needs.

---

## The backend

### The Supabase CLI

`setup.sh` installs it from Homebrew and signs you in. By hand:

```bash
brew install supabase/tap/supabase
supabase login
```

There is no "am I signed in" command. The test is whether a real call answers:

```bash
supabase projects list --output json
```

Signed out, that prints nothing on stdout and complains on stderr, and both `setup.sh` and
`deploy.sh` stop there and tell you to sign in. Signed in, you get your projects as JSON.

### Deploying by hand

```bash
KNOB_PROJECT_REF=yourprojectref ./server/deploy.sh
```

That regenerates `server/generated.ts` from your `topics.txt` and your token hash, then
deploys. It is safe to run repeatedly and reuses your existing token.

### Every upload returns 401

The function checks the SHA-256 of the bearer token. A 401 on everything means the hash it was
built with does not match the token your device holds. Redeploy and reconfigure:

```bash
./server/deploy.sh
./scripts/configure.sh
```

**`--no-verify-jwt` is not optional.** If you deploy the function by hand without it, Supabase
turns on JWT verification and every device upload starts failing with a 401, because the device
has a bearer token and not a Supabase JWT.

### Checking the backend is alive

```bash
curl -s -H "Authorization: Bearer $(cat ~/.desk-recorder/token)" \
  "https://YOURREF.supabase.co/functions/v1/transcriber-ingest/categories"
```

You should get your topic list back.

### The bucket

Private, named `transcriber`. If `deploy.sh` could not create it, make it by hand:
Supabase dashboard, Storage, New bucket, name `transcriber`, **public off**.

---

## Transcripts

### Nothing appears

Transcripts are not immediate by design. The worker waits **12 minutes** after the last chunk
to be sure the recording has ended, then Whisper takes roughly as long again as the recording
itself. A 20 minute meeting can be 35 minutes before the file exists.

Then, in order:

```bash
tail -20 ~/.desk-recorder/worker.log
launchctl list | grep desk-recorder
curl -s -H "Authorization: Bearer $(cat ~/.desk-recorder/token)" \
  "https://YOURREF.supabase.co/functions/v1/transcriber-ingest/pending"
```

If `/pending` shows your chunks, the board did its job and the problem is on the Mac.

### They exist on my Mac but not in Google Drive

**Google Drive for Desktop is not running.** Files written into the CloudStorage folder while
the app is closed stay on the Mac, and the folder looks entirely normal while it happens. The
worker warns about this and leaves a `SYNC-WARNING.txt` in your transcripts folder.

```bash
pgrep -f "Google Drive"      # nothing means it is not running
open -a "Google Drive"       # it backfills once it starts
```

### Run the worker by hand

```bash
QUIET_SECONDS=1 ./worker/worker.sh
tail -30 ~/.desk-recorder/worker.log
```

`QUIET_SECONDS=1` skips the 12 minute wait, which is what you want when testing.

### The transcript is nonsense, or contains things nobody said

Whisper invents text to fill silence. The worker already high-passes, denoises and normalises
before transcribing, which is what stops most of it. If it is still poor, the recording is
probably too quiet: the device has a single small microphone, and a laptop fan or an open
window competes with it.

### It is slow

Whisper runs slower than realtime on most Macs. The worker takes a lock, so runs cannot pile up
on top of each other, and a long meeting simply takes a while. Nothing is stuck.

---

## Starting again

```bash
launchctl unload ~/Library/LaunchAgents/com.desk-recorder.worker.plist
rm -rf ~/.desk-recorder
```

That removes your token, so the device stops being able to upload until you run `setup.sh`
again. Recordings already on the board's SD card are not touched: nothing is deleted from the
card until the server confirms it has been stored.

#!/usr/bin/env bash
#
# Desk Recorder: the one command.
#
# Installs what is missing, sets up your backend, flashes the board, configures
# it over the cable, installs the background worker, and finishes by recording
# ten seconds and showing you the transcript.
#
# That last step is the point. A setup script that ends with "installed
# successfully" has proved nothing. This one ends with your own voice on screen.
#
# BASH 3.2 ONLY - macOS ships 3.2 and always will.
#
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
CONF="$HOME/.desk-recorder"
SETTINGS="$CONF/config"
BIN="$CONF/bin"
MODEL_DIR="$CONF/models"
MODEL="$MODEL_DIR/ggml-small.en.bin"
MODEL_URL="https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small.en.bin"
FIRMWARE="$HERE/firmware/releases/desk-recorder-v1.bin"
PLIST="$HOME/Library/LaunchAgents/com.desk-recorder.worker.plist"

say()  { printf '\n\033[1m%s\033[0m\n' "$*"; }
step() { printf '  %s\n' "$*"; }
die()  { printf '\n%s\n' "$*" >&2; exit 1; }

[ "$(uname -s)" = "Darwin" ] || die "This installer is macOS only. The background worker uses launchd."
mkdir -p "$CONF" "$BIN" "$MODEL_DIR" && chmod 700 "$CONF"

# ============================================================
say "1/7  Checking what you already have"

for t in curl python3 openssl shasum; do
  command -v "$t" >/dev/null || die "Missing '$t', which macOS normally provides. Install Xcode command line tools with: xcode-select --install"
done
step "system tools present"

# ffmpeg and whisper both come from Homebrew. whisper.cpp publishes binaries for
# Windows and Linux but NOT for macOS (checked against their releases), so there
# is no download-a-binary route here and Homebrew is genuinely required.
if ! command -v brew >/dev/null; then
  die "Homebrew is needed for ffmpeg and Whisper, and is not installed.

Install it with the command on https://brew.sh, then run this again.
It is the standard package manager for macOS and takes a few minutes."
fi
step "homebrew present"

for pkg in ffmpeg whisper-cpp; do
  case "$pkg" in
    ffmpeg)      have=ffmpeg ;;
    whisper-cpp) have=whisper-cli ;;
  esac
  if command -v "$have" >/dev/null; then
    step "$pkg already installed"
  else
    step "installing $pkg (this can take a few minutes)"
    brew install "$pkg" || die "Could not install $pkg. Try 'brew install $pkg' on its own to see why."
  fi
done

# ---- the speech model ------------------------------------------------------
if [ -s "$MODEL" ]; then
  step "speech model already downloaded"
else
  step "downloading the speech model, 488MB, one time only"
  curl -fL --progress-bar -o "$MODEL.part" "$MODEL_URL" || die "Model download failed."
  # A truncated model fails inside whisper with an unhelpful error, so the size
  # is checked here where the message can be useful.
  SZ=$(stat -f%z "$MODEL.part")
  [ "$SZ" -gt 400000000 ] || die "The model downloaded only $SZ bytes and is incomplete. Run this again."
  mv "$MODEL.part" "$MODEL"
fi

# ---- esptool, for flashing -------------------------------------------------
# Espressif DO publish standalone macOS builds, so flashing needs no Python.
if [ -x "$BIN/esptool" ]; then
  step "esptool already installed"
else
  step "downloading esptool"
  case "$(uname -m)" in
    arm64) ARCH=macos-arm64 ;;
    *)     ARCH=macos-amd64 ;;
  esac
  URL=$(curl -s https://api.github.com/repos/espressif/esptool/releases/latest \
        | python3 -c "import sys,json
d=json.load(sys.stdin)
for a in d.get('assets',[]):
    if '$ARCH' in a['name']: print(a['browser_download_url']); break")
  [ -n "$URL" ] || die "Could not find an esptool build for $ARCH."
  curl -fL --progress-bar -o "$BIN/esptool.tar.gz" "$URL" || die "esptool download failed."
  tar -xzf "$BIN/esptool.tar.gz" -C "$BIN" || die "Could not unpack esptool."
  FOUND=$(find "$BIN" -type f -name esptool -perm -u+x | head -1)
  [ -n "$FOUND" ] || die "esptool binary not found after unpacking."
  if [ "$FOUND" != "$BIN/esptool" ]; then
    cp "$FOUND" "$BIN/esptool"
    # The binary is self-contained (verified by running the copy away from its
    # siblings), so the unpacked directory is 27MB of duplication.
    rm -rf "$(dirname "$FOUND")"
  fi
  rm -f "$BIN/esptool.tar.gz"
fi

# ============================================================
say "2/7  Where your transcripts should go"

DEST_DIR=""
DRIVE_ROOT=$(ls -d "$HOME/Library/CloudStorage/GoogleDrive-"*/"My Drive" 2>/dev/null | head -1)
if [ -n "$DRIVE_ROOT" ]; then
  echo "  Found Google Drive: $DRIVE_ROOT"
  printf "  Put transcripts in Google Drive? [Y/n] "
  read -r ans
  case "$ans" in
    [Nn]*) ;;
    *) DEST_DIR="$DRIVE_ROOT/Voice Transcripts" ;;
  esac
fi
if [ -z "$DEST_DIR" ]; then
  DEST_DIR="$HOME/Voice Transcripts"
  echo "  Using a folder on this Mac: $DEST_DIR"
fi
mkdir -p "$DEST_DIR" || die "Could not create $DEST_DIR"

# Prove the destination is writable NOW, rather than discovering it at 2am when
# a transcript is being written.
probe="$DEST_DIR/.write-test-$$"
: > "$probe" 2>/dev/null || die "Cannot write to $DEST_DIR"
rm -f "$probe"
step "destination is writable"

case "$DEST_DIR" in
  *"/Library/CloudStorage/GoogleDrive-"*)
    if ! pgrep -f "Google Drive" >/dev/null 2>&1; then
      echo
      echo "  NOTE: Google Drive is not running. Files written while it is closed"
      echo "  stay on this Mac and do not upload. Open Google Drive when you can."
    fi ;;
esac

# ============================================================
say "3/7  Your topics"

echo "  These are what the dial shows. Currently:"
grep -vE '^[[:space:]]*(#|$)' "$HERE/topics.txt" | sed 's/^/    /'
echo
printf "  Edit them now? [y/N] "
read -r ans
case "$ans" in [Yy]*) "${EDITOR:-nano}" "$HERE/topics.txt" ;; esac

# ============================================================
say "4/7  Your backend"

echo "  You need a free Supabase account and one empty project."
echo "  Create one at https://supabase.com if you have not already."
echo "  Then copy the project ref: it is the subdomain of your project URL,"
echo "  for example https://abcdefghijklm.supabase.co -> abcdefghijklm"
echo
printf "  Supabase project ref: "
read -r PROJECT_REF
[ -n "$PROJECT_REF" ] || die "A project ref is needed."

KNOB_PROJECT_REF="$PROJECT_REF" "$HERE/server/deploy.sh" || die "Backend setup failed. Nothing else has been changed."

# ============================================================
say "5/7  Flashing the board"

echo "  Plug the board into this Mac with a USB-C cable."
printf "  Press Enter when it is connected. "
read -r _

PORT=""
for try in 1 2 3; do
  PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)
  [ -n "$PORT" ] && break
  echo
  echo "  No device found."
  echo "  This board has two chips behind one USB-C port and only one of them"
  echo "  answers, depending on which way up the plug goes in. Unplug it, TURN"
  echo "  THE PLUG OVER, and plug it back in. This is normal for this hardware."
  printf "  Press Enter to look again. "
  read -r _
done
[ -n "$PORT" ] || die "Still no device. See docs/TROUBLESHOOTING.md."
step "found $PORT"

"$BIN/esptool" --port "$PORT" --baud 921600 write-flash 0x0 "$FIRMWARE" \
  || die "Flashing failed. Unplug, turn the plug over, plug back in, and run this again."
step "firmware written and verified"

# ============================================================
say "6/7  Telling the board about your WiFi"

echo "  The board joins WiFi on its own. It needs a 2.4GHz network."
printf "  WiFi network name: "; read -r W_SSID
printf "  WiFi password: ";     read -rs W_PASS; echo

KNOB_PORT="$PORT" KNOB_SSID="$W_SSID" KNOB_PASS="$W_PASS" \
KNOB_HOST="$PROJECT_REF.supabase.co" KNOB_TOKEN="$(cat "$CONF/token")" \
  "$HERE/scripts/configure.sh" || die "Could not configure the board."

# ============================================================
say "7/7  Installing the worker"

cat > "$SETTINGS" <<EOF
PROJECT_REF="$PROJECT_REF"
DEST_DIR="$DEST_DIR"
EOF
chmod 600 "$SETTINGS"

cat > "$PLIST" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>com.desk-recorder.worker</string>
  <key>ProgramArguments</key>
  <array><string>$HERE/worker/worker.sh</string></array>
  <key>StartInterval</key><integer>300</integer>
  <key>RunAtLoad</key><true/>
  <key>StandardErrorPath</key><string>$CONF/worker.err</string>
</dict>
</plist>
EOF

chmod +x "$HERE/worker/worker.sh"
launchctl unload "$PLIST" 2>/dev/null
launchctl load "$PLIST" || die "Could not install the background worker."
step "worker installed, runs every 5 minutes"

# ============================================================
say "Proving it works"

echo "  Recording ten seconds. SAY SOMETHING - the whole point is that you"
echo "  read your own words back in a moment."
echo

stty -f "$PORT" 115200 raw -echo -hupcl 2>/dev/null
exec 3<>"$PORT"
printf 'REC\r\n' >&3
END=$((SECONDS+11))
while [ $SECONDS -lt $END ]; do
  printf '\r  recording... %2d ' $((END-SECONDS))
  IFS= read -r -t 1 -u 3 _line || true
done
printf 'REC\r\n' >&3
printf '\r  stopped.            \n'

echo "  Waiting for the board to upload..."
UPLOADED=""
END=$((SECONDS+90))
while [ $SECONDS -lt $END ]; do
  if IFS= read -r -t 3 -u 3 line; then
    case "$line" in *uploaded*) UPLOADED=yes; break ;; esac
  fi
done
exec 3<&-

[ -n "$UPLOADED" ] || die "The board did not confirm an upload. Check WiFi and see docs/TROUBLESHOOTING.md.
Your recording is safe on the board's SD card and will upload when it can."
step "uploaded"

echo "  Transcribing (QUIET_SECONDS is overridden so you do not wait 12 minutes)..."
QUIET_SECONDS=1 "$HERE/worker/worker.sh"

NEWEST=$(find "$DEST_DIR" -name '*.md' -type f -print0 2>/dev/null | xargs -0 ls -t 2>/dev/null | head -1)
if [ -n "$NEWEST" ]; then
  say "Here is your transcript"
  echo "  $NEWEST"
  echo
  sed -n '/^---$/,/^---$/!p' "$NEWEST" | sed '/^$/d' | head -10 | sed 's/^/    /'
  say "Done. Turn the dial, press to record, press to stop."
  echo "  Transcripts land in: $DEST_DIR"
else
  say "Setup finished, but no transcript appeared"
  echo "  The recording uploaded, so the board is working. Something on this Mac"
  echo "  did not finish the job. Check $CONF/worker.log"
  exit 1
fi

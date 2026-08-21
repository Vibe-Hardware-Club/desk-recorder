#!/bin/bash
#
# Desk Recorder worker. Runs on your Mac under launchd, every 5 minutes.
#
# It asks the server what is waiting, joins each finished recording into one
# audio file, transcribes it with Whisper, writes a markdown transcript into
# your transcripts folder, and archives the audio.
#
# HOW "FINISHED" IS DECIDED
#
# The device gives no end-of-session signal and /pending carries no timestamps,
# so completion is inferred here: a recording is done when its chunk count has
# not changed for QUIET_SECONDS. State lives in sessions.json, keyed by session
# id, holding wall-clock times - NOT a count of worker runs, because a sleeping
# Mac would make a lunch break look like a finished meeting.
#
# The consequence, which is deliberate: a transcript appears ONCE, about twelve
# minutes after you stop recording, rather than in dribs every five.
#
# BASH 3.2 ONLY - macOS ships 3.2 and always will.

set -u

CONF="$HOME/.desk-recorder"
LOG="$CONF/worker.log"
TOKEN_FILE="$CONF/token"
STATE="$CONF/sessions.json"
LOCKDIR="$CONF/worker.lock"
SETTINGS="$CONF/config"

log() { echo "$(date '+%Y-%m-%d %H:%M:%S') $*" >> "$LOG"; }

# Written by setup.sh: PROJECT_REF and DEST_DIR.
[ -f "$SETTINGS" ] || { log "no config at $SETTINGS - run setup.sh"; exit 0; }
# shellcheck disable=SC1090
. "$SETTINGS"
: "${PROJECT_REF:?}" "${DEST_DIR:?}"

FN="https://$PROJECT_REF.supabase.co/functions/v1/transcriber-ingest"

# Tools are FOUND, not hardcoded. Homebrew lives at /opt/homebrew on Apple
# Silicon and /usr/local on Intel, and hardcoding either one is a script that
# works on the machine of whoever wrote it.
WHISPER=$(command -v whisper-cli || true)
FFMPEG=$(command -v ffmpeg || true)
MODEL="${WHISPER_MODEL:-$CONF/models/ggml-small.en.bin}"

# 12 minutes. Chunks arrive every 5, so two missed chunks means the recording
# stopped. Long enough to survive a WiFi drop mid-meeting, short enough that a
# transcript still lands while the conversation is fresh.
QUIET_SECONDS=${QUIET_SECONDS:-720}

# THE LOCK. Whisper is slower than realtime on most Macs, so a 25-minute
# meeting can still be transcribing when launchd fires again. Without this,
# runs pile up and transcribe the same recording concurrently. mkdir is atomic,
# unlike test-then-touch.
if ! mkdir "$LOCKDIR" 2>/dev/null; then
  if [ -f "$LOCKDIR/pid" ] && ! kill -0 "$(cat "$LOCKDIR/pid" 2>/dev/null)" 2>/dev/null; then
    log "stale lock from dead pid $(cat "$LOCKDIR/pid" 2>/dev/null), reclaiming"
    rm -rf "$LOCKDIR"; mkdir "$LOCKDIR" 2>/dev/null || exit 0
  else
    exit 0   # a run is genuinely in progress; say nothing, this is the normal case
  fi
fi
echo $$ > "$LOCKDIR/pid"
trap 'rm -rf "$LOCKDIR"' EXIT

[ -f "$TOKEN_FILE" ] || { log "no token file"; exit 0; }
TOKEN=$(cat "$TOKEN_FILE")
[ -n "$WHISPER" ] && [ -x "$WHISPER" ] || { log "whisper-cli not found on PATH"; exit 0; }
[ -n "$FFMPEG" ]  && [ -x "$FFMPEG" ]  || { log "ffmpeg not found on PATH"; exit 0; }
[ -f "$MODEL" ]   || { log "whisper model missing at $MODEL"; exit 0; }

# If transcripts go to Google Drive, Drive has to be RUNNING. A file written
# into the CloudStorage folder while the app is quit lands in a local replica
# and does not upload, and the folder looks completely normal while it happens.
# This cost the author most of a day, so it is checked rather than documented.
case "$DEST_DIR" in
  *"/Library/CloudStorage/GoogleDrive-"*)
    if ! pgrep -f "Google Drive" >/dev/null 2>&1; then
      log "WARNING: Google Drive is not running. Transcripts will be written but will NOT sync until you open it."
      mkdir -p "$DEST_DIR" 2>/dev/null
      echo "Google Drive was not running when a transcript was written. Open Google Drive and it should catch up. Checked $(date '+%Y-%m-%d %H:%M')." \
        > "$DEST_DIR/SYNC-WARNING.txt" 2>/dev/null
    else
      rm -f "$DEST_DIR/SYNC-WARNING.txt" 2>/dev/null
    fi
    ;;
esac

PENDING=$(curl -s --max-time 60 -H "Authorization: Bearer $TOKEN" "$FN/pending") || { log "pending fetch failed"; exit 0; }
# curl -s exits 0 on an HTTP error, so a 502 arrives here as an EMPTY string
# rather than a failure. Without this guard the grouping step dies on invalid
# JSON and the run produces nothing, with no explanation in the log.
case "$PENDING" in
  *'"pending"'*) ;;
  *) log "pending response was not JSON (${#PENDING} bytes) - skipping this run"; exit 0 ;;
esac
CATS_JSON=$(curl -s --max-time 30 -H "Authorization: Bearer $TOKEN" "$FN/categories?scope=all" || echo '')

# Group pending chunks into sessions, age them against the state file, and print
# one line per session that is ready: "<topic> <session> <file1,file2,...>".
# The pending JSON goes via a FILE, not stdin. Both the heredoc carrying this
# program and a here-string carrying the data would redirect stdin, and the last
# redirection silently wins - python then tries to execute the JSON as source and
# the grouping fails without a word in the log.
PENDING_FILE=$(mktemp)
printf '%s' "$PENDING" > "$PENDING_FILE"
READY=$(/usr/bin/python3 - "$STATE" "$QUIET_SECONDS" "$PENDING_FILE" <<'PY'
import json, re, sys, time
state_path, quiet, pending_path = sys.argv[1], int(sys.argv[2]), sys.argv[3]
pending = json.load(open(pending_path)).get('pending', [])
now = time.time()

try:
    state = json.load(open(state_path))
except Exception:
    state = {}

# <session>-pNN.wav, where session is the recording's start stamp. Chunks of one
# conversation share the prefix, which is what makes them groupable at all.
sessions = {}
for p in pending:
    name = p['name']
    m = re.match(r'^(.*)-p(\d+)\.wav$', name)
    key = '%s/%s' % (p['category'], m.group(1) if m else name[:-4])
    sessions.setdefault(key, []).append(name)

seen = {}
ready = []
for key, files in sessions.items():
    files.sort()
    prev = state.get(key)
    if prev and prev.get('count') == len(files):
        last_change = prev['last_change']
    else:
        last_change = now          # new session, or a chunk just arrived
    seen[key] = {'count': len(files), 'last_change': last_change}
    if now - last_change >= quiet:
        cat, session = key.split('/', 1)
        ready.append('%s %s %s' % (cat, session, ','.join(files)))

# Sessions that vanished from pending are finished and can leave the state file.
json.dump(seen, open(state_path, 'w'))
print('\n'.join(ready))
PY
)
rm -f "$PENDING_FILE"

[ -z "$READY" ] && exit 0
log "$(echo "$READY" | wc -l | tr -d ' ') recording(s) ready"

echo "$READY" | while read -r CAT SESSION FILES; do
  [ -n "$SESSION" ] || continue
  TMP=$(mktemp -d)
  log "recording $CAT/$SESSION ($(echo "$FILES" | tr ',' '\n' | wc -l | tr -d ' ') chunk(s))"

  # --- fetch every chunk, in order -----------------------------------------
  OK=1
  : > "$TMP/list.txt"
  for F in $(echo "$FILES" | tr ',' ' '); do
    if ! curl -s --max-time 300 --retry 3 --retry-delay 5 --retry-all-errors \
         -H "Authorization: Bearer $TOKEN" -o "$TMP/$F" "$FN/file?path=incoming/$CAT/$F"; then
      log "download failed: $F"; OK=0; break
    fi
    # An error body instead of audio is small; never feed whisper JSON.
    [ "$(stat -f%z "$TMP/$F")" -lt 1000 ] && { log "dud download: $F"; OK=0; break; }
    echo "file '$TMP/$F'" >> "$TMP/list.txt"
  done
  [ "$OK" = "1" ] || { rm -rf "$TMP"; continue; }

  # --- join into one conversation ------------------------------------------
  JOINED="$TMP/session.wav"
  if ! "$FFMPEG" -v error -y -f concat -safe 0 -i "$TMP/list.txt" -c copy "$JOINED" 2>>"$LOG"; then
    log "concat failed for $SESSION"; rm -rf "$TMP"; continue
  fi

  # --- clean it up ----------------------------------------------------------
  # This filter chain is load-bearing, not a nicety. On raw mic audio Whisper
  # invents text to fill quiet gaps: in testing it turned "test out the main
  # capabilities" into "very similar to what I was calling", and hallucinated a
  # product name that was never said. High-pass, denoise and loudness normalise
  # fixed it on the same audio with the same model.
  CLEAN="$TMP/clean.wav"
  if ! "$FFMPEG" -v error -y -i "$JOINED" \
       -af "highpass=f=90,afftdn=nf=-25,loudnorm=I=-16:TP=-1.5:LRA=11" \
       -ar 16000 -ac 1 -c:a pcm_s16le "$CLEAN" 2>>"$LOG"; then
    log "clean failed for $SESSION, transcribing raw"; CLEAN="$JOINED"
  fi

  # --- transcribe ----------------------------------------------------------
  if ! "$WHISPER" -m "$MODEL" -f "$CLEAN" -oj -ml 60 -of "$TMP/out" >>"$LOG" 2>&1; then
    log "whisper failed: $SESSION (left in incoming/ for retry)"; rm -rf "$TMP"; continue
  fi

  # --- where it goes, straight from the server ------------------------------
  FOLDER=$(echo "$CATS_JSON" | /usr/bin/python3 -c "
import json,sys
try: cats = json.load(sys.stdin).get('categories', [])
except Exception: cats = []
slug='$CAT'
c = next((c for c in cats if c.get('slug')==slug), {})
print(c.get('folder') or slug)
" 2>/dev/null || echo "$CAT")
  [ -n "$FOLDER" ] || FOLDER="$CAT"
  DEST="$DEST_DIR/$FOLDER"; mkdir -p "$DEST"

  DATEPART="${SESSION:0:10}"; TIMEPART="${SESSION:11:4}"
  if [ ${#SESSION} -ge 15 ] && [ -n "$TIMEPART" ]; then
    TITLE="$DATEPART $TIMEPART $FOLDER"
  else
    TITLE="$SESSION $FOLDER"
  fi

  # --- write the transcript -------------------------------------------------
  if ! /usr/bin/python3 - "$TMP/out.json" "$DEST/$TITLE.md" "$FOLDER" "$DATEPART" "$TIMEPART" <<'PY' 2>>"$LOG"
import json, sys, os

src, dest, topic, datepart, timepart = sys.argv[1:6]
segs = json.load(open(src)).get('transcription', [])

def seconds(seg):
    try:
        return seg['offsets']['from'] / 1000.0
    except Exception:
        return 0.0

# Paragraph on a pause. Whisper's segments are a couple of seconds each, and a
# wall of them is unreadable; a gap of more than two seconds is where a human
# would have started a new line anyway.
paras, cur, last_end = [], [], None
for s in segs:
    text = (s.get('text') or '').strip()
    if not text:
        continue
    start = seconds(s)
    if last_end is not None and start - last_end > 2.0 and cur:
        paras.append(' '.join(cur)); cur = []
    cur.append(text)
    try:
        last_end = s['offsets']['to'] / 1000.0
    except Exception:
        last_end = start
if cur:
    paras.append(' '.join(cur))

mins = int(last_end // 60) if last_end else 0
when = datepart + ((' ' + timepart[:2] + ':' + timepart[2:]) if len(timepart) == 4 else '')

os.makedirs(os.path.dirname(dest), exist_ok=True)
with open(dest, 'w') as f:
    f.write('---\n')
    f.write('topic: %s\n' % topic)
    f.write('recorded: %s\n' % when)
    f.write('duration_minutes: %d\n' % mins)
    f.write('source: knob transcriber\n')
    f.write('---\n\n')
    f.write('# %s, %s\n\n' % (topic, when))
    if not paras:
        f.write('_No speech was detected in this recording._\n')
    else:
        f.write('\n\n'.join(paras) + '\n')
PY
  then
    log "writing the transcript failed for $SESSION"; rm -rf "$TMP"; continue
  fi

  # Only once the markdown exists does anything get archived. A failed move
  # leaves chunks in incoming/ and the recording is simply redone - a duplicate
  # transcript beats a silently lost recording.
  for F in $(echo "$FILES" | tr ',' ' '); do
    curl -s --max-time 60 -X POST -H "Authorization: Bearer $TOKEN" \
      "$FN/done?path=incoming/$CAT/$F" | grep -q '"ok"' || log "archive failed: $F"
  done
  log "done: $CAT/$SESSION -> $DEST/$TITLE.md"
  rm -rf "$TMP"
done

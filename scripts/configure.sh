#!/usr/bin/env bash
#
# Send WiFi, host and token to a flashed device over the USB cable.
#
# The device stores these in NVS and reboots. Nothing is compiled in, so the
# same published binary works for everybody.
#
# Values are never passed as arguments. Anything on a command line shows up in
# `ps` for every other process on the machine, and one of these is a bearer
# token. They are prompted for, or taken from the environment when this is
# driven by setup.sh.
#
#   ./configure.sh              prompts for everything
#   KNOB_SSID=... KNOB_PASS=... KNOB_HOST=... KNOB_TOKEN=... ./configure.sh
#
# BASH 3.2 ONLY. macOS still ships bash 3.2 as /bin/bash and always will, for
# licensing reasons. So: no mapfile, no associative arrays, no fractional
# `read -t`. Anything newer works on the machine of whoever wrote it and fails
# on the machine of whoever downloaded it.
#
set -u

PORT="${KNOB_PORT:-}"

# ---- find the device -------------------------------------------------------
if [ -z "$PORT" ]; then
  PORTS=()
  while IFS= read -r p; do
    [ -n "$p" ] && PORTS+=("$p")
  done < <(ls /dev/cu.usbmodem* 2>/dev/null)
  if [ "${#PORTS[@]}" -eq 0 ]; then
    echo "No device found."
    echo
    echo "This board has two chips behind one USB-C port and only one of them"
    echo "answers, depending on which way up the plug is. Unplug it, turn the"
    echo "plug over, plug it back in, and run this again."
    exit 1
  elif [ "${#PORTS[@]}" -gt 1 ]; then
    echo "More than one device is plugged in:"
    for i in "${!PORTS[@]}"; do echo "  $((i+1))) ${PORTS[$i]}"; done
    printf "Which one is the recorder? [1-%d] " "${#PORTS[@]}"
    read -r pick
    PORT="${PORTS[$((pick-1))]}"
  else
    PORT="${PORTS[0]}"
  fi
fi
[ -c "$PORT" ] || { echo "Not a serial device: $PORT"; exit 1; }
echo "Device: $PORT"

# ---- collect the values ----------------------------------------------------
SSID="${KNOB_SSID:-}"
PASS="${KNOB_PASS:-}"
HOST="${KNOB_HOST:-}"
TOKEN="${KNOB_TOKEN:-}"

[ -n "$SSID" ]  || { printf "WiFi network name: "; read -r SSID; }
# -s so it never appears on screen or in a screen recording, which matters
# because people film this sort of thing.
[ -n "$PASS" ]  || { printf "WiFi password: "; read -rs PASS; echo; }
[ -n "$HOST" ]  || { printf "Ingest host (e.g. abcdef.supabase.co): "; read -r HOST; }
[ -n "$TOKEN" ] || { printf "Device token: "; read -rs TOKEN; echo; }

for v in SSID HOST TOKEN; do
  [ -n "${!v}" ] || { echo "$v cannot be empty."; exit 1; }
done

# ---- talk to it ------------------------------------------------------------
# -hupcl matters: without it, closing the port can reset the board mid-write.
stty -f "$PORT" 115200 raw -echo -hupcl 2>/dev/null || {
  echo "Could not open $PORT. Is a serial monitor already connected to it?"
  exit 1
}

exec 3<>"$PORT" || { echo "Could not open $PORT for read/write."; exit 1; }
sleep 1
# Drain anything the device was already saying.
while IFS= read -r -t 1 -u 3 _drain; do :; done

send() { printf '%s\r\n' "$1" >&3; sleep 0.3; }

send "SET ssid $SSID"
send "SET pass $PASS"
send "SET host $HOST"
send "SET token $TOKEN"

# Read back what it thinks it has. Lengths only, never values - the firmware
# will not print a token back and neither will this.
send "STATUS"

READY=""
while IFS= read -r -t 3 -u 3 line; do
  line="${line%$'\r'}"
  case "$line" in
    ERR*)              echo "  device said: $line" ;;
    "STATUS ready "*)  READY="${line##* }" ;;
    STATUS*)           echo "  ${line#STATUS }" ;;
  esac
done

if [ "$READY" != "yes" ]; then
  echo
  echo "The device does not have everything it needs. Nothing has been saved."
  exec 3<&-
  exit 1
fi

send "SAVE"
SAVED=""
while IFS= read -r -t 5 -u 3 line; do
  line="${line%$'\r'}"
  case "$line" in
    "OK saved")     SAVED=yes ;;
    "OK rebooting") break ;;
    ERR*)           echo "  device said: $line" ;;
  esac
done
exec 3<&-

if [ "$SAVED" != "yes" ]; then
  echo "Save was not confirmed. Try again, and if it keeps happening see docs/TROUBLESHOOTING.md."
  exit 1
fi

echo
echo "Saved. The device is rebooting and should join your WiFi in a few seconds."

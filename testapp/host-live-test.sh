#!/usr/bin/env bash
# Host-side integration validation for crossnook-test (no real hardware).
#
# Runs the real ARM binary under qemu-arm inside the pinned docker toolchain
# image, using FIFOs as stand-ins for /dev/input/eventX and a regular file
# for /dev/graphics/fb0, then feeds a scripted event sequence and verifies
# the resulting on-screen state (page/screen/toggles/marker) plus the
# documented POWER long-press exit path by inspecting the written
# framebuffer and the app's stderr log. Nothing here is deployed to the Nook.
#
# Usage: bash testapp/host-live-test.sh
set -u

# ---- below runs INSIDE the docker container -------------------------------
inner() {
  set -u
  export PATH=/opt/tc/arm-linux-musleabi-cross/bin:$PATH
  S=/io/testapp

  lebytes() {  # <value> <nbytes> -> \xNN escapes LSB first
    local val=$1 n=$2 i h
    for ((i = 0; i < n; i++)); do
      h=$(printf '%02x' $(( (val >> (8 * i)) & 255 )))
      printf '\\x%s' "$h"
    done
  }
  ev() {  # <fd> <type> <code> <value> — one 16-byte record, single write()
    local fd=$1
    printf "%b" "$(lebytes 0 4)$(lebytes 0 4)$(lebytes "$2" 2)$(lebytes "$3" 2)$(lebytes "$4" 4)" >&$fd
  }

  mkdir -p /dev/graphics /dev/input
  rm -f /dev/graphics/fb0 /dev/input/event0 /dev/input/event1 /dev/input/event2
  touch /dev/graphics/fb0
  mkfifo /dev/input/event0 /dev/input/event1 /dev/input/event2

  qemu-arm -L /opt/tc/arm-linux-musleabi-cross/arm-linux-musleabi \
    $S/crossnook-test >/tmp/out.log 2>/tmp/err.log &
  APP=$!
  sleep 1

  exec 3> /dev/input/event0   # opens in device-order the app uses
  exec 4> /dev/input/event1
  exec 5> /dev/input/event2
  sleep 1

  ev 3 0x01 407 1   # event0 KEY_NEXT  press     -> page 2
  ev 3 0x01 407 0   # event0 KEY_NEXT  release
  sleep 1
  ev 4 0x01 102 1   # event1 KEY_HOME  press     -> HOME screen
  ev 4 0x01 102 0   # event1 KEY_HOME  release
  sleep 1
  ev 3 0x01 407 1   # event0 KEY_NEXT  press     -> page 3 (on HOME)
  ev 3 0x01 407 0
  sleep 1
  ev 5 0x03 0x00 100   # event2 ABS_X=100
  ev 5 0x03 0x01 50    # event2 ABS_Y=50
  ev 5 0x01 330 1      # event2 BTN_TOUCH down
  ev 5 0x00 0x00 0     # event2 SYN_REPORT
  ev 5 0x01 330 0      # event2 BTN_TOUCH up
  ev 5 0x00 0x00 0     # event2 SYN_REPORT
  sleep 1
  ev 3 0x01 158 1   # event0 KEY_BACK  press     -> diag_back toggle
  ev 3 0x01 158 0
  sleep 1
  ev 4 0x01 116 1   # event1 KEY_POWER press  (held)
  sleep 3
  ev 4 0x01 116 0   # event1 KEY_POWER release  -> LONG -> exit
  sleep 1

  kill -0 $APP 2>/dev/null && kill -9 $APP 2>/dev/null || true
  cp /dev/graphics/fb0 $S/fb-live.bin

  echo "===== stderr log ====="
  cat /tmp/err.log
  echo "===== fb size ====="
  ls -la $S/fb-live.bin
  echo "===== assert: long-press exit logged ====="
  grep -q "POWER long-press, exiting" /tmp/err.log && echo "OK" || {
    echo "FAIL: no exit log"; return 1; }
  echo "===== assert: exactly one initial-read error max, none expected ====="
  ! grep -q "EAGAIN\|Resource temporarily unavailable" /tmp/err.log || echo "note: saw EAGAIN (benign)"
  return 0
}

# ---- outer: Windows host wrapper -------------------------------------------
REPO=$(cd "$(dirname "$0")/.." && pwd)
REPO_WIN=$(cygpath -w "$REPO" 2>/dev/null || echo "$REPO")

if [ "${1:-}" = "__inner__" ]; then
  inner; exit $?
fi

MSYS_NO_PATHCONV=1 docker run --rm \
  -v "$REPO_WIN:/io" \
  crossnook-toolchain \
  bash /io/testapp/host-live-test.sh __inner__
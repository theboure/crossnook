#!/usr/bin/env bash
# Host-side integration validation of crossnook-ui-test (no real hardware).
#
# Like host-live-test.sh: run the real ARM binary under qemu-arm inside the
# pinned docker toolchain, using FIFOs as stand-ins for /dev/input/eventX and
# a regular file for /dev/graphics/fb0. Feeds a scripted event sequence and
# verifies the UI state transitions (HOME -> READER page n, back to HOME),
# touch gesture coordinate stabilization (transient first/release frames must
# never produce a committed tap coordinate; TOUCH_UP resolves to the last
# in-contact coordinate), marker commits on release, the semantic INPUT
# traces (MENU/BACK/HOME/POWER_DOWN/POWER_UP) and the long-POWER exit via
# the app's stdout/stderr logs.
#
# Usage: bash testapp/host-live-ui-test.sh
set -u

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
    $S/crossnook-ui-test /io/testapp/test-font.ttf >/tmp/ui.out.log 2>/tmp/ui.err.log &
  APP=$!
  sleep 1

  exec 3> /dev/input/event0
  exec 4> /dev/input/event1
  exec 5> /dev/input/event2
  sleep 1

  ev 3 0x01 407 1   # event0 KEY_NEXT  press      -> READER page 2
  ev 3 0x01 407 0
  sleep 1
  ev 3 0x01 407 1   #                            -> READER page 3
  ev 3 0x01 407 0
  sleep 1
  ev 3 0x01 412 1   # event0 KEY_PREVIOUS press   -> READER page 2
  ev 3 0x01 412 0
  sleep 1
  ev 4 0x01 102 1   # event1 KEY_HOME  press      -> HOME
  ev 4 0x01 102 0
  sleep 1
  # tap #1: normal single-frame tap — DOWN and UP carry the same coords;
  # resolved coordinate must be (51,33) (registered on TOUCH_UP only).
  ev 5 0x03 0x00 51    # event2 ABS_X=51
  ev 5 0x03 0x01 33    # event2 ABS_Y=33
  ev 5 0x01 330 1      # event2 BTN_TOUCH down
  ev 5 0x00 0x00 0     # event2 SYN -> TOUCH_DOWN (provisional, not logged)
  ev 5 0x03 0x00 51
  ev 5 0x03 0x01 33
  ev 5 0x01 330 0      # event2 BTN_TOUCH up
  ev 5 0x00 0x00 0     # event2 SYN -> TOUCH_UP resolved (51,33)
  sleep 1

  # tap #2: bottom-edge transient. Physical touch is near (298,780) but the
  # very first and the release frames carry a transient Y near the opposite
  # edge (300,9). The resolved coordinate MUST be ~(301,781), never (300,9).
  ev 5 0x03 0x00 300
  ev 5 0x03 0x01 9
  ev 5 0x01 330 1
  ev 5 0x00 0x00 0     # SYN -> TOUCH_DOWN (300,9) provisional
  ev 5 0x03 0x00 301
  ev 5 0x03 0x01 781
  ev 5 0x00 0x00 0     # SYN -> TOUCH_MOVE (301,781)
  ev 5 0x03 0x00 301
  ev 5 0x03 0x01 780
  ev 5 0x00 0x00 0     # SYN -> TOUCH_MOVE (301,780)
  ev 5 0x03 0x00 301
  ev 5 0x03 0x01 781
  ev 5 0x00 0x00 0     # SYN -> TOUCH_MOVE (301,781), latest in-contact
  ev 5 0x03 0x00 300   # release frame re-carries the transient coords
  ev 5 0x03 0x01 9
  ev 5 0x01 330 0
  ev 5 0x00 0x00 0     # SYN -> TOUCH_UP resolved (301,781)
  sleep 1

  # tap #3: multiple in-contact moves; must resolve to the coordinate of the
  # last in-contact frame before release.
  ev 5 0x03 0x00 100
  ev 5 0x03 0x01 200
  ev 5 0x01 330 1
  ev 5 0x00 0x00 0     # SYN -> TOUCH_DOWN (100,200) provisional
  ev 5 0x03 0x00 150
  ev 5 0x03 0x01 250
  ev 5 0x00 0x00 0     # SYN -> TOUCH_MOVE (150,250)
  ev 5 0x03 0x00 220
  ev 5 0x03 0x01 320
  ev 5 0x00 0x00 0     # SYN -> TOUCH_MOVE (220,320)
  ev 5 0x03 0x00 220
  ev 5 0x03 0x01 320
  ev 5 0x01 330 0
  ev 5 0x00 0x00 0     # SYN -> TOUCH_UP resolved (220,320)
  sleep 1
  ev 3 0x01 407 1   # NEXT from HOME -> READER page 3 (2 + 1)
  ev 3 0x01 407 0
  sleep 1
  ev 3 0x01 158 1   # event0 KEY_BACK -> HOME (trace INPUT BACK)
  ev 3 0x01 158 0
  sleep 1
  ev 3 0x01 139 1   # event0 KEY_MENU press -> recognized, intentional no-op
  ev 3 0x01 139 0
  sleep 1
  ev 4 0x01 116 1   # event1 KEY_POWER press (held)
  sleep 3
  ev 4 0x01 116 0   # event1 KEY_POWER release -> LONG -> exit
  sleep 1

  kill -0 $APP 2>/dev/null && kill -9 $APP 2>/dev/null || true
  cp /dev/graphics/fb0 $S/fb-ui-live.bin

  echo "===== ui stdout log ====="
  cat /tmp/ui.out.log
  echo "===== ui stderr log ====="
  cat /tmp/ui.err.log

  L=/tmp/ui.out.log
  LERR=/tmp/ui.err.log
  fail=0
  assert() {  # <desc> <grep pattern>
    if grep -q "$2" "$1"; then echo "OK   $2"; else echo "FAIL $2"; fail=1; fi
  }
  not_assert() {  # <desc> <grep pattern> — pattern must be absent
    if grep -q "$2" "$1"; then echo "FAIL(absent) $2"; fail=1; else echo "OK   !$2"; fi
  }

  echo "===== asserts ====="
  assert $L "UI initial state=HOME page=1 inputs=3"
  assert $L "UI state=READER page=2"
  assert $L "UI state=READER page=3"
  assert $L "UI state=READER page=2"
  assert $L "UI state=HOME"          # after KEY_HOME
  assert $L "UI touch=51,33"         # normal tap resolved to the down coords
  assert $L "UI touch=301,781"       # transient border tap: resolved Y ~781
  assert $L "UI touch=220,320"       # multi-move: last in-contact coordinate
  assert $L "UI state=READER page=3" # NEXT from HOME (2 + 1)
  assert $L "UI long-power exit"
  not_assert $L "UI touch=300,9"     # transient first/release Y never used
  not_assert $L "UI touch=100,50"    # provisional DOWN coord never committed

  echo "===== semantic INPUT traces (stderr) ====="
  assert $LERR "INPUT MENU"
  assert $LERR "INPUT BACK"
  assert $LERR "INPUT HOME"
  assert $LERR "INPUT POWER_DOWN"
  assert $LERR "INPUT POWER_UP"

  echo "===== fb size (final power-up frame is the HOME screen) ====="
  ls -la $S/fb-ui-live.bin
  FB_SIZE=$(stat -c %s $S/fb-ui-live.bin 2>/dev/null || echo 0)
  if [ "$FB_SIZE" = "960000" ]; then echo "OK   fb frame 960000 bytes"; else
    echo "FAIL fb frame size $FB_SIZE"; fail=1; fi

  # no input device open errors expected
  ! grep -q "input: open\|Resource temporarily unavailable" /tmp/ui.err.log \
    || echo "note: input open warning(s) seen"

  return $fail
}

REPO=$(cd "$(dirname "$0")/.." && pwd)
REPO_WIN=$(cygpath -w "$REPO" 2>/dev/null || echo "$REPO")

if [ "${1:-}" = "__inner__" ]; then
  inner; exit $?
fi

MSYS_NO_PATHCONV=1 docker run --rm \
  -v "$REPO_WIN:/io" \
  crossnook-toolchain \
  bash /io/testapp/host-live-ui-test.sh __inner__
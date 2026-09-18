#!/usr/bin/env bash
# Host-side integration validation of crossnook-library-test (no hardware).
#
# Runs the real ARM binary under qemu-arm inside the pinned docker toolchain
# with input FIFO stand-ins + a file fb0, exactly like host-live-ui-test.sh,
# and scripts two full sessions:
#
#   Session A (fixtures dir): HOME shows books=41; tap the "[ Open library ]"
#     button; browse with PAGE_NEXT/PAGE_PREV; tap a row to select, tap again
#     to open the Selected Book screen (back returns to the same selection);
#     scroll all the way down (sel=40 top=27 no-op at the end); HOME; exit.
#   Session B (empty dir): books=0; the library renders the empty state
#     (sel=0 top=0 of=0) and never leaves LIBRARY; exit.
#
# Both sessions end with a long-POWER exit and a 960000-byte framebuffer.
#
# The FIFOs are opened with O_RDWR (<>), so neither side ever blocks waiting
# for the other to open the FIFO first: the harness is independent of the
# order in which the device driver opens /dev/input/eventN. A per-session
# watchdog kills the qemu child if a scripted event hangs, and an early
# qemu exit before the devices are opened is detected and reported instead
# of leaving the FIFO open blocked forever.
#
# Usage: bash testapp/host-live-library-test.sh
set -u

inner() {
  set -u
  export PATH=/opt/tc/arm-linux-musleabi-cross/bin:$PATH
  S=/io/testapp
  fail_all=0

  # Regenerate the fixtures deterministically before scripting sessions.
  bash /io/testapp/library-fixtures/make-fixtures.sh

  # Never leave device stand-ins behind.
  trap 'rm -f /dev/graphics/fb0 /dev/input/event0 /dev/input/event1 /dev/input/event2 2>/dev/null' EXIT

  lebytes() {  # <value> <nbytes> -> \xNN escapes LSB first
    local val=$1 n=$2 i h
    for ((i = 0; i < n; i++)); do
      h=$(printf '%02x' $(( (val >> (8 * i)) & 255 )))
      printf '\\x%s' "$h"
    done
  }
  ev() {  # <fd> <type> <code> <value>
    local fd=$1
    printf "%b" "$(lebytes 0 4)$(lebytes 0 4)$(lebytes "$2" 2)$(lebytes "$3" 2)$(lebytes "$4" 4)" >&$fd
  }

  mkdir -p /dev/graphics /dev/input
  rm -f /dev/graphics/fb0 /dev/input/event0 /dev/input/event1 /dev/input/event2
  touch /dev/graphics/fb0
  mkfifo /dev/input/event0 /dev/input/event1 /dev/input/event2

  tap() {  # <x> <y> — one DOWN + one UP frame
    local x=$1 y=$2
    ev 5 0x03 0x00 $x
    ev 5 0x03 0x01 $y
    ev 5 0x01 330 1
    ev 5 0x00 0x00 0
    ev 5 0x03 0x00 $x
    ev 5 0x03 0x01 $y
    ev 5 0x01 330 0
    ev 5 0x00 0x00 0
    sleep 1
  }

  # session <dir> <suffix> — boots the app and runs the scripted party.
  session() {
    local dir=$1 sfx=$2 app= wd= fail=0
    qemu-arm -L /opt/tc/arm-linux-musleabi-cross/arm-linux-musleabi \
      $S/crossnook-library-test /io/testapp/test-font.ttf "$dir" \
      >/tmp/out-$sfx.log 2>/tmp/err-$sfx.log &
    app=$!

    # Watchdog: if the event party ever stalls, kill the child. 180s would
    # only trip on a regression; a correct run finishes in well under that.
    ( sleep 180; kill -9 $app 2>/dev/null ) &
    wd=$!
    killwd() { kill $wd 2>/dev/null; wait $wd 2>/dev/null || true; }

    # Open the device FIFOs O_RDWR. This never blocks, so the harness works
    # no matter when (or whether) the app opens /dev/input/eventN for read.
    exec 3<> /dev/input/event0
    exec 4<> /dev/input/event1
    exec 5<> /dev/input/event2
    sleep 1

    L=/tmp/out-$sfx.log
    LE=/tmp/err-$sfx.log
    assert() {
      if grep -q "$2" "$1"; then echo "OK   $2"; else echo "FAIL $2"; fail=1; fi
    }

    if ! kill -0 $app 2>/dev/null; then
      echo "FAIL: app exited before the event session started ($sfx)"
      echo "----- stderr -----"; cat $LE
      killwd
      exec 3>&- 4>&- 5>&-
      return 1
    fi

    echo "===== session $sfx: events ====="
    if [ "$sfx" = fixtures ]; then
      tap 200 320                  # enter library via the [ Open library ] button
      ev 3 0x01 407 1              # PAGE_NEXT -> sel=1
      ev 3 0x01 407 0
      sleep 1
      for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13; do  # ...x13 -> sel=14 top=14
        ev 3 0x01 407 1
        ev 3 0x01 407 0
        sleep 1
      done
      ev 3 0x01 412 1              # PAGE_PREV -> sel=13 top=13
      ev 3 0x01 412 0
      sleep 1
      ev 3 0x01 407 1              # -> sel=14 top=14 again
      ev 3 0x01 407 0
      sleep 1
      tap 200 298                  # row r=3 -> select viewport index 16
      tap 200 298                  # same row again -> SELECTED_BOOK
      ev 3 0x01 158 1              # BACK -> LIBRARY, selection preserved
      ev 3 0x01 158 0
      sleep 1
      for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23; do
        ev 3 0x01 407 1            # x23 -> sel=40 top=27 (last page)
        ev 3 0x01 407 0
        sleep 1
      done
      ev 3 0x01 407 1              # one more: no-op past the end
      ev 3 0x01 407 0
      sleep 1
      ev 4 0x01 102 1              # HOME
      ev 4 0x01 102 0
      sleep 1
    else
      tap 200 320                  # enter the (empty) library
    fi

    ev 4 0x01 116 1               # POWER hold
    sleep 3
    ev 4 0x01 116 0               # release -> long-POWER exit
    sleep 1

    killwd
    # Terminate deterministically and reap the child before copying fb0.
    kill -0 $app 2>/dev/null && kill -9 $app 2>/dev/null || true
    wait $app 2>/dev/null || true
    cp /dev/graphics/fb0 $S/fb-library-$sfx.bin
    exec 3>&- 4>&- 5>&-

    echo "===== stdout ($sfx) ====="
    cat $L
    echo "===== stderr ($sfx) ====="
    cat $LE

    echo "===== asserts ($sfx) ====="
    if [ "$sfx" = fixtures ]; then
      assert $L "UI initial state=HOME page=1 inputs=3 books=41"
      assert $L "UI state=LIBRARY"
      assert $L "UI library sel=0 top=0 of=41 rows=14"
      assert $L "UI library sel=1 top=0 of=41 rows=14"
      assert $L "UI library sel=14 top=14 of=41 rows=14"
      assert $L "UI library sel=13 top=13 of=41 rows=14"
      assert $L "UI library sel=14 top=13 of=41 rows=14"
      assert $L "UI library sel=16 top=13 of=41 rows=14"
      assert $L "UI state=SELECTED"
      assert $L 'UI selected title="charms" format=TXT'
      assert $L "UI library sel=40 top=27 of=41 rows=14"
      assert $L "UI state=HOME"
      assert $LE "INPUT BACK"
      assert $LE "INPUT HOME"
    else
      assert $L "UI initial state=HOME page=1 inputs=3 books=0"
      assert $L "UI state=LIBRARY"
      assert $L "UI library sel=0 top=0 of=0 rows=14"
    fi
    assert $L "UI long-power exit"
    assert $LE "INPUT POWER_DOWN"
    assert $LE "INPUT POWER_UP"

    s=$(stat -c %s $S/fb-library-$sfx.bin 2>/dev/null || echo 0)
    if [ "$s" = "960000" ]; then echo "OK   fb frame 960000 bytes"; else
      echo "FAIL fb frame size $s"; fail=1; fi
    return $fail
  }

  echo "===== session fixtures (direct) ====="
  session /io/testapp/library-fixtures fixtures
  r1=$?
  rm -rf /tmp/books-empty && mkdir /tmp/books-empty
  session /tmp/books-empty empty
  r2=$?
  [ $r1 -eq 0 ] && [ $r2 -eq 0 ]
}

REPO=$(cd "$(dirname "$0")/.." && pwd)
REPO_WIN=$(cygpath -w "$REPO" 2>/dev/null || echo "$REPO")

if [ "${1:-}" = "__inner__" ]; then
  inner; exit $?
fi

MSYS_NO_PATHCONV=1 docker run --rm \
  -v "$REPO_WIN:/io" \
  crossnook-toolchain \
  bash /io/testapp/host-live-library-test.sh __inner__
#!/system/bin/sh
# inputlog.sh - log decoded input events from a Nook Simple Touch event device.
#
# Pure busybox (sh + od), no python/perl/dev-side toolchain required.
# Kernel evdev delivers whole 16-byte input_event records per read():
#   timeval(8: tv_sec + tv_usec)  type(u16)  code(u16)  value(i32)
# all native (ARM little-endian).  od -td4 on exactly 16 bytes prints:
#   sec  usec  type|(code<<16)  value
#
# Usage (adbd runs as root on the NM diagnostic image):
#   inputlog.sh /dev/input/eventN        stream decoded events to stdout
#   inputlog.sh --raw /dev/input/eventN  raw hex bytes, one event per line
#   inputlog.sh all [logfile]            background event0..2 to a merged log
#
# Press one physical control at a time; the KEY/ABS code column tells you
# which event device + code that control maps to.

# --- event type -> name (linux/input-event-codes.h) ---
tname() {
  case "$1" in
    0) echo EV_SYN ;;
    1) echo EV_KEY ;;
    2) echo EV_REL ;;
    3) echo EV_ABS ;;
    4) echo EV_MSC ;;
    5) echo EV_SW ;;
    *) echo "EV_$1" ;;
  esac
}

# --- key codes (Nook-relevant subset; full list in linux/keyboard.h input) ---
kname() {
  case "$1" in
    1)  echo KEY_ESC ;;
    2)  echo KEY_1 ;;
    3)  echo KEY_2 ;;
    28) echo KEY_ENTER ;;
    57) echo KEY_SPACE ;;
    102) echo KEY_HOME ;;          # Nook home button candidate
    104) echo KEY_PAGEUP ;;        # page-turn candidates
    109) echo KEY_PAGEDOWN ;;
    105) echo KEY_LEFT ;;
    106) echo KEY_RIGHT ;;
    103) echo KEY_UP ;;
    108) echo KEY_DOWN ;;
    116) echo KEY_POWER ;;         # power button candidate
    114) echo KEY_VOLUMEDOWN ;;
    115) echo KEY_VOLUMEUP ;;
    330) echo BTN_TOUCH ;;
    325) echo BTN_TOOL_FINGER ;;
    *)  echo "KEY_$1" ;;
  esac
}

# --- abs codes (touchscreen MT protocol A/B) ---
aname() {
  case "$1" in
    0)  echo ABS_X ;;              # legacy single-touch X
    1)  echo ABS_Y ;;              # legacy single-touch Y
    24) echo ABS_PRESSURE ;;
    28) echo ABS_MISC ;;
    47) echo ABS_MT_SLOT ;;
    48) echo ABS_MT_TOUCH_MAJOR ;;
    49) echo ABS_MT_TOUCH_MINOR ;;
    50) echo ABS_MT_WIDTH_MAJOR ;;
    51) echo ABS_MT_WIDTH_MINOR ;;
    52) echo ABS_MT_ORIENTATION ;;
    53) echo ABS_MT_POSITION_X ;;
    54) echo ABS_MT_POSITION_Y ;;
    55) echo ABS_MT_TOOL_TYPE ;;
    56) echo ABS_MT_BLOB_ID ;;
    57) echo ABS_MT_TRACKING_ID ;;
    58) echo ABS_MT_PRESSURE ;;
    *)  echo "ABS_$1" ;;
  esac
}

cname() {
  case "$1" in
    0) echo SYN_REPORT ;;
    1) echo SYN_CONFIG ;;
    4) echo MSC_SCAN ;;
    *) echo "C$1" ;;
  esac
}

# decode from positional args: $1 sec $2 usec $3 comb( type|code<<16 ) $4 val
fmt_line() {
  TYPE=$(( $3 & 0xFFFF ))
  CODE=$(( ($3 >> 16) & 0xFFFF ))
  case "$TYPE" in
    1) N=$(kname "$CODE") ;;
    3) N=$(aname "$CODE") ;;
    *) N=$(cname "$CODE") ;;
  esac
  printf "%d.%06d %-7s %-22s value=%d\n" "$1" "$2" "$(tname "$TYPE")" "$N" "$4"
}

# read one record from stdin, decode, print, repeat
stream() {
  while :; do
    set -- $(dd bs=16 count=1 2>/dev/null | od -An -td4 -v)
    [ -z "$1" ] && exit 0
    fmt_line "$1" "$2" "$3" "$4"
  done
}

stream_raw() {
  while :; do
    set -- $(dd bs=16 count=1 2>/dev/null | od -An -tx1 -v)
    [ -z "$1" ] && exit 0
    echo "$*"
  done
}

case "$1" in
  "--raw")
    stream_raw < "$2"
    ;;
  "all")
    LOG="${2:-/tmp/input.log}"
    : > "$LOG"
    emitter() {
      LBL="$1"
      while :; do
        set -- $(dd bs=16 count=1 2>/dev/null | od -An -td4 -v)
        [ -z "$1" ] && exit 0
        LINE=$(fmt_line "$1" "$2" "$3" "$4")
        printf "[%s] %s\n" "$LBL" "$LINE" >> "$LOG"
      done
    }
    emitter event0 < /dev/input/event0 &
    P0=$!
    emitter event1 < /dev/input/event1 &
    P1=$!
    emitter event2 < /dev/input/event2 &
    P2=$!
    echo "logging event0/1/2 -> $LOG  (pids $P0 $P1 $P2)"
    echo "press each control one at a time; Ctrl-C to stop."
    wait
    ;;
  "")
    echo "usage: $0 /dev/input/eventN | --raw /dev/input/eventN | all [logfile]"
    exit 1
    ;;
  *)
    stream < "$1"
    ;;
esac
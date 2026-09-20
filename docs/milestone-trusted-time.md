# Trusted Time Foundation

**Status: HOST VALIDATION PASS; HARDWARE VALIDATION PASS.**

This milestone adds a minimal bounded SNTPv4 bootstrap for establishing a sane
system wall clock before verified HTTPS. It does not change TLS, certificate,
hostname, or certificate-time verification.

## Hardware Basis

Read-only reconnaissance on the physical Nook Simple Touch established:

- Linux `2.6.29-omap1` on ARMv7;
- NookManager `adbd` runs as root with effective `CAP_SYS_TIME`;
- `/dev/rtc0` and `/sys/class/rtc/rtc0` exist;
- RTC driver name is `twl4030_rtc`;
- the RTC advances but contains stale year-2000 time; and
- no useful stock `ntpd`, `ntpclient`, `rdate`, or `adjtimex` is available.

The kernel log explicitly reports:

```text
twl4030_rtc twl4030_rtc: rtc core: registered twl4030_rtc as rtc0
twl4030_rtc twl4030_rtc: setting system clock to 2000-01-03 16:18:05 UTC (...)
```

The kernel therefore copies stale RTC time into `CLOCK_REALTIME` at boot. The
RTC is not a trusted time source and is neither read nor written by the new
module. Phase 2 intentionally does not repair it.

## Architecture

`src/time/timesimple.[ch]` is standalone and uses direct UDP/IPv4. It does not
sit inside `netsimple`, which continues to own HTTP over TCP or verified TLS.

```text
Wi-Fi ready
    -> cn_timesimple_sync()
    -> validated SNTP candidate
    -> clock_settime(CLOCK_REALTIME)
    -> existing HTTPS/KOSync may proceed
```

`cn_timesimple_query()` runs the complete request and validation path without
changing wall time. It exists for deterministic host tests and diagnostics.
`cn_timesimple_sync()` succeeds only after the same query path and a successful
`clock_settime(CLOCK_REALTIME, ...)`. Failure does not authorize HTTPS.

Configuration contains at most four numeric IPv4 server addresses, a shared
UDP port, and a per-server timeout. Port zero selects production port 123.
Servers are attempted sequentially. There is no DNS, background operation,
periodic synchronization, or persistent state.

## Protocol Bounds

Each attempt sends one 48-byte SNTPv4 client packet with LI 0, version 4, and
mode 3. Its Transmit Timestamp contains the current, possibly wrong, wall-clock
seconds plus unpredictable fractional bits read from `/dev/urandom`. Entropy
failure is fatal.

The client uses a nonblocking `AF_INET`/`SOCK_DGRAM` socket, `poll()`, and an
absolute `CLOCK_MONOTONIC` deadline. A response is accepted only when:

- it is exactly 48 bytes;
- its IPv4 source address and UDP source port match the configured peer;
- version is 4 and server mode is 4;
- Leap Indicator is not 3;
- stratum is 1 through 15;
- Originate Timestamp exactly matches the request Transmit Timestamp;
- Receive and Transmit Timestamps are nonzero; and
- the converted server Transmit Timestamp is inside the trusted target range.

The estimate is the server Transmit Timestamp plus half the bounded monotonic
round trip. This is sufficient for certificate validity checks; it is not a
full NTP clock-discipline algorithm.

## Reproducible Time Bounds

The compiled lower bound is Unix `1789908147`, or
`2026-09-20T12:42:27Z`. This is the published HTTPS baseline commit time for
`f229d434771bdd210e464ce494f61d08e3add097`. It is explicit source-controlled
release metadata, not `__DATE__`, `__TIME__`, device time, or a runtime option.

The upper bound is Unix `2147483647`, or `2038-01-19T03:14:07Z`, because the
32-bit Linux 2.6.29 target uses the legacy signed kernel wall-clock ABI. NTP
era 0 and era 1 candidates are computed explicitly with 64-bit arithmetic;
exactly one candidate must fall inside the compiled range. There is no hidden
post-2038 assumption.

## Trust Model

Unauthenticated SNTP is a bootstrap mechanism, not authenticated time.

It protects against a stale RTC/system clock, malformed responses, unrelated
or most off-path responses through peer and request correlation, and rollback
earlier than the compiled release lower bound.

It does not protect against an active on-path attacker able to replace UDP
traffic, a compromised configured NTP server, cryptographic server
impersonation, or denial of synchronization. TLS verification remains fully
enabled after synchronization. NTS and multiple-server consensus are not
implemented.

## Host Gate

Run:

```bash
bash testapp/build-time.sh
```

The gate builds `testapp/crossnook-time-test` as static ARM EABI5 soft-float
non-PIE and runs it under `qemu-arm` against a deterministic local UDP mock on
nonprivileged ports. It never invokes `--sync` and cannot change the host
clock.

The passing cases are:

```text
valid                         -> OK, Unix 1790000000
originate mismatch            -> originate-mismatch
truncated response            -> packet-size
oversized response            -> packet-size
LI=3                          -> unsynchronized
stratum 0                     -> kiss-of-death
reserved stratum              -> stratum
zero Receive Timestamp        -> zero-receive
zero Transmit Timestamp       -> zero-transmit
time below release bound      -> time-range
time above target-safe bound  -> time-range
no response                   -> timeout
wrong peer response           -> rejected/timeout at Docker boundary
wrong version                 -> version
wrong mode                    -> mode
first server fails, second OK -> OK, server index 1
TIME HOST VALIDATION OK
```

## Physical Nook Validation

The controlled mock returns Unix `1790000000`
(`2026-09-21T14:13:20Z`), inside the TEST-ONLY HTTPS certificate's validity
window. Validation began after a normal reboot, with uptime around 23 seconds.
The system clock was still in January 2000, `/proc/driver/rtc` reported
`rtc_date : 2000-01-03`, Wi-Fi had `192.168.0.104/24`, and a default route was
present.

The physical Nook then queried the controlled Windows PC mock over UDP/IPv4 on
port 19123 and explicitly applied the validated result:

```text
TIMETEST SYNC OK unix=1790000000 nsec=60882571 rtt_ms=122 server=0
```

Immediately afterward, `date -u` reported:

```text
Mon Sep 21 14:13:28 UTC 2026
```

The existing verified HTTPS diagnostic was then run without any TLS code or
policy change:

```text
NETTEST HTTPS GET 200 23 184 OK
```

This proves the complete physical-hardware path:

```text
stale twl4030_rtc
    -> stale year-2000 boot CLOCK_REALTIME
    -> Wi-Fi
    -> validated SNTP over UDP/IPv4
    -> clock_settime(CLOCK_REALTIME)
    -> sane wall clock
    -> unchanged BearSSL/X.509 HTTPS succeeds
```

Certificate-time validation remained enabled. The milestone introduced no TLS
verification bypass and did not write or repair the RTC.

### Reproduction Procedure

Start after a normal reboot so the kernel has naturally loaded the stale RTC
into system time. Do not manually write the RTC.

In PowerShell window 1, from the repository root, select the PC LAN address
and start one local SNTP response:

```powershell
$PcIp = (Get-NetIPAddress -AddressFamily IPv4 |
  Where-Object { $_.IPAddress -notlike '127.*' -and $_.PrefixOrigin -ne 'WellKnown' } |
  Select-Object -First 1 -ExpandProperty IPAddress)
$PcIp
python .\testapp\sntp_mock_server.py --host 0.0.0.0 --port 19123 `
  --timestamp 1790000000 --count 1
```

Allow this Python process on the private LAN if Windows Firewall prompts.

In PowerShell window 2, start the existing TEST-ONLY HTTPS server:

```powershell
python .\testapp\https_mock_server.py --host 0.0.0.0 --port 18443 `
  --cert .\testapp\pki\server.crt --key .\testapp\pki\server.key `
  --sni-log "$env:TEMP\crossnook-time-sni.log"
```

In PowerShell window 3, use the same PC address and only `/tmp` on the Nook:

```powershell
$Adb = 'C:\platform-tools\adb.exe'
$PcIp = '192.168.1.100' # replace with the address printed in window 1

# Must initially show the natural stale year-2000 post-reboot wall clock.
& $Adb shell 'date -u; cat /proc/driver/rtc'

& $Adb push .\testapp\crossnook-time-test /tmp/crossnook-time-test
& $Adb push .\testapp\crossnook-net-test /tmp/crossnook-net-test
& $Adb push .\testapp\pki\testca.crt /tmp/crossnook-testca.crt
& $Adb shell 'chmod 755 /tmp/crossnook-time-test /tmp/crossnook-net-test'

# Explicitly mutates CLOCK_REALTIME only. It never writes /dev/rtc0.
& $Adb shell "/tmp/crossnook-time-test --sync 19123 3000 $PcIp"
& $Adb shell 'date -u'

# Existing verified TLS path, with certificate-time checks unchanged.
& $Adb shell "/tmp/crossnook-net-test --https-get $PcIp 18443 secure.test.local /tmp/crossnook-testca.crt /https/get"

& $Adb shell 'rm -f /tmp/crossnook-time-test /tmp/crossnook-net-test /tmp/crossnook-testca.crt'
```

The validated results are `TIMETEST SYNC OK`, a sane 2026 UTC `date`, and
`NETTEST HTTPS GET 200 ... OK`. No command writes `/data`, persistent Wi-Fi
configuration, calibration data, boot partitions, or the RTC.

## Deferred

- Authenticated time such as NTS.
- DNS and public NTP server selection.
- Multiple-server agreement.
- RTC correction or use as a trusted source.
- Persistent last-known-good time and rollback state.
- Clock slewing, drift control, and periodic/background synchronization.
- Reader/UI and automatic KOSync startup integration.
- Post-2038 platform support.

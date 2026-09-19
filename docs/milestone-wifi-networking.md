# Milestone: Wi-Fi Detach And Outbound Plain-HTTP

**Status: HOST VALIDATION PASS; HARDWARE VALIDATION PASS.** `crossnook-net-test`
is a static ARM binary, validated on the host toolchain, that performs a
bounded plain-HTTP/1.0 `GET`. This milestone also ships `diag/network-info.sh`,
a read-only probe for the Wi-Fi environment on the real device. The chain —
interface -> AP association -> IPv4 + default route -> outbound TCP -> plain
HTTP `GET` — was proven on the real Nook against a PC HTTP server
(`NESTEST GET 200 186 18 204 OK`, see Real-Device Results). No TLS, no KOSync,
no libcurl, no persistent config, no writes outside `/tmp`.

## Measured Environment (diagnostic ramdisk)

Measured from `work/crossnook-diag-nm-ramdisk.img` (original NookManager
diagnostic ramdisk; `URAMDISK` is an uImage-wrapped gzip cpio, kernel
2.6.29 `omap1`). On this image Wi-Fi **never comes up automatically** — the
stock `wpa_supplicant` service is started by its init only when the stock launcher
decides to enable Wi-Fi, which the diagnostic UI does not do. Associations must
therefore start the bits manually. Facts:

| Item | Measured value |
| --- | --- |
| Kernel module | `etc/wifi/tiwlan_drv.ko`, `vermagic=2.6.29-omap1 preempt mod_unload ARMv7` |
| Interface name | `tiwlan0` (`wifi.interface=tiwlan0` in `init.rc`) |
| Supplicant | `wpa_supplicant` (bionic-linked) banner: TI "Station Driver (1271)"; `-Dtiwlan0` supported |
| Driver loader | `tiwlan_loader` (static); usage `wlan_loader [driver] [-e eeprom] [-i ini] [-f firmware]` |
| Tool (MadWiFi-based) | `iwconfig` / `iwlist` / `iwpriv` (wireless-tools v29, glibc 2.11.1) |
| DHCP | BusyBox `udhcpc`; also static BSD `dhcpcd` |
| resolv.conf | `/etc/resolv.conf` is a symlink to `../tmp/resolv.conf` (writes are `/tmp`-only) |
| `wpa_cli` | not present (use a config file, not interactive commands) |
| Calibration | `/rom/devconf/WiFiBackupCalibration` (read-only) |

Stock boot flow (for reference, in `init.rc`): `wlan_loader` runs
`tiwlan_loader -f /etc/wifi/firmware.bin -i /etc/wifi/tiwlan.ini -e /rom/devconf/WiFiBackupCalibration`,
then `wpa_supplicant -Dtiwlan0 -itiwlan0 -c/data/misc/wifi/wpa_supplicant.conf`,
`ifconfig tiwlan0 up`, then `dhcpcd -ABKL -d tiwlan0`. The stock
`/etc/wifi/tiwlan.ini` exists. The supplicant config template used
`ctrl_interface=tiwlan0`.

## Real-Device Results (measured)

`diag/network-info.sh` collected the environment; the association + GET sequence
below was run manually on the physical Nook. The measured facts the next work
builds on:

1. `insmod /etc/wifi/tiwlan_drv.ko` succeeds **manually** on the diagnostic
   ramdisk.
2. `tiwlan_loader` successfully loads `/etc/wifi/firmware.bin`,
   `/etc/wifi/tiwlan.ini`, and `/rom/devconf/WiFiBackupCalibration`.
3. `tiwlan_loader` then sets `wlan.driver.status=ok`.
4. That property activates a stock `init.rc` trigger:

   ```text
   on property:wlan.driver.status=ok
       start wpa_supplicant
       start ifcfg_ti
       start dhcpcd
   ```

5. The diagnostic ramdisk is therefore **not fully detached** after a manual
   loader startup: the property trigger starts the stock networking services.
   Stock services must be stopped before running the `/tmp`-only manual
   CrossNook flow.
6. The stock `wpa_supplicant` service uses
   `/data/misc/wifi/wpa_supplicant.conf` — `/data` is the internal eMMC in this
   environment. CrossNook validation must stop the stock service and use only
   `/tmp/crossnook-wifi.conf` (nothing written to `/data`).
7. A manually launched
   `/sbin/wpa_supplicant -Dtiwlan0 -itiwlan0 -c/tmp/crossnook-wifi.conf -dd`
   associated successfully with the real AP.
8. The real hardware WPA result included association success, WPA key
   negotiation completion, CCMP PTK/GTK, and `CTRL-EVENT-CONNECTED`.
9. BusyBox `udhcpc` obtained/offered `192.168.0.104` during foreground testing
   but behaved awkwardly/intermittently. The stock init uses
   `/sbin/dhcpcd -ABKL -d tiwlan0`. This milestone reports both observations
   and does **not** claim `udhcpc` is the preferred production path — that
   decision needs further evidence.
10. Real Nook IPv4 observed: `192.168.0.104/24`.
11. Real default gateway observed: `192.168.0.1`.
12. `/tmp/resolv.conf` remained empty during this test. DNS was therefore
    **not** validated and is **not required** for this milestone.
13. Direct IPv4 HTTP was validated successfully.
14. The Windows PC required an inbound **Private-network** firewall rule for
    TCP/8000 before the Nook could reach `python -m http.server 8000`.
    ICMP is intentionally out of scope: the PC did not answer pings (only
    TCP/8000 was opened) and ping to the router `192.168.0.1` succeeded.
    No ICMP requirement is added to this milestone.
15. Final real-device result:

    ```text
    NESTEST GET 200 186 18 204 OK
    ```

## Manual Association Flow (measured)

The flow below is what the real-device pass exercised. The `insmod` step has a
side effect that must be handled first, so this is not the naive sequence:

1. Copy the supplicant config to `/tmp/crossnook-wifi.conf` (`/tmp` only; no
   `/data` writes). Push `crossnook-net-test` to `/tmp` and `chmod 755`.
2. `insmod /etc/wifi/tiwlan_drv.ko` — this also flips `wlan.driver.status=ok`
   via `tiwlan_loader` (init property trigger), which **starts the stock
   `wpa_supplicant`, `ifcfg_ti`, and `dhcpcd`**. Stop those stock services so
   the manual flow owns the interface (their config lives on `/data`).
3. `wpa_supplicant -Dtiwlan0 -itiwlan0 -c/tmp/crossnook-wifi.conf -dd`
   (the `-dd` variant is the measured one and printed
   `CTRL-EVENT-CONNECTED`).
4. verify with `iwconfig tiwlan0` / `/proc/net/wireless` (ESSID, rate).
5. obtain an IPv4 address + default route (`udhcpc -i tiwlan0` was tested and
   intermittent; the stock line `/sbin/dhcpcd -ABKL -d tiwlan0` is the
   reference). Measured outcome: `192.168.0.104/24`, gateway `192.168.0.1`.
6. `crossnook-net-test <pc-ip> 8000 <path>` against a PC `python -m
   http.server 8000` (with the PC inbound TCP/8000 rule open on Private
   networks). Recorded result:
   `NESTEST GET 200 186 18 204 OK`.

No `-Dwext` fallback is configured yet; `-Dtiwlan0` is the measured path.
DNS is optional and was not exercised (empty `/tmp/resolv.conf`); direct
IPv4 HTTP is the milestone proof.

## Tool Inventory Check

`diag/network-info.sh` prints a per-tool check. Expected healthy output:

```text
ifconfig: /sbin/ifconfig
route: /system/bin/toolbox/lib/...
ip: /system/bin/ip
wpa_supplicant: /system/bin/wpa_supplicant
tiwlan_loader: /system/bin/tiwlan_loader
iwconfig: /system/bin/iwconfig
iwlist: /system/bin/iwlist
iwpriv: /system/bin/iwpriv
udhcpc: /sbin/udhcpc
dhcpcd: /system/bin/dhcpcd
ping: /system/bin/ping
wget: /system/bin/wget
```

Running the probe changes nothing on the device; it reads `/proc`, `/sys`,
`/etc`, and runs read-only version banners.

## crossnook-net-test

`src/net/netsimple.{h,c}` is a bounded plain-HTTP/1.0 client used by
`src/app/nettest.c`. It supports a literal IPv4 (`inet_pton`-style parsing,
direct `connect`) **and** hostnames (`getaddrinfo`, `AI_NUMERICSERV`), one
`GET` request per run, HTTP/1.0 + `Connection: close`, reads until EOF or the
fixed 64 KiB buffer fills, and classifies failures into stable result names.
Everything is fixed-size; there are no unbounded allocations and no TLS.

Connect-failure classification is distinct per cause and never collapses
arbitrary `SO_ERROR` values into `connect-refused`:

| underlying cause | result name |
| --- | --- |
| `ECONNREFUSED` | `connect-refused` |
| `ENETUNREACH` | `network-unreachable` |
| `EHOSTUNREACH` | `host-unreachable` |
| `ETIMEDOUT` / `EINPROGRESS` | `connect-timeout` |
| any other errno / SO_ERROR | `connect-error` |

Real-hardware testing exposed that the nonblocking `connect()`/`poll()`/
`getsockopt(SO_ERROR)` path previously collapsed `ENETUNREACH` (route gone,
`Network is unreachable`) into `connect-refused`; the mapper is now a public
`cn_netsimple_connect_error(errno_value)` pinned by deterministic host tests.

```c
cn_netsimple_get(host, port, path, buffer, buffer_cap,
                 connect_ms, recv_ms, &response)
```

CLI surface (`crossnook-net-test`):

| Mode | Purpose | Exit (success) |
| --- | --- | --- |
| `<host> <port> <path>` | live GET; prints `NESTEST GET <code> <hdr> <body> <total> <reason>` | 0 |
| `--api-smoke` | parser/validation checks | 0 |
| `--self-refused` | connect to a closed loopback port must fail cleanly | 0 |
| `--self-timeout` | 600 ms connect deadline against `192.0.2.1` must fail in under 6 s | 0 |
| `--self-test` | fork + loopback canned HTTP/1.1 200 server; must GET it | 0 |
| `--help` / no args | usage | 0 / 2 |

Hostname output: DNS resolution and outbound `TCP:80` are validated on the
device only once a resolv.conf gives the resolver something to use; in the
measured pass `/tmp/resolv.conf` stayed empty, so DNS was not exercised and
is not part of this milestone.

## Host Validation

Run:

```sh
bash testapp/build-nettest.sh
```

The suite verifies:

- static ARM EABI5 soft-float, non-PIE, no INTERP/DYNAMIC (fully static);
- structural isolation: `src/net` and `src/app/nettest.c` reference no
  `libcurl`, `ssl`, `openssl`, `wpa`, or `koreader` symbols;
- IPv4 literal, port, host/path validation, request builder, and status parser
  behavior;
- canned 200/404 parses; garbage response rejection; stable result names;
- deterministic connect-error mapping pinned by synthetic errno tests:
  `ECONNREFUSED` -> `connect-refused`, `ENETUNREACH` -> `network-unreachable`,
  `EHOSTUNREACH` -> `host-unreachable`, `ETIMEDOUT`/`EINPROGRESS` ->
  `connect-timeout`, and every other value (including `EACCES`,
  `EADDRNOTAVAIL`, `EPERM`, `ENOBUFS`) -> `connect-error` (never
  `connect-refused`);
- all three self-tests run under `qemu-arm` against the container's real
  kernel (loopback server, refused connect, real connect deadline);
- usage exits non-zero.

Observed final result:

```text
NETTEST HOST VALIDATION OK
```

## Focused Real-Nook Validation (passed)

Run with a PC on the same LAN:

```powershell
bash testapp/build-nettest.sh
python -m http.server 8000
```

Start the HTTP server **in a directory you are willing to share**, note the
PC's LAN IP (`192.168.0.107` in the test), and open the PC firewall for
inbound TCP/8000 on **Private** networks only:

```powershell
New-NetFirewallRule -DisplayName crossnook-test-tcp8000 -Direction Inbound `
  -Protocol TCP -LocalPort 8000 -Action Allow -Profile Private
```

Create the supplicant config on the PC **outside the repository** (no
credentials may be committed anywhere), push it plus the binary to `/tmp`
only, and delete the PC-side config copy immediately:

```powershell
C:\platform-tools\adb.exe push C:\temp\crossnook-wifi.conf /tmp/crossnook-wifi.conf
Remove-Item C:\temp\crossnook-wifi.conf
C:\platform-tools\adb.exe push testapp\crossnook-net-test /tmp/crossnook-net-test
C:\platform-tools\adb.exe shell "chmod 755 /tmp/crossnook-net-test"
```

Then run the Manual Association Flow (measured) and finish with:

```powershell
C:\platform-tools\adb.exe shell "/tmp/crossnook-net-test 192.168.0.107 8000 /hello.txt"
```

The on-device result that recorded the pass was:

```text
NESTEST GET 200 186 18 204 OK
```

Hardware observations recorded: `insmod` succeeded manually; `tiwlan_loader`
loaded firmware/calibration and set `wlan.driver.status=ok`, which started the
stock networking services via `init.rc` (they were stopped before the manual
flow); manual `/sbin/wpa_supplicant -Dtiwlan0 -itiwlan0 -c/tmp/crossnook-wifi.conf -dd`
printed `CTRL-EVENT-CONNECTED` (CCMP PTK/GTK); IPv4 `192.168.0.104/24`,
gateway `192.168.0.1`; the Windows firewall rule was required before TCP/8000
reached the PC; ICMP to the PC stayed blocked (not required), ping to the
router succeeded.

Password hygiene: SSID and password are written only to `/tmp` on the device;
the PC-side copy of the config is deleted immediately after use; passwords
never appear in the repo, this doc, or diag scripts; never echo a password on
the device console.

## Focused Real-Nook Revalidation

Small, targeted check after any `netsimple` change (this is what was done after
the error-classification fix). It exercises only the networking path, not
unrelated suites.

A. With valid networking (associated, IPv4 + default route present):

```powershell
C:\platform-tools\adb.exe push testapp\crossnook-net-test /tmp/crossnook-net-test
C:\platform-tools\adb.exe shell "chmod 755 /tmp/crossnook-net-test"
C:\platform-tools\adb.exe shell "/tmp/crossnook-net-test 192.168.0.107 8000 /hello.txt"
```

Must still return:

```text
NESTEST GET 200 <hdr> <body> <total> OK
```

B. Optional, in a controlled no-route state (tiwlan0 up but no IPv4 address
and empty `/proc/net/route`): the same command must now report
`network-unreachable`, not `connect-refused`:

```powershell
C:\platform-tools\adb.exe shell "/tmp/crossnook-net-test 192.168.0.107 8000 /hello.txt"
```

Expected:

```text
NESTEST GET FAIL 192.168.0.107 8000 /hello.txt network-unreachable
```

Only run B if it does not destabilize the device; the deterministic host test
of the errno mapping (`--api-smoke`) already pins `ENETUNREACH ->
network-unreachable`, so B is corroboration, not the proof.

## Limitations And Non-Goals

No TLS (HTTP only), no KOSync client, no automatic sync, no credentials kept
anywhere on the device, no settings UI, no persistent config, no writes
outside `/tmp`, no `-Dwext` fallback yet, no IPv6, and no retries. The stock
calibration file `/rom/devconf/WiFiBackupCalibration` is read-only and is
only *passed through* if it exists.
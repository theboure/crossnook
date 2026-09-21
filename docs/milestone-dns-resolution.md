# DNS Resolution Foundation

**Status: HOST VALIDATION PASS; HARDWARE REVALIDATION PASS.**

This milestone adds a small bounded DNS A-record resolver for the numeric
routing address used by CrossNook networking. It does not change HTTP identity,
TLS SNI, CA trust, DNS hostname verification, certificate-time verification, or
the trusted-time prerequisite.

## Hardware Basis

Read-only reconnaissance on the physical Nook Simple Touch established:

- `/etc/resolv.conf` is a symlink to `../tmp/resolv.conf`;
- `/tmp/resolv.conf` was empty;
- Android properties exposed `dhcp.tiwlan0.dns1=192.168.0.1` and
  `dhcp.tiwlan0.dns2=8.8.8.8`;
- the stock `nslookup` failed against both addresses on the observed network;
  and
- UDP/IPv4 itself works, as proven by the trusted-time milestone's controlled
  SNTP exchange on port 19123.

The libc resolver is therefore not the runtime abstraction for this milestone.
It is synchronous, obtains policy from global resolver state such as
`/etc/resolv.conf`, normally fixes DNS to port 53, and does not provide the
caller-controlled monotonic deadline required here. CrossNook instead receives
explicit numeric DNS server addresses from its caller. Discovering those
addresses from DHCP or Android properties is deferred.

No public DNS probing or router reconfiguration is part of this milestone.
Physical validation uses a controlled Windows-PC UDP server on nonprivileged
port 19153.

## Architecture

`src/net/dnssimple.[ch]` is a standalone UDP/IPv4 resolver. It does not alter
the existing `netsimple` or `tlssimple` interfaces.

```text
explicit numeric DNS server
    -> cn_dnssimple_resolve_a(original hostname)
    -> bounded numeric IPv4 answer
    -> request.connect_host (routing only)

original hostname
    -> request.host
    -> HTTP Host + TLS SNI + X.509 DNS identity
```

Configuration contains at most two numeric IPv4 servers, one UDP port, and a
per-server timeout. Port zero selects 53 and timeout zero selects 2000 ms.
Servers are attempted sequentially for timeout, transport, malformed-response,
truncation, packet-size, and server-failure results. Semantic answers such as
NXDOMAIN, no direct A address, or unsupported CNAME are terminal.

The resolver returns at most four unique IPv4 addresses in DNS answer order.
There is no allocation, global resolver state, background work, cache, or libc
resolver fallback.

## Protocol Bounds

Each server attempt builds one standard DNS query with:

- an unpredictable 16-bit transaction ID from `/dev/urandom`;
- exactly one validated hostname question;
- type A, class IN; and
- recursion desired, without EDNS.

The exact query datagram is sent initially and, if no accepted response has
arrived, retransmitted at approximately one quarter and one half of the
configured timeout. All three transmissions use the same socket, server,
transaction ID, and query bytes. They share the original absolute monotonic
deadline; the timeout is never reset or extended. A delayed valid response to
any transmission remains acceptable.

The hostname is at most 253 bytes. Labels are at most 63 bytes and contain only
ASCII letters, digits, and interior hyphens. Empty labels, leading or trailing
hyphens, controls, non-ASCII data, and malformed dotted forms are rejected
before network access.

The socket is nonblocking and each attempt uses `poll()` against an absolute
`CLOCK_MONOTONIC` deadline. A response is considered only when its IPv4 source
address and UDP source port match the configured server and its transaction ID
matches the query. Unrelated datagrams do not extend the deadline.

Packets are bounded to the classic 512-byte DNS UDP limit. A 513-byte receive
buffer detects oversized datagrams. The parser validates the header, echoed
question, type/class, RCODE, record bounds, and compressed names. Compression
pointers are checked for packet bounds, loops, and a finite hop limit. TC is
reported as `truncated-response`; TCP fallback is not attempted.

Only A records whose owner exactly matches the original question are accepted.
Duplicate addresses are removed. A CNAME-only result is reported as
`unsupported-cname`; aliases are not followed. Unrelated owners, zero direct A
records, AAAA, and other record types do not become routing addresses.

## Trust Model

DNS output is unauthenticated routing information. It is never an authenticated
service identity.

An active network attacker or compromised DNS server can choose the numeric
address to which CrossNook connects, suppress replies, or cause resolution to
fail. The resolver's source, transaction, question, and parser checks reduce
accidental acceptance and common off-path spoofing opportunities, but they do
not provide cryptographic DNS authentication.

Verified HTTPS remains the authentication boundary. The original hostname is
preserved for HTTP `Host`, TLS SNI, and BearSSL certificate hostname
verification. The resolved address is used only as `connect_host`. Redirecting
DNS to an attacker therefore still requires a certificate chaining to the
explicit trust anchor and valid for the original hostname. Certificate-time
validation also remains enabled and requires trusted wall-clock time.

## Diagnostic Modes

`crossnook-net-test` adds:

```text
--dns-query <server> <port> <timeout-ms> <hostname> [server-2]
--https-get-dns <server> <port> <dns-timeout-ms> <https-port> \
  <hostname> <ca-file> <path> [epoch] [https-timeout-ms]
```

The first mode resolves and prints bounded textual IPv4 answers. The second
resolves the hostname, routes HTTPS to the first address, and retains the
hostname as the HTTP/TLS identity.

## Host Gate

Run:

```bash
bash testapp/build-dns.sh
```

The focused gate builds `testapp/crossnook-net-test` as static ARM EABI5
soft-float non-PIE and runs it under `qemu-arm` against
`testapp/dns_mock_server.py`. It does not rebuild BearSSL or run the full
historical suite.

The deterministic cases cover:

```text
single A                       -> one address
multiple A                     -> answer order preserved
duplicate A                    -> deduplicated
more than four A               -> bounded to four
NXDOMAIN                       -> nxdomain
SERVFAIL then second server    -> success, server index 1
silence                        -> timeout
wrong transaction ID          -> ignored, then timeout
wrong UDP source port          -> ignored, then timeout
truncated packet               -> malformed-response
packet larger than 512 bytes   -> packet-size
TC response                    -> truncated-response
mismatched question            -> malformed-response
bad compression pointer        -> malformed-response
compression loop               -> malformed-response
zero A records                 -> no-address
unrelated A owner              -> no-address
CNAME-only                     -> unsupported-cname
unusual unrelated owner label  -> ignored, direct A accepted
reserved header Z bit          -> malformed-response
first two queries dropped      -> identical third transmission succeeds
first three queries dropped    -> timeout; no fourth transmission
delayed initial response       -> accepted after both retransmissions
delayed retransmit-1 response  -> accepted after retransmit 2
invalid hostname               -> invalid-hostname
```

The retry cases assert exactly three received datagrams. Their monotonic receive
offsets are checked around the configured quarter- and half-time points,
byte-for-byte query identity is required, and the all-drop case returns near
the original deadline rather than a reset timeout period.

The same gate resolves `secure.test.local` to the controlled HTTPS server and
gets `NETTEST HTTPS DNS GET 200 23 184 OK`. It then resolves `wrong.test` to
the same address and requires `tls-hostname-mismatch`. The server-side SNI log
contains both original names, and its HTTP log contains
`Host: secure.test.local:19443`, proving that the DNS address replaced neither TLS
nor HTTP identity. Focused `--api-smoke` and `--tls-time` regressions also pass.

## Physical Nook Validation

The functional and security path passed on the physical Nook at
`192.168.0.104`, using the controlled Windows PC at `192.168.0.107`:

```text
NETTEST DNS OK host=secure.test.local count=1 server=0 addr0=192.168.0.107
NETTEST DNS ROUTE host=secure.test.local address=192.168.0.107 server=0
NETTEST HTTPS DNS GET 200 23 184 OK
NETTEST DNS ROUTE host=wrong.test address=192.168.0.107 server=0
NETTEST HTTPS DNS GET FAIL tls-hostname-mismatch
```

The HTTPS mock recorded SNI for both `secure.test.local` and `wrong.test`, and
recorded HTTP Host `secure.test.local:18443`. This proves the DNS answer remains
routing-only while the original hostname remains the HTTP and TLS identity.

A prior `tls-recv-timeout` was caused by two HTTPS mock processes accidentally
being left active in the Windows test environment. With exactly one HTTPS
listener, direct and DNS-routed HTTPS repeatedly passed. There is no current
evidence of a TLS defect; the reliability evidence below is DNS/UDP-only.

Final hardware revalidation after adding two bounded retransmissions passed.
All commands used only `/tmp` on the Nook and controlled services on the
Windows PC. No command modified resolver configuration, Android properties,
Wi-Fi configuration, the RTC, `/data`, `/system`, `/rom`, or boot partitions.

In PowerShell window 1, from the repository root, start the DNS mock. The
answer must be the PC's LAN address reachable from the Nook:

```powershell
$PcIp = '192.168.0.107' # replace only if the PC LAN address changed
python .\testapp\dns_mock_server.py --host 0.0.0.0 --port 19153 `
  --answer $PcIp --verbose
```

Allow UDP/19153 on the Private network if Windows Firewall prompts.

In PowerShell window 2, start the TEST-ONLY HTTPS server:

```powershell
python .\testapp\https_mock_server.py --host 0.0.0.0 --port 18443 `
  --cert .\testapp\pki\server.crt --key .\testapp\pki\server.key `
  --sni-log "$env:TEMP\crossnook-dns-sni.log" `
  --host-log "$env:TEMP\crossnook-dns-host.log"
```

Allow TCP/18443 on the Private network if Windows Firewall prompts.

In PowerShell window 3, push only temporary test artifacts and run the resolver
and integrated verified-HTTPS checks:

```powershell
$Adb = 'C:\platform-tools\adb.exe'
$PcIp = '192.168.0.107' # same address used by the DNS mock

& $Adb shell 'date -u'
# STOP unless trusted UTC is >= 2025-01-01T00:00:00Z and
# strictly < 2030-12-31T00:00:00Z.

& $Adb push .\testapp\crossnook-net-test /tmp/crossnook-net-test
& $Adb push .\testapp\pki\testca.crt /tmp/crossnook-testca.crt
& $Adb shell 'chmod 755 /tmp/crossnook-net-test'

# Resolver-only proof: must report count=1, server=0, and the PC LAN address.
& $Adb shell "/tmp/crossnook-net-test --dns-query $PcIp 19153 2000 secure.test.local"

# Resolved numeric route plus unchanged hostname identity: must return 200 OK.
& $Adb shell "/tmp/crossnook-net-test --https-get-dns $PcIp 19153 2000 18443 secure.test.local /tmp/crossnook-testca.crt /https/get"

# Same resolved route with the wrong original identity: must fail hostname check.
& $Adb shell "/tmp/crossnook-net-test --https-get-dns $PcIp 19153 2000 18443 wrong.test /tmp/crossnook-testca.crt /https/get"

& $Adb shell 'rm -f /tmp/crossnook-net-test /tmp/crossnook-testca.crt'
```

The required outputs are `NETTEST DNS OK`,
`NETTEST HTTPS DNS GET 200 ... OK`, and
`NETTEST HTTPS DNS GET FAIL tls-hostname-mismatch`. The SNI log must contain
both `secure.test.local` and `wrong.test`; the Host log must contain
`secure.test.local:18443`.

### Physical Reliability Evidence

Device-local syscall instrumentation proved that normal and trace-enabled ARM
builds both scheduled the existing retransmission correctly at approximately
1000 ms for a 2000 ms timeout. In one normal run, local `sendto()` calls were
approximately 1001.8 ms apart while Windows received the corresponding packets
only approximately 1.57 ms apart. The prior near-simultaneous Windows receive
timestamps were therefore network delivery delay/batching after Nook kernel
acceptance, not an immediate client retransmission or a trace-dependent timer
bug.

Five subsequent normal-build runs against the controlled drop-first mock showed:

```text
1. PASS: two Windows receives approximately 0.68 ms apart
2. FAIL: two receives approximately 1006 ms apart; response sent, Nook timed out
3. PASS: two receives approximately 1001 ms apart
4. FAIL: only the intentionally dropped initial query reached Windows
5. PASS: two receives approximately 534 ms apart
```

These runs demonstrate that the path can independently lose or delay an
outbound retransmission or the inbound response to it. Two total transmissions
were therefore insufficiently robust on this device.

A full-length UDP `sendto()` proves complete datagram acceptance by the local
kernel, not delivery over the Wi-Fi path. The evidence therefore demonstrates
that an individual query can be delayed or lost between Nook kernel acceptance
and Windows mock delivery. It does not identify or claim an exact lower-layer
cause such as neighbor discovery, WiLink power management, driver/firmware
behavior, or another network component. Three sends improve tolerance but do
not guarantee UDP delivery.

This evidence justifies two bounded same-server retransmissions at one quarter
and one half of the original timeout. Trace-enabled builds distinguish
`send initial`, `send retransmit 1`, `send retransmit 2`, `recv`, and `timeout`.
Normal builds compile the trace out.

The final bounded policy is:

```text
initial send      start
retransmit #1     start + T/4
retransmit #2     start + T/2
hard deadline     start + T
```

Each server attempt accepts at most three sends. All sends preserve the same
query bytes, transaction ID, socket, server, and port. The deadline is never
reset or extended.

Controlled-loss hardware validation then passed:

```text
first two queries deliberately dropped, third answered -> resolver recovered
all three queries deliberately dropped                 -> NETTEST DNS FAIL timeout
all-drop measured wall duration                        -> 2029 ms
all-drop mock receives                                 -> exactly 3, same ID
all-drop fourth query                                  -> not sent
```

Production-representative router DNS validation also passed against
`192.168.0.1:53` for `example.com`. After one initial 60-second idle period and
five more lookups each preceded by 60 seconds idle, results were:

```text
34 ms  -> NETTEST DNS OK
27 ms  -> NETTEST DNS OK
27 ms  -> NETTEST DNS OK
28 ms  -> NETTEST DNS OK
535 ms -> NETTEST DNS OK
24 ms  -> NETTEST DNS OK
```

Router DNS idle validation was therefore 6/6 PASS. The approximately 535 ms
success is consistent with recovery near the first `T/4` retransmission point
for a 2000 ms timeout.

Controlled peer-to-peer Nook-to-Windows testing continued to show intermittent
UDP delay/loss after idle. Windows `pktmon` positive control proved the capture
path sees both query and response during successful operation. During a clean
idle failure, the Nook resolver sent all three queries, Windows Wi-Fi capture
observed all three incoming DNS queries, Windows sent all three DNS responses
toward the Wi-Fi/AP path, and the Nook still reached its bounded deadline. This
anomaly is below the DNS resolver and was specific to the controlled peer
topology in testing. It was not reproduced against the
production-representative router DNS path. No exact WiLink, driver, firmware,
AP, or 802.11 root cause is claimed.

### Hardware Reproduction Procedure

Use the normal, trace-disabled `testapp/crossnook-net-test`. Ensure only one DNS
mock owns UDP/19153.

In PowerShell window 1, prove recovery when the first two datagrams are lost.
The mock exits after receiving three expected identical datagrams and answers
only the third:

```powershell
$PcIp = '192.168.0.107'
python .\testapp\dns_mock_server.py --host 0.0.0.0 --port 19153 `
  --answer $PcIp --mode drop-first-two-then-valid --count 3 --verbose
```

In PowerShell window 2:

```powershell
$Adb = 'C:\platform-tools\adb.exe'
$PcIp = '192.168.0.107'
& $Adb push .\testapp\crossnook-net-test /tmp/crossnook-net-test
& $Adb push .\testapp\pki\testca.crt /tmp/crossnook-testca.crt
& $Adb shell 'chmod 755 /tmp/crossnook-net-test'
& $Adb shell "/tmp/crossnook-net-test --dns-query $PcIp 19153 2000 secure.test.local"
```

The resolver must succeed, and the mock must show three receives with the same
transaction ID. Its identity check withholds the response if any query byte
changed.

Next restart window 1 in all-drop mode:

```powershell
python .\testapp\dns_mock_server.py --host 0.0.0.0 --port 19153 `
  --answer $PcIp --mode drop-all-fourth-valid --verbose
```

Repeat the resolver command. It must return `NETTEST DNS FAIL timeout` after
the original approximately 2000 ms budget, and the mock must show exactly three
receives with the same transaction ID. A fourth query would receive a valid
answer and incorrectly turn this test into success.

Finally stop that mock, start normal `valid` mode, and repeat the resolver-only,
DNS-routed HTTPS, wrong-host, SNI, and HTTP Host checks in the procedure above.
Remove only the temporary artifacts afterward:

```powershell
& $Adb shell 'rm -f /tmp/crossnook-net-test /tmp/crossnook-testca.crt'
```

## Deferred

- DHCP/Android-property DNS server discovery and production server policy.
- CNAME following and canonical-name policy.
- AAAA and IPv6.
- TCP fallback for truncated UDP responses.
- EDNS, DNSSEC, DoH, and DoT.
- Caching, TTL handling, search domains, and background retries.
- Production CA bundle policy.
- Reader/UI and automatic KOSync integration.

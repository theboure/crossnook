# TLS / HTTPS Transport Foundation

**Status: HOST VALIDATION PASS; HARDWARE VALIDATION PASS.**

The published `milestone/kosync-protocol` tag remains unchanged at
`5e1051b025ff49cceab7dd314ff8e209b8fb81a0`.

## Implementation

CrossNook uses BearSSL 0.6 (`0.6+dfsg.1`) under the MIT license. The pinned
source artifact is:

```
bearssl_0.6+dfsg.1.orig.tar.xz
sha256 102a625bd8f6155260867a50c508df8d8c3f796985dd744df1a68e5cd1a79004
```

`toolchain/bearssl/fetch.sh` downloads and verifies the artifact into the
ignored `work/bearssl/` build-input area. `toolchain/Dockerfile` verifies the
digest again and builds a static ARM library; the HTTPS host gate also performs
a clean focused ARM rebuild of every BearSSL source file. The license notice is retained at
`third_party/bearssl-LICENSE.txt`.

BearSSL implements TLS 1.0 through 1.2. CrossNook restricts the client to TLS
1.2, disables renegotiation, requires RSA certificate keys of at least 2048
bits, and advertises only ECDHE with AES-GCM or ChaCha20-Poly1305. BearSSL 0.6
does not implement TLS 1.3 and upstream describes the release as beta-quality.

The measured static ARM sizes from the host gate are:

| Artifact | text+data+bss |
| --- | ---: |
| Published plain net diagnostic rebuilt from `5e1051b` | 70,712 bytes |
| TLS net diagnostic, including its BearSSL test server | 207,688 bytes |
| KOSync diagnostic with HTTPS client support | 196,856 bytes |

The net diagnostic delta is 136,976 bytes and is a conservative test-binary
impact because it includes server-side BearSSL and PKI fixture decoding that
production client code does not use. The unstripped static files produced by
the gate are 263,740 bytes and 252,680 bytes respectively.

## Layering

```
KOSync protocol
      |
bounded HTTP request/response (netsimple)
      |
plain TCP or verified TLS 1.2/TCP (tlssimple + BearSSL)
```

`cn_netsimple_request.host` is the HTTP `Host`, SNI name, and certificate DNS
identity. `connect_host` is an optional routing override. This permits a Nook
to connect directly to a PC LAN address without DNS while BearSSL still
verifies `secure.test.local` against the certificate SAN. A non-empty DNS
identity is mandatory for TLS; there is no verify-off or name-check bypass.
IP-SAN-only identities are not supported because BearSSL's minimal verifier
does not implement them, and CrossNook does not carry a custom X.509 parser.

BearSSL validates the supplied chain to an explicit trust anchor, certificate
validity, key use, signatures, and DNS SAN (with CN fallback only when SAN is
absent). It does not fetch missing intermediates, perform AIA or revocation
checks, or enforce Name Constraints.

HTTP requests and responses remain bounded. TLS responses with
`Content-Length` complete only after the authenticated body is received; raw
TCP EOF before complete framing is a TLS protocol failure. The client sends a
best-effort `close_notify` before closing its socket.

## Entropy And Time

On the old kernel path, the client first waits with the TLS deadline for
`/dev/random` to report a seeded pool, then reads 32 bytes from
`/dev/urandom`. It fails with `tls-entropy-failed` on readiness, open, read,
timeout, or short-read failure. It does not use
`getrandom()`, time, PID, MAC address, or a fixed seed. This is compatible with
the Nook's Linux 2.6.29 kernel, provided its entropy pool has been seeded.

Certificate time validation is always enabled. Production calls use `time()`;
host expiry tests inject a fixed test time. A wrong device clock must be fixed
or reported before hardware TLS testing. Time validation must not be disabled.

The milestone has no production trust-store default. Tests pass an explicit
committed TEST-ONLY CA. A future milestone must choose and maintain a modern
production CA bundle without relying on the diagnostic ramdisk.

## Test PKI

`testapp/pki/` contains public, deterministic TEST-ONLY fixtures and private
keys. They are never production defaults. The valid leaf has DNS SANs for
`secure.test.local` and `localhost`; separate fixtures cover another CA,
expired and not-yet-valid leaves, and a deliberately omitted intermediate.
`testapp/gen-pki.sh` reissues the leaves from the committed keys with fixed
validity windows. Host TLS and KOSync tests inject a fixed validation epoch.
OpenSSL is used only to create and inspect test fixtures;
it is not a CrossNook runtime dependency.

## Host Gate

Run:

```bash
bash testapp/build-https.sh
```

The gate rebuilds static EABI5 soft-float non-PIE ARM diagnostics, runs them
under `qemu-arm`, and proves:

- verified TLS 1.2 handshake, DNS SAN hostname validation, and observed SNI;
- HTTPS GET and PUT;
- untrusted CA, wrong hostname, expired leaf, not-yet-valid leaf, omitted
  intermediate, and non-TLS peer rejection;
- handshake, read, and write timeout mapping;
- bounded/truncated HTTPS response handling, including short declared bodies,
  incomplete headers, raw EOF, and entropy failure;
- clean plain-HTTP regressions through `build-nettest.sh` and
  `build-kosync.sh`; and
- the exact KOSync HTTPS round trip:

```
KOSYNC ROUNDTRIP failures=0 first=1700000000 update=1700000001 other=1700000002 -> OK
```

## Real Nook Validation

The complete secure-transport matrix passed on a physical Barnes & Noble Nook
Simple Touch running the known-working diagnostic ramdisk and Linux 2.6.29.
The exact results were:

```text
NETTEST HTTPS GET 200 23 184 OK
NETTEST HTTPS GET FAIL tls-hostname-mismatch
NETTEST HTTPS GET FAIL tls-trust-failed
NETTEST HTTPS PUT 200 14 175 OK
[OK] first progress upload
[OK] first progress retrieval
[OK] same-document update
[OK] updated progress retrieval
[OK] distinct-document upload
[OK] distinct documents remain independent
KOSYNC ROUNDTRIP failures=0 first=1700000000 update=1700000001 other=1700000002 -> OK
```

The server-side SNI log contained `secure.test.local`, `wrong.test`, and later
`secure.test.local` entries. This proves that SNI is sent for both the accepted
and rejected identity cases. Numeric routing through `connect_host` worked
without working DNS, while BearSSL still verified the DNS SAN identity. The
TEST-ONLY CA was loaded only from `/tmp` on the Nook.

### Clock Finding

Immediately after reboot, the Nook runtime clock reported approximately:

```text
Mon Jan 3 ... UTC 2000
```

That time is outside the controlled certificate validity window of 2025-01-01
through 2030-12-31 UTC. For this controlled hardware validation, the device
clock was manually set from the trusted host PC before TLS was tested.
Certificate validity checking remained enabled throughout the test.

Production HTTPS has a strict prerequisite: **CrossNook must obtain trustworthy
wall-clock time before starting TLS.** Automatic trustworthy time acquisition
is unresolved and belongs to a future milestone. It must not be replaced by
disabling certificate-time validation, ignoring `NotBefore`/`NotAfter`, adding
an insecure TLS mode, or accepting certificates against a known-invalid clock.
NTP is intentionally not implemented by this milestone.

### Reproduction Procedure

The server certificate is valid from 2025-01-01 through 2030-12-31 UTC. Begin
every reproduction with `adb shell date`. Stop if the time is outside this
window; do not bypass time validation.

In PowerShell window 1, from the repository root, select the PC's LAN IPv4 and
start the TEST-ONLY HTTPS/KOSync mock. Replace the sample address selection if
the machine has multiple active adapters.

```powershell
$Repo = (Get-Location).Path
$PcIp = (Get-NetIPAddress -AddressFamily IPv4 |
  Where-Object { $_.IPAddress -notlike '127.*' -and $_.PrefixOrigin -ne 'WellKnown' } |
  Select-Object -First 1 -ExpandProperty IPAddress)
$PcIp
python .\testapp\https_mock_server.py --host 0.0.0.0 --port 18443 `
  --cert .\testapp\pki\server.crt --key .\testapp\pki\server.key `
  --sni-log "$env:TEMP\crossnook-nook-sni.log"
```

Allow that Python process on the private LAN if Windows Firewall prompts.
Keep it running. In PowerShell window 2, set the same PC address and use only
`/tmp` on the Nook:

```powershell
$Adb = 'C:\platform-tools\adb.exe'
$PcIp = '192.168.1.100'  # replace with the value printed in window 1

& $Adb shell date
# STOP unless the device UTC date is within 2025-01-01..2030-12-31.

& $Adb push .\testapp\crossnook-net-test /tmp/crossnook-net-test
& $Adb push .\testapp\crossnook-kosync-test /tmp/crossnook-kosync-test
& $Adb push .\testapp\pki\testca.crt /tmp/crossnook-testca.crt
& $Adb push .\testapp\pki\otherca.crt /tmp/crossnook-otherca.crt
& $Adb shell 'chmod 755 /tmp/crossnook-net-test /tmp/crossnook-kosync-test'

# Trusted CA, correct DNS identity, numeric route: must print HTTPS GET 200 ... OK.
& $Adb shell "/tmp/crossnook-net-test --https-get $PcIp 18443 secure.test.local /tmp/crossnook-testca.crt /https/get"

# Wrong identity: must fail and print tls-hostname-mismatch.
& $Adb shell "/tmp/crossnook-net-test --https-get $PcIp 18443 wrong.test /tmp/crossnook-testca.crt /https/get"

# Wrong CA: must fail and print tls-trust-failed.
& $Adb shell "/tmp/crossnook-net-test --https-get $PcIp 18443 secure.test.local /tmp/crossnook-otherca.crt /https/get"

# Verified HTTPS PUT: must print HTTPS PUT 200 ... OK.
& $Adb shell "/tmp/crossnook-net-test --https-put $PcIp 18443 secure.test.local /tmp/crossnook-testca.crt /https/put nook-test-body"

# Deterministic dummy credentials only; must reproduce 1700000000/1/2.
& $Adb shell "/tmp/crossnook-kosync-test --roundtrip-https https://secure.test.local:18443 $PcIp /tmp/crossnook-testca.crt test-user dfb450efddbb5387197c84460623675b e1a1e9016cfc9bca8c694187943e9c4f 519220cea448409961e6b3081a36eca3"

& $Adb shell 'rm -f /tmp/crossnook-net-test /tmp/crossnook-kosync-test /tmp/crossnook-testca.crt /tmp/crossnook-otherca.crt'
```

Afterward, window 1's SNI log must contain `secure.test.local`. No command
writes to `/data`, `/system`, or `/rom`.

## Known Limitations

- BearSSL 0.6 is upstream beta-quality. CrossNook enables TLS 1.2 only and has
  no TLS 1.3 support.
- The client is IPv4-only. DNS resolution itself has no portable monotonic
  timeout on this libc; the primary hardware procedure uses a numeric
  `connect_host`, so DNS is not in that path.
- IP-address SAN verification is intentionally unsupported. A separately
  routed, verified DNS identity is required.
- CA input is bounded to 64 KiB and eight anchors. There is no production CA
  bundle decision yet.
- BearSSL performs no revocation, AIA fetching, or Name Constraints checking.
- Reliable automatic production time acquisition is unresolved. The controlled
  hardware test used a clock set manually from the trusted host.
- No real KOSync account, production credentials, or production KOSync server
  were used.
- Reader/UI sync integration is not implemented.
- Background sync, automatic sync, retry queues, and persistent credential
  storage are not implemented.

# KOSync End-to-End Sync Integration

**Status: HOST VALIDATION PASS; HARDWARE REVALIDATION PASS.**

This milestone composes the published document identity, local progress,
trusted-time, DNS, verified HTTPS, and KOSync protocol components into one
synchronous foreground operation. It adds no scheduler, credential store,
settings UI, reader conflict UI, or background work.

## Integration API

`src/sync/kosync_sync.[ch]` exports:

```c
void cn_kosync_sync_result_init(cn_kosync_sync_result *result);
void cn_kosync_sync_result_clear(cn_kosync_sync_result *result);

cn_kosync_sync_status cn_kosync_sync_once(
    const cn_kosync_sync_config *config,
    cn_kosync_sync_result *result);
```

The caller supplies an EPUB path, an already-open progress store, an initialized
HTTPS KOSync client, DNS and TLS configuration, time policy, and device fields.
The call:

1. computes the path-keyed local identity;
2. computes the KOReader Binary document identity;
3. loads local progress;
4. establishes trusted time when configured to do so;
5. resolves the original service hostname with `dnssimple`;
6. routes verified HTTPS through the first numeric DNS answer while preserving
   the original hostname for HTTP Host, TLS SNI, and X.509 verification;
7. performs KOSync GET; and
8. applies the conservative decision below.

Plain HTTP clients are rejected before synchronization.

`cn_kosync_sync_result` owns `remote_progress.logical_position`. The result must
be initialized before its first call and cleared when finished. It reports the
integration status and decision, local/remote presence and mutation flags,
KOReader document ID, remote progress, component results, transport outcome,
and accepted PUT timestamp.

## Time Policy

`CN_KOSYNC_SYNC_TIME_ESTABLISH` requires a `cn_time_config` and calls
`cn_timesimple_sync()` before DNS or HTTPS. Failure returns
`CN_KOSYNC_SYNC_STATUS_TRUSTED_TIME_FAILED`; DNS and KOSync are not attempted.
This policy rejects a TLS `get_time` callback so certificate validation must use
the newly synchronized `CLOCK_REALTIME`.

`CN_KOSYNC_SYNC_TIME_CALLER_ESTABLISHED` is explicit. It does not set the clock;
the caller guarantees that the TLS time source is already valid. The host gate
uses this mode with either the current valid wall clock (`-`) or a fixed numeric
TLS test epoch. Neither form calls `cn_timesimple_sync()` or changes
`CLOCK_REALTIME`. Production-style physical synchronization uses
`TIME_ESTABLISH`.

## Decision Policy

CREngine XPointer location is authoritative. Percentage is auxiliary and never
orders records.

| Local | Remote | Decision | Mutation |
|---|---|---|---|
| missing | missing | `NO_STATE` | none |
| present | missing | `LOCAL_SELECTED` | PUT when percentage is serializable |
| missing | present | `REMOTE_SELECTED` | save remote position and percentage locally |
| present | same XPointer | `NO_CHANGE` | none |
| present | different XPointer | `AMBIGUOUS` | none |

Equal XPointers return `NO_CHANGE` even when percentages differ. Timestamps,
percentage ordering, and the protocol core's older/newer classifier are not
used because local persistence has no comparable timestamp.

A local record with `progress_10000 == -1` and no remote record returns
`CN_KOSYNC_SYNC_STATUS_LOCAL_UNSUPPORTED` with decision `LOCAL_SELECTED`.
`local_load_result` remains `CN_PROGRESS_OK`, and no PUT occurs.

GET-missing followed by PUT has a concurrent-create race. The current KOSync
protocol has no conditional PUT, revision, or ETag. This operation never PUTs
when a remote record was returned, but it cannot prevent a different client
from creating one between this GET and PUT. Solving that race is deferred.

## Errors

The integration distinguishes identity, local store, trusted-time, DNS,
authentication, HTTPS transport, KOSync protocol, and allocation failures.
Exact underlying component results remain in the result structure. Completed
`NO_STATE`, `NO_CHANGE`, and `AMBIGUOUS` decisions have status `OK`.

## Credentials

Authentication remains `x-auth-user` plus `x-auth-key`. The integration does
not persist credentials. The diagnostic reads exactly two lines from a
caller-supplied credential file, passes copied values to `cn_kosync_client`,
and never prints the key. Mock transcripts contain username and protocol data
but no authentication key.

Host and hardware fixtures use dummy credentials only. Production credentials
must not be placed in the repository. Physical validation credentials remain
under `/tmp` and are removed after the test.

## Host Gate

Run:

```bash
bash testapp/build-kosync.sh
```

The focused gate uses the existing prebuilt CREngine, FreeType, and BearSSL
libraries. It does not rebuild those dependencies. It builds one static ARM
EABI5 soft-float non-PIE diagnostic and runs under qemu-arm.

The gate covers:

- local-only GET-missing followed by an exact PUT;
- remote-only import and byte-for-byte logical-position persistence;
- both missing;
- equal XPointer with equal percentage;
- equal XPointer with different percentage;
- different XPointer with equal percentage;
- different XPointer with different percentage;
- local unknown percentage;
- authentication rejection with HTTP 401 retained;
- DNS NXDOMAIN propagation before HTTPS;
- trusted-time failure before DNS;
- wrong TLS hostname rejection;
- malformed KOSync response;
- TLS receive timeout;
- insecure HTTP rejection;
- exact EPUB Binary identity in GET and PUT;
- exact local position and percentage in PUT;
- one and only one integration PUT, for the remote-missing case; and
- credential-redacted transcript plus original Host/SNI identity.

The prior focused KOSync protocol API, mock, round-trip, and network-error
regressions run in the same gate.

## Resource Bounds

The orchestration frame adds approximately 3 KiB of fixed stack state. DNS adds
about 1 KiB while active. Existing TLS remains the peak stack consumer: its
connection state plus nested CA loading storage can exceed 128 KiB. No new
large network buffer is introduced.

Existing GET allocation remains approximately 401 KiB plus up to 65 KiB each
for local and remote logical positions. Existing PUT allocation remains about
799 KiB plus the retained local position, for an approximate peak below 0.9
MiB. Progress serialization uses its existing bounded record allocation after
network buffers have been released.

## Physical Validation

Hardware revalidation passed on the physical Nook Simple Touch. The validated
paths covered:

- real trusted-time establishment via `cn_timesimple_sync()`;
- DNS-routed verified HTTPS while preserving hostname identity;
- local-only progress, remote miss, and PUT;
- repeated sync selecting `NO_CHANGE`;
- remote-only progress import;
- persisted remote XPointer/progress verified via `--local-get`;
- wrong credentials returning `auth-failed` with HTTP 401;
- wrong hostname returning `https-failed` with `tls-hostname-mismatch`; and
- corrected `caller-established` plus epoch `-` behavior on physical ARM.

The final caller-established hardware revalidation command produced:

```text
KOSYNC SYNC status=ok decision=no-change local=1 remote=1 saved=0 uploaded=0 document=e1a1e9016cfc9bca8c694187943e9c4f identity=ok load=ok save=invalid time=ok dns=ok kosync=ok http=200 transport=ok timestamp=-1 remote_progress=3210 remote_position=/body/DocFragment[1]/body/p[1]/text().0
```

The diagnostic defect found during validation was limited to CLI parsing:
`caller-established` with epoch `-` previously attempted `strtoll("-")` and
returned before sync. Through the ADB shell invocation this appeared as a silent
no-op. The diagnostic now treats `-` as caller-established wall-clock time,
leaves `CLOCK_REALTIME` unchanged, preserves numeric epoch injection for host
tests, and leaves `sync` mode on `cn_timesimple_sync()`. Production
`src/sync/kosync_sync.[ch]` did not change for this bugfix.

The validated ARM artifact SHA-256 was:

```text
3e4f84a0cca66eb4a4f73b2ab9abc68707206eb860c10bc8e4c8bc6ff499b5bc
```

The procedure below is the `/tmp`-only reproduction path. It must not use
`/data`, update the RTC, alter resolver or Wi-Fi configuration, or write
boot/system partitions.

On Windows, from the repository root, create dummy credential files without a
UTF-8 BOM. These use only the repository's existing test key:

```powershell
$Utf8 = New-Object System.Text.UTF8Encoding($false)
$DummyKey = 'dfb450efddbb5387197c84460623675b'
[IO.File]::WriteAllText("$env:TEMP\crossnook-local.credentials", "integration-local-only`n$DummyKey`n", $Utf8)
[IO.File]::WriteAllText("$env:TEMP\crossnook-remote.credentials", "integration-remote-only`n$DummyKey`n", $Utf8)
[IO.File]::WriteAllText("$env:TEMP\crossnook-wrong.credentials", "integration-auth`nwrong-key`n", $Utf8)
```

Set the controlled Windows LAN address and start three server windows:

```powershell
Remove-Item "$env:TEMP\crossnook-kosync-device.jsonl","$env:TEMP\crossnook-kosync-device-sni.log","$env:TEMP\crossnook-kosync-device-host.log" -ErrorAction SilentlyContinue
```

```powershell
$PcIp = '192.168.0.107' # replace if the controlled PC address changed
python .\testapp\sntp_mock_server.py --host 0.0.0.0 --port 19123 --timestamp 1790000000
```

```powershell
$PcIp = '192.168.0.107'
python .\testapp\dns_mock_server.py --host 0.0.0.0 --port 19153 --answer $PcIp --verbose
```

```powershell
python .\testapp\https_mock_server.py --host 0.0.0.0 --port 18443 `
  --cert .\testapp\pki\server.crt --key .\testapp\pki\server.key `
  --integration-fixtures --transcript "$env:TEMP\crossnook-kosync-device.jsonl" `
  --sni-log "$env:TEMP\crossnook-kosync-device-sni.log" `
  --host-log "$env:TEMP\crossnook-kosync-device-host.log" --quiet
```

Allow only the controlled Private-network UDP/19123, UDP/19153, and TCP/18443
listeners. Then push temporary artifacts:

```powershell
$Adb = 'C:\platform-tools\adb.exe'
& $Adb push .\testapp\crossnook-kosync-test /tmp/crossnook-kosync-test
& $Adb push .\testapp\cre-fixtures\test.epub /tmp/crossnook-sync-test.epub
& $Adb push .\testapp\pki\testca.crt /tmp/crossnook-testca.crt
& $Adb push "$env:TEMP\crossnook-local.credentials" /tmp/crossnook-local.credentials
& $Adb push "$env:TEMP\crossnook-remote.credentials" /tmp/crossnook-remote.credentials
& $Adb push "$env:TEMP\crossnook-wrong.credentials" /tmp/crossnook-wrong.credentials
& $Adb shell 'chmod 755 /tmp/crossnook-kosync-test; chmod 600 /tmp/crossnook-*.credentials; rm -rf /tmp/crossnook-sync-local /tmp/crossnook-sync-remote; mkdir -p /tmp/crossnook-sync-local /tmp/crossnook-sync-remote'
```

Seed a local-only record, then perform real SNTP, DNS, verified HTTPS, GET, and
PUT. Replace the PC address if necessary:

```powershell
$PcIp = '192.168.0.107'
& $Adb shell "/tmp/crossnook-kosync-test --local-set /tmp/crossnook-sync-local /tmp/crossnook-sync-test.epub '/body/DocFragment[1]/body/p[1]/text().0' 3210"
& $Adb shell "/tmp/crossnook-kosync-test --sync-once /tmp/crossnook-sync-local /tmp/crossnook-sync-test.epub $PcIp 19153 2000 sync $PcIp 19123 3000 https://secure.test.local:18443 /tmp/crossnook-testca.crt /tmp/crossnook-local.credentials crossnook-nook crossnook-nook-test - 10000"
```

The first result must be `decision=local-selected`, `uploaded=1`, and document
`e1a1e9016cfc9bca8c694187943e9c4f`. Repeating the same command must return
`decision=no-change`.

After the preceding `sync` command has established valid wall time, revalidate
the caller-established `-` diagnostic path without changing the clock:

```powershell
$PcIp = '192.168.0.107'
& $Adb shell "/tmp/crossnook-kosync-test --sync-once /tmp/crossnook-sync-local /tmp/crossnook-sync-test.epub $PcIp 19153 2000 caller-established $PcIp 19123 3000 https://secure.test.local:18443 /tmp/crossnook-testca.crt /tmp/crossnook-local.credentials crossnook-nook crossnook-nook-test - 10000"
```

It must print a normal `KOSYNC SYNC` result with `status=ok`,
`decision=no-change`, `time=ok`, `dns=ok`, `kosync=ok`, and `transport=ok`.

Exercise remote-only import and verify its persisted value:

```powershell
$PcIp = '192.168.0.107'
& $Adb shell "/tmp/crossnook-kosync-test --sync-once /tmp/crossnook-sync-remote /tmp/crossnook-sync-test.epub $PcIp 19153 2000 sync $PcIp 19123 3000 https://secure.test.local:18443 /tmp/crossnook-testca.crt /tmp/crossnook-remote.credentials crossnook-nook crossnook-nook-test - 10000"
& $Adb shell "/tmp/crossnook-kosync-test --local-get /tmp/crossnook-sync-remote /tmp/crossnook-sync-test.epub"
```

The sync must report `decision=remote-selected`, `saved=1`, position
`/body/DocFragment[1]/body/p[3]/text().5`, and percentage `6543`. The local GET
must report the same values.

Verify authentication and hostname rejection without mutating either side:

```powershell
$PcIp = '192.168.0.107'
& $Adb shell "/tmp/crossnook-kosync-test --sync-once /tmp/crossnook-sync-remote /tmp/crossnook-sync-test.epub $PcIp 19153 2000 sync $PcIp 19123 3000 https://secure.test.local:18443 /tmp/crossnook-testca.crt /tmp/crossnook-wrong.credentials crossnook-nook crossnook-nook-test - 10000"
& $Adb shell "/tmp/crossnook-kosync-test --sync-once /tmp/crossnook-sync-remote /tmp/crossnook-sync-test.epub $PcIp 19153 2000 sync $PcIp 19123 3000 https://wrong.test:18443 /tmp/crossnook-testca.crt /tmp/crossnook-remote.credentials crossnook-nook crossnook-nook-test - 10000"
```

The first command must report `status=auth-failed`, `http=401`; the second must
report `status=https-failed`, `transport=tls-hostname-mismatch`.

After recording outputs, remove only temporary artifacts:

```powershell
& $Adb shell 'rm -rf /tmp/crossnook-sync-local /tmp/crossnook-sync-remote /tmp/crossnook-kosync-test /tmp/crossnook-sync-test.epub /tmp/crossnook-testca.crt /tmp/crossnook-local.credentials /tmp/crossnook-remote.credentials /tmp/crossnook-wrong.credentials'
Remove-Item "$env:TEMP\crossnook-local.credentials","$env:TEMP\crossnook-remote.credentials","$env:TEMP\crossnook-wrong.credentials" -ErrorAction SilentlyContinue
```

These commands, including the negative auth and wrong-host checks, are what the
hardware revalidation status above records.

## Deferred

- persisted local timestamps, revisions, and device metadata;
- automatic newest-wins selection;
- conditional PUT/concurrent-create protection;
- persistent credentials and account/settings UI;
- live reader apply/prompt UX;
- background scheduling, retries, and Wi-Fi lifecycle changes; and
- public KOSync service validation.

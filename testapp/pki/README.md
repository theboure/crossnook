# TEST-ONLY PKI

Every certificate and private key in this directory is public test material
for the CrossNook HTTPS validation harness. Never use these keys, certificates,
or roots for production or any non-test service.

`testca.crt` is the only trust anchor used by successful tests. `otherca.crt`
proves distrust. `expired.crt`, `notyet.crt`, and `missing-chain.crt` exercise
validity and chain failures. The DER files are consumed only by the in-process
BearSSL test server in `crossnook-net-test`.

`../gen-pki.sh` reissues the fixtures deterministically from the committed
TEST-ONLY keys and fixed dates. No production code has a default path to this
directory or to any certificate in it.

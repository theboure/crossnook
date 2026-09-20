#!/usr/bin/env bash
# Reissue the committed TEST-ONLY TLS fixtures with fixed validity windows.
# The committed private keys are intentionally public test material and must
# never be used outside this validation harness.
set -euo pipefail
export MSYS2_ARG_CONV_EXCL='/CN='

PKI="$(cd "$(dirname "$0")" && pwd)/pki"
TMP=$(mktemp -d)
TMP_NATIVE=$(cygpath -m "$TMP" 2>/dev/null || printf '%s' "$TMP")
trap 'rm -rf "$TMP"' EXIT

for key in testca.key otherca.key server.key distrust.key intermediate.key missing-chain.key; do
  test -f "$PKI/$key" || {
    echo "missing committed TEST-ONLY key: $PKI/$key" >&2
    exit 1
  }
done
for ca in testca.crt otherca.crt; do
  test -f "$PKI/$ca" || {
    echo "missing committed TEST-ONLY CA: $PKI/$ca" >&2
    exit 1
  }
done

cat > "$TMP/ca.conf" <<EOF
[ ca ]
default_ca = issue
[ issue ]
database = $TMP_NATIVE/index.txt
serial = $TMP_NATIVE/serial
new_certs_dir = $TMP_NATIVE
default_md = sha256
policy = policy_any
email_in_dn = no
unique_subject = no
[ policy_any ]
commonName = supplied
EOF

issue() {
  local ca_cert=$1 ca_key=$2 key=$3 subject=$4 serial=$5
  local start=$6 end=$7 output=$8 extensions=$9

  : > "$TMP/index.txt"
  printf '%s\n' "$serial" > "$TMP/serial"
  openssl req -new -key "$PKI/$key" -subj "$subject" -out "$TMP/request.csr"
  openssl ca -config "$TMP/ca.conf" -batch \
    -cert "$PKI/$ca_cert" -keyfile "$PKI/$ca_key" \
    -in "$TMP/request.csr" -out "$PKI/$output" \
    -startdate "$start" -enddate "$end" \
    -extensions requested -extfile "$extensions" >/dev/null
}

cat > "$TMP/server.ext" <<'EOF'
[ requested ]
subjectAltName=DNS:secure.test.local,DNS:localhost,IP:127.0.0.1
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature
extendedKeyUsage=serverAuth
EOF

cat > "$TMP/intermediate.ext" <<'EOF'
[ requested ]
basicConstraints=critical,CA:TRUE,pathlen:0
keyUsage=critical,keyCertSign,cRLSign
subjectKeyIdentifier=hash
authorityKeyIdentifier=keyid,issuer
EOF

# Fixed dates make host validation independent of fixture-generation time.
issue testca.crt testca.key server.key "/CN=secure.test.local" 01 \
  20250101000000Z 20301231000000Z server.crt "$TMP/server.ext"
issue testca.crt testca.key server.key "/CN=secure.test.local" 02 \
  20210101000000Z 20230101000000Z expired.crt "$TMP/server.ext"
issue testca.crt testca.key server.key "/CN=secure.test.local" 03 \
  20310101000000Z 20320101000000Z notyet.crt "$TMP/server.ext"
issue otherca.crt otherca.key distrust.key "/CN=secure.test.local" 01 \
  20250101000000Z 20301231000000Z distrust.crt "$TMP/server.ext"
issue testca.crt testca.key intermediate.key "/CN=CrossNook Test Intermediate" 04 \
  20250101000000Z 20301231000000Z intermediate.crt "$TMP/intermediate.ext"
issue intermediate.crt intermediate.key missing-chain.key "/CN=secure.test.local" 01 \
  20250101000000Z 20301231000000Z missing-chain.crt "$TMP/server.ext"

openssl pkey -in "$PKI/server.key" -outform DER \
  -out "$PKI/server.key.der"
openssl pkey -in "$PKI/distrust.key" -outform DER \
  -out "$PKI/distrust.key.der"
for cert in server testca otherca distrust expired notyet; do
  openssl x509 -in "$PKI/$cert.crt" -outform DER -out "$PKI/$cert.crt.der"
done

printf 'garbage, not a certificate\n' > "$PKI/garbage-ca.pem"
: > "$PKI/empty-ca.pem"
echo "TEST-ONLY PKI fixtures reissued under $PKI"

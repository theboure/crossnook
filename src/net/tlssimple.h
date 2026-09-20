/* Minimal bounded TLS/1.2 client for the Wi-Fi transport bring-up.
 *
 * Unlike the plain net layer, this component requires a BearSSL static
 * library (see toolchain/bearssl/pin.txt).  All buffers live inside the
 * caller-provided cn_tls_conn; no heap allocation happens here.
 *
 * Certificate validation is enforced (no skip-verify mode). The required
 * DNS service name is used for both SNI and BearSSL's SAN/CN verification.
 * Callers that cannot use DNS may route the socket to a numeric address
 * separately while still authenticating the service's DNS identity.
 */
#ifndef CN_NET_TLSSIMPLE_H
#define CN_NET_TLSSIMPLE_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include <bearssl.h>

#define CN_TLS_ENTROPY_PATH_DEFAULT "/dev/urandom"
#define CN_TLS_ENTROPY_MIN_BYTES 32
#define CN_TLS_ENTROPY_MAX_BYTES 64
#define CN_TLS_CA_MAX_BYTES 65536
#define CN_TLS_MAX_ANCHORS 8
#define CN_TLS_CA_DN_MAX 512
#define CN_TLS_CA_KEY_MAX 1024

typedef enum cn_tls_result {
    CN_TLS_OK = 0,
    CN_TLS_INVALID,
    CN_TLS_INTERNAL,
    CN_TLS_ENTROPY_FAILED,
    CN_TLS_INVALID_CA,
    CN_TLS_HANDSHAKE_FAILED,
    CN_TLS_HANDSHAKE_TIMEOUT,
    CN_TLS_TRUST_FAILED,
    CN_TLS_HOSTNAME_MISMATCH,
    CN_TLS_CERT_TIME_FAILED,
    CN_TLS_CERT_INVALID,
    CN_TLS_PROTOCOL_FAILED,
    CN_TLS_CLOSED,
    CN_TLS_RECV_TIMEOUT,
    CN_TLS_SEND_TIMEOUT,
    CN_TLS_LENGTH
} cn_tls_result;

typedef struct cn_tls_config {
    const char *ca_path;       /* PEM file with trusted CA anchors */
    const char *entropy_path;  /* entropy source; NULL => /dev/urandom */
    time_t (*get_time)(void);  /* wall clock; NULL => time() */
} cn_tls_config;

typedef struct cn_tls_ta {
    br_x509_trust_anchor anchor;
    unsigned char dn[CN_TLS_CA_DN_MAX];
    unsigned char key[CN_TLS_CA_KEY_MAX];
} cn_tls_ta;

typedef struct cn_tls_conn {
    int fd;
    int phase; /* 0 = handshake, 1 = data */
    int anchor_count;
    unsigned char iobuf[BR_SSL_BUFSIZE_MONO];
    br_ssl_client_context cc;
    br_x509_minimal_context xc;
    cn_tls_ta anchors[CN_TLS_MAX_ANCHORS];
    unsigned char work[CN_TLS_CA_MAX_BYTES];
    unsigned char entropy[CN_TLS_ENTROPY_MAX_BYTES];
} cn_tls_conn;

/* Ownership: cn_tls_open takes ownership of fd on success (closes it
 * via cn_tls_close).  On failure it closes fd itself and reports the
 * failure; caller must not use fd afterwards. */
cn_tls_result cn_tls_open(cn_tls_conn *conn, int fd,
                          const cn_tls_config *cfg,
                          const char *server_name,
                          int64_t deadline_ms);
/* deadline_ms is an absolute CLOCK_MONOTONIC deadline for the whole call. */
cn_tls_result cn_tls_send_all(cn_tls_conn *conn, const void *data,
                              size_t len, int64_t deadline_ms);
cn_tls_result cn_tls_recv_some(cn_tls_conn *conn, void *buf, size_t cap,
                               int64_t deadline_ms, size_t *got,
                               int *closed);
int cn_tls_conn_closed(cn_tls_conn *conn);
void cn_tls_close(cn_tls_conn *conn);
const char *cn_tls_result_name(cn_tls_result result);

#endif /* CN_NET_TLSSIMPLE_H */

/* Minimal bounded TLS 1.2 client (BearSSL 0.6 pin, see toolchain/bearssl). */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "net/tlssimple.h"

#define CN_TLS_X509_ERR_MIN 32
#define CN_TLS_X509_ERR_MAX 62

static int64_t tls_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int tls_poll_interval_ms(int64_t deadline)
{
    int64_t remaining = deadline - tls_now_ms();

    if (remaining < 0)
        return -1;
    if (remaining > INT_MAX)
        return INT_MAX;
    return (int)remaining;
}

typedef struct cn_blob {
    unsigned char *buf;
    size_t cap;
    size_t len;
    int overflow;
} cn_blob;

static void blob_append(void *cc, const void *data, size_t len)
{
    cn_blob *b = cc;

    if (b->len > b->cap || len > b->cap - b->len) {
        b->overflow = 1;
        return;
    }
    memcpy(b->buf + b->len, data, len);
    b->len += len;
}

static int read_bounded_file(const char *path, unsigned char *buf,
                             size_t cap, size_t *out)
{
    int fd;
    size_t total = 0;

    if (!path)
        return 0;
    fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0)
        return 0;
    while (total < cap) {
        ssize_t n = read(fd, buf + total, cap - total);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            return 0;
        }
        if (n == 0) {
            close(fd);
            *out = total;
            return 1;
        }
        total += (size_t)n;
    }
    for (;;) {
        unsigned char extra;
        ssize_t n = read(fd, &extra, 1);

        if (n < 0 && errno == EINTR)
            continue;
        close(fd);
        if (n != 0)
            return 0;
        *out = total;
        return 1;
    }
}

static int decode_anchor(cn_tls_ta *ta, const unsigned char *der, size_t len)
{
    br_x509_decoder_context dc;
    cn_blob dn;
    const br_x509_pkey *pk;
    size_t half = CN_TLS_CA_KEY_MAX / 2;

    if (!der || len == 0)
        return 0;
    memset(ta, 0, sizeof *ta);
    dn.buf = ta->dn;
    dn.cap = sizeof ta->dn;
    dn.len = 0;
    dn.overflow = 0;
    br_x509_decoder_init(&dc, blob_append, &dn);
    br_x509_decoder_push(&dc, der, len);
    if (br_x509_decoder_last_error(&dc) != 0 || dn.overflow)
        return 0;
    pk = br_x509_decoder_get_pkey(&dc);
    if (!pk)
        return 0;
    ta->anchor.dn.data = ta->dn;
    ta->anchor.dn.len = dn.len;
    ta->anchor.flags = br_x509_decoder_isCA(&dc) ? BR_X509_TA_CA : 0;
    if (pk->key_type == BR_KEYTYPE_RSA) {
        if (pk->key.rsa.nlen == 0 || pk->key.rsa.nlen > half ||
            pk->key.rsa.elen == 0 || pk->key.rsa.elen > half)
            return 0;
        memcpy(ta->key, pk->key.rsa.n, pk->key.rsa.nlen);
        memcpy(ta->key + half, pk->key.rsa.e, pk->key.rsa.elen);
        ta->anchor.pkey.key_type = BR_KEYTYPE_RSA;
        ta->anchor.pkey.key.rsa.n = ta->key;
        ta->anchor.pkey.key.rsa.nlen = pk->key.rsa.nlen;
        ta->anchor.pkey.key.rsa.e = ta->key + half;
        ta->anchor.pkey.key.rsa.elen = pk->key.rsa.elen;
        return 1;
    }
    if (pk->key_type == BR_KEYTYPE_EC) {
        if (pk->key.ec.qlen == 0 || pk->key.ec.qlen > sizeof ta->key)
            return 0;
        memcpy(ta->key, pk->key.ec.q, pk->key.ec.qlen);
        ta->anchor.pkey.key_type = BR_KEYTYPE_EC;
        ta->anchor.pkey.key.ec.curve = pk->key.ec.curve;
        ta->anchor.pkey.key.ec.q = ta->key;
        ta->anchor.pkey.key.ec.qlen = pk->key.ec.qlen;
        return 1;
    }
    return 0;
}

static int is_pem_certificate(const char *name)
{
    static const char bare[] = "CERTIFICATE";
    const char *p;

    if (!name || strncmp(name, bare, sizeof bare - 1) != 0)
        return 0;
    p = name + sizeof bare - 1;
    while (*p == '-')
        p++;
    return *p == '\0';
}

static cn_tls_result load_anchors(cn_tls_conn *conn, const cn_tls_config *cfg)
{
    unsigned char filebuf[CN_TLS_CA_MAX_BYTES];
    size_t file_len;
    br_pem_decoder_context pd;
    const unsigned char *buf;
    size_t len;
    cn_blob der;
    int inobj = 0;
    int is_cert = 0;
    int extra_nl = 1;
    int count = 0;

    if (!cfg || !cfg->ca_path ||
        !read_bounded_file(cfg->ca_path, filebuf, sizeof filebuf, &file_len))
        return CN_TLS_INVALID_CA;
    der.buf = conn->work;
    der.cap = sizeof conn->work;
    br_pem_decoder_init(&pd);
    buf = filebuf;
    len = file_len;
    while (len > 0 || extra_nl) {
        size_t consumed;
        int event;

        if (len == 0) {
            extra_nl = 0;
            buf = (const unsigned char *)"\n";
            len = 1;
        }
        consumed = br_pem_decoder_push(&pd, buf, len);
        buf += consumed;
        len -= consumed;
        event = br_pem_decoder_event(&pd);
        if (event == BR_PEM_BEGIN_OBJ) {
            inobj = 1;
            der.len = 0;
            der.overflow = 0;
            is_cert = is_pem_certificate(br_pem_decoder_name(&pd));
            if (is_cert)
                br_pem_decoder_setdest(&pd, blob_append, &der);
        } else if (event == BR_PEM_END_OBJ) {
            if (inobj && is_cert) {
                if (count >= CN_TLS_MAX_ANCHORS || der.overflow ||
                    der.len == 0 ||
                    !decode_anchor(&conn->anchors[count], der.buf, der.len))
                    return CN_TLS_INVALID_CA;
                count++;
            }
            inobj = 0;
        } else if (event == BR_PEM_ERROR) {
            return CN_TLS_INVALID_CA;
        }
    }
    if (inobj || count == 0)
        return CN_TLS_INVALID_CA;
    conn->anchor_count = count;
    return CN_TLS_OK;
}

static cn_tls_result wait_for_entropy_ready(int64_t deadline)
{
    int fd = open("/dev/random", O_RDONLY | O_NONBLOCK);
    struct pollfd pfd;

    if (fd < 0)
        return CN_TLS_ENTROPY_FAILED;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    for (;;) {
        int timeout = tls_poll_interval_ms(deadline);
        int result;

        if (timeout < 0) {
            close(fd);
            return CN_TLS_ENTROPY_FAILED;
        }
        result = poll(&pfd, 1, timeout);
        if (result > 0) {
            close(fd);
            return (pfd.revents & POLLIN) ? CN_TLS_OK
                                         : CN_TLS_ENTROPY_FAILED;
        }
        if (result == 0) {
            close(fd);
            return CN_TLS_ENTROPY_FAILED;
        }
        if (errno != EINTR) {
            close(fd);
            return CN_TLS_ENTROPY_FAILED;
        }
    }
}

static cn_tls_result read_entropy(cn_tls_conn *conn, const cn_tls_config *cfg,
                                  int64_t deadline)
{
    const char *path = cfg && cfg->entropy_path
                           ? cfg->entropy_path
                           : CN_TLS_ENTROPY_PATH_DEFAULT;
    int fd;
    size_t have = 0;

    if ((!cfg || !cfg->entropy_path) &&
        wait_for_entropy_ready(deadline) != CN_TLS_OK)
        return CN_TLS_ENTROPY_FAILED;
    fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0)
        return CN_TLS_ENTROPY_FAILED;
    while (have < CN_TLS_ENTROPY_MIN_BYTES) {
        ssize_t n = read(fd, conn->entropy + have,
                         CN_TLS_ENTROPY_MIN_BYTES - have);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pfd;
                int timeout = tls_poll_interval_ms(deadline);

                if (timeout < 0) {
                    close(fd);
                    return CN_TLS_ENTROPY_FAILED;
                }
                pfd.fd = fd;
                pfd.events = POLLIN;
                pfd.revents = 0;
                if (poll(&pfd, 1, timeout) > 0)
                    continue;
            }
            close(fd);
            return CN_TLS_ENTROPY_FAILED;
        }
        if (n == 0) {
            close(fd);
            return CN_TLS_ENTROPY_FAILED;
        }
        have += (size_t)n;
    }
    close(fd);
    return CN_TLS_OK;
}

static cn_tls_result classify_engine(cn_tls_conn *conn)
{
    int e = br_ssl_engine_last_error(&conn->cc.eng);

    if (e == BR_ERR_OK)
        return CN_TLS_CLOSED;
    if (e == BR_ERR_NO_RANDOM)
        return CN_TLS_ENTROPY_FAILED;
    if (e >= CN_TLS_X509_ERR_MIN && e <= CN_TLS_X509_ERR_MAX) {
        if (e == BR_ERR_X509_NOT_TRUSTED)
            return CN_TLS_TRUST_FAILED;
        if (e == BR_ERR_X509_BAD_SERVER_NAME)
            return CN_TLS_HOSTNAME_MISMATCH;
        if (e == BR_ERR_X509_EXPIRED || e == BR_ERR_X509_TIME_UNKNOWN)
            return CN_TLS_CERT_TIME_FAILED;
        return CN_TLS_CERT_INVALID;
    }
    return conn->phase == 0 ? CN_TLS_HANDSHAKE_FAILED : CN_TLS_PROTOCOL_FAILED;
}

static cn_tls_result io_flush_out(cn_tls_conn *conn, int64_t deadline)
{
    for (;;) {
        size_t len;
        unsigned char *buf = br_ssl_engine_sendrec_buf(&conn->cc.eng, &len);
        struct pollfd pfd;
        int pm;
        int polled;
        ssize_t written;

        if (len == 0)
            return CN_TLS_OK;
        pm = tls_poll_interval_ms(deadline);
        if (pm < 0)
            return conn->phase == 0 ? CN_TLS_HANDSHAKE_TIMEOUT
                                    : CN_TLS_SEND_TIMEOUT;
        pfd.fd = conn->fd;
        pfd.events = POLLOUT;
        pfd.revents = 0;
        polled = poll(&pfd, 1, pm);
        if (polled == 0)
            return conn->phase == 0 ? CN_TLS_HANDSHAKE_TIMEOUT
                                    : CN_TLS_SEND_TIMEOUT;
        if (polled < 0) {
            if (errno == EINTR)
                continue;
            return CN_TLS_INTERNAL;
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
            return CN_TLS_PROTOCOL_FAILED;
        written = send(conn->fd, buf, len, MSG_NOSIGNAL);
        if (written < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            return CN_TLS_PROTOCOL_FAILED;
        }
        if (written == 0)
            return CN_TLS_PROTOCOL_FAILED;
        br_ssl_engine_sendrec_ack(&conn->cc.eng, (size_t)written);
    }
}

static cn_tls_result io_ingest_in(cn_tls_conn *conn, int64_t deadline)
{
    for (;;) {
        size_t len;
        unsigned char *buf = br_ssl_engine_recvrec_buf(&conn->cc.eng, &len);
        struct pollfd pfd;
        int pm;
        int polled;
        ssize_t received;

        if (len == 0)
            return CN_TLS_OK;
        pm = tls_poll_interval_ms(deadline);
        if (pm < 0)
            return conn->phase == 0 ? CN_TLS_HANDSHAKE_TIMEOUT
                                    : CN_TLS_RECV_TIMEOUT;
        pfd.fd = conn->fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        polled = poll(&pfd, 1, pm);
        if (polled == 0)
            return conn->phase == 0 ? CN_TLS_HANDSHAKE_TIMEOUT
                                    : CN_TLS_RECV_TIMEOUT;
        if (polled < 0) {
            if (errno == EINTR)
                continue;
            return CN_TLS_INTERNAL;
        }
        if (pfd.revents & POLLNVAL)
            return CN_TLS_PROTOCOL_FAILED;
        received = recv(conn->fd, buf, len, 0);
        if (received == 0)
            return CN_TLS_PROTOCOL_FAILED;
        if (received < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            return CN_TLS_PROTOCOL_FAILED;
        }
        br_ssl_engine_recvrec_ack(&conn->cc.eng, (size_t)received);
        return CN_TLS_OK;
    }
}

static cn_tls_result tls_handshake(cn_tls_conn *conn, int64_t deadline)
{
    for (;;) {
        unsigned st = br_ssl_engine_current_state(&conn->cc.eng);

        if (st & BR_SSL_CLOSED)
            return classify_engine(conn);
        if (st & BR_SSL_SENDREC) {
            cn_tls_result r = io_flush_out(conn, deadline);

            if (r != CN_TLS_OK)
                return r;
            continue;
        }
        if (st & (BR_SSL_SENDAPP | BR_SSL_RECVAPP))
            return CN_TLS_OK;
        if (st & BR_SSL_RECVREC) {
            cn_tls_result r = io_ingest_in(conn, deadline);

            if (r != CN_TLS_OK)
                return r;
            continue;
        }
        return CN_TLS_INTERNAL;
    }
}

static int valid_server_name(const char *name)
{
    size_t i;

    if (!name || !name[0])
        return 0;
    for (i = 0; name[i]; i++) {
        unsigned char c = (unsigned char)name[i];

        if (i >= 255 || c <= 0x20 || c == 0x7f || c == '/' || c == '\\')
            return 0;
    }
    return 1;
}

cn_tls_result cn_tls_open(cn_tls_conn *conn, int fd, const cn_tls_config *cfg,
                          const char *server_name, int64_t deadline_ms)
{
    static const uint16_t suites[] = {
        BR_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
        BR_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
        BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
        BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
        BR_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
        BR_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384
    };
    time_t wall;
    int64_t seconds_since_epoch;
    uint32_t days;
    uint32_t seconds;
    cn_tls_result r;
    int flags;

    if (!conn || fd < 0 || !cfg || !valid_server_name(server_name)) {
        if (fd >= 0)
            close(fd);
        return CN_TLS_INVALID;
    }
    memset(conn, 0, sizeof *conn);
    conn->fd = fd;
    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        r = CN_TLS_INTERNAL;
        goto fail;
    }
    r = load_anchors(conn, cfg);
    if (r != CN_TLS_OK)
        goto fail;

    br_ssl_client_init_full(&conn->cc, &conn->xc,
                            &conn->anchors[0].anchor, conn->anchor_count);
    br_ssl_engine_set_versions(&conn->cc.eng, BR_TLS12, BR_TLS12);
    br_ssl_engine_set_suites(&conn->cc.eng, suites,
                             sizeof suites / sizeof suites[0]);
    br_ssl_engine_add_flags(&conn->cc.eng, BR_OPT_NO_RENEGOTIATION);
    br_x509_minimal_set_minrsa(&conn->xc, 256);

    wall = cfg->get_time ? cfg->get_time() : time(NULL);
    if (wall == (time_t)-1 || wall < 0) {
        r = CN_TLS_INTERNAL;
        goto fail;
    }
    seconds_since_epoch = (int64_t)wall;
    days = (uint32_t)(seconds_since_epoch / 86400) + 719528;
    seconds = (uint32_t)(seconds_since_epoch % 86400);
    br_x509_minimal_set_time(&conn->xc, days, seconds);

    r = read_entropy(conn, cfg, deadline_ms);
    if (r != CN_TLS_OK)
        goto fail;
    br_ssl_engine_inject_entropy(&conn->cc.eng, conn->entropy,
                                 CN_TLS_ENTROPY_MIN_BYTES);
    br_ssl_engine_set_buffer(&conn->cc.eng, conn->iobuf, sizeof conn->iobuf, 0);
    if (!br_ssl_client_reset(&conn->cc, server_name, 0)) {
        r = classify_engine(conn);
        goto fail;
    }
    r = tls_handshake(conn, deadline_ms);
    if (r != CN_TLS_OK)
        goto fail;
    conn->phase = 1;
    return CN_TLS_OK;

fail:
    close(fd);
    conn->fd = -1;
    return r;
}

cn_tls_result cn_tls_send_all(cn_tls_conn *conn, const void *data, size_t len,
                              int64_t deadline_ms)
{
    const unsigned char *p = data;
    size_t left = len;

    if (!conn || conn->fd < 0 || (len != 0 && !data))
        return CN_TLS_INVALID;
    while (left > 0) {
        unsigned st = br_ssl_engine_current_state(&conn->cc.eng);

        if (st & BR_SSL_CLOSED)
            return classify_engine(conn);
        if (st & BR_SSL_SENDAPP) {
            size_t cap;
            unsigned char *buf = br_ssl_engine_sendapp_buf(&conn->cc.eng, &cap);
            size_t chunk = left < cap ? left : cap;

            if (chunk == 0)
                return CN_TLS_INTERNAL;
            memcpy(buf, p, chunk);
            br_ssl_engine_sendapp_ack(&conn->cc.eng, chunk);
            br_ssl_engine_flush(&conn->cc.eng, 0);
            p += chunk;
            left -= chunk;
            continue;
        }
        if (st & BR_SSL_SENDREC) {
            cn_tls_result r = io_flush_out(conn, deadline_ms);

            if (r != CN_TLS_OK)
                return r;
            continue;
        }
        if (st & BR_SSL_RECVAPP)
            return CN_TLS_PROTOCOL_FAILED;
        if (st & BR_SSL_RECVREC) {
            cn_tls_result r = io_ingest_in(conn, deadline_ms);

            if (r != CN_TLS_OK)
                return r;
            continue;
        }
        return CN_TLS_INTERNAL;
    }
    for (;;) {
        unsigned st = br_ssl_engine_current_state(&conn->cc.eng);

        if (st & BR_SSL_CLOSED)
            return classify_engine(conn);
        if (st & BR_SSL_SENDREC) {
            cn_tls_result r = io_flush_out(conn, deadline_ms);

            if (r != CN_TLS_OK)
                return r;
            continue;
        }
        return CN_TLS_OK;
    }
}

cn_tls_result cn_tls_recv_some(cn_tls_conn *conn, void *buf, size_t cap,
                               int64_t deadline_ms, size_t *got, int *closed)
{
    if (!conn || conn->fd < 0 || !got || cap == 0 || !buf)
        return CN_TLS_INVALID;
    *got = 0;
    if (closed)
        *closed = 0;
    for (;;) {
        unsigned st = br_ssl_engine_current_state(&conn->cc.eng);

        if (st & BR_SSL_CLOSED) {
            cn_tls_result r = classify_engine(conn);

            if (r == CN_TLS_CLOSED && closed)
                *closed = 1;
            return r;
        }
        if (st & BR_SSL_RECVAPP) {
            size_t available;
            unsigned char *src = br_ssl_engine_recvapp_buf(&conn->cc.eng,
                                                            &available);
            size_t chunk = cap < available ? cap : available;

            if (chunk == 0)
                return CN_TLS_INTERNAL;
            memcpy(buf, src, chunk);
            br_ssl_engine_recvapp_ack(&conn->cc.eng, chunk);
            *got = chunk;
            return CN_TLS_OK;
        }
        if (st & BR_SSL_SENDREC) {
            cn_tls_result r = io_flush_out(conn, deadline_ms);

            if (r != CN_TLS_OK)
                return r;
            continue;
        }
        if (st & BR_SSL_RECVREC) {
            cn_tls_result r = io_ingest_in(conn, deadline_ms);

            if (r != CN_TLS_OK)
                return r;
            continue;
        }
        return CN_TLS_INTERNAL;
    }
}

int cn_tls_conn_closed(cn_tls_conn *conn)
{
    return !conn || conn->fd < 0 ||
           (br_ssl_engine_current_state(&conn->cc.eng) & BR_SSL_CLOSED) != 0;
}

void cn_tls_close(cn_tls_conn *conn)
{
    if (!conn || conn->fd < 0)
        return;
    if (conn->phase == 1) {
        int64_t deadline = tls_now_ms() + 200;

        br_ssl_engine_close(&conn->cc.eng);
        (void)io_flush_out(conn, deadline);
    }
    close(conn->fd);
    conn->fd = -1;
}

const char *cn_tls_result_name(cn_tls_result result)
{
    static const char *const names[] = {
        "tls-ok", "tls-invalid", "tls-internal", "tls-entropy-failed",
        "tls-invalid-ca", "tls-handshake-failed", "tls-handshake-timeout",
        "tls-trust-failed", "tls-hostname-mismatch", "tls-cert-time-failed",
        "tls-cert-invalid", "tls-protocol-failed", "tls-closed",
        "tls-recv-timeout", "tls-send-timeout"
    };

    if ((size_t)result < CN_TLS_LENGTH)
        return names[result];
    return "tls-unknown";
}

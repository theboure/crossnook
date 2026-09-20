/* CLI and self-test driver for the minimal networking client. */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>

#include "net/netsimple.h"
#include "net/tlssimple.h"

static long now_ms(void);
static void check(int condition, const char *name, int *failures);

#define TLS_TEST_CA "testca.crt"
#define TLS_TEST_SERVER_KEY "server.key.der"
#define TLS_TEST_SERVER_CERT "server.crt.der"
#define TLS_TEST_CA_DER "testca.crt.der"
#define TLS_TEST_DISTRUST_KEY "distrust.key.der"
#define TLS_TEST_DISTRUST_CERT "distrust.crt.der"
#define TLS_TEST_OTHERCA_DER "otherca.crt.der"

static const char tls_test_response[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Length: 11\r\n"
    "Connection: close\r\n"
    "\r\n"
    "hello world";

static time_t tls_fixed_now;
static time_t tls_fixed_time(void)
{
    return tls_fixed_now;
}

static int read_bounded(const char *path, unsigned char *buf, size_t cap,
                        size_t *out)
{
    FILE *f = fopen(path, "rb");
    size_t total = 0;

    if (!f)
        return 0;
    while (total < cap) {
        size_t n = fread(buf + total, 1, cap - total, f);
        total += n;
        if (n == 0) {
            int err = ferror(f);
            fclose(f);
            if (err)
                return 0;
            *out = total;
            return 1;
        }
    }
    fclose(f);
    return 0;
}

/* BearSSL supports raw PKCS#1 and unencrypted PKCS#8 DER keys directly. */
static const br_rsa_private_key *load_rsa_key(
    const char *path, br_skey_decoder_context *decoder,
    unsigned char *buf, size_t cap)
{
    size_t len;

    if (!read_bounded(path, buf, cap, &len) || len == 0)
        return NULL;
    br_skey_decoder_init(decoder);
    br_skey_decoder_push(decoder, buf, len);
    if (br_skey_decoder_last_error(decoder) != 0)
        return NULL;
    return br_skey_decoder_get_rsa(decoder);
}

static int load_cert(const char *path, br_x509_certificate *c,
                     unsigned char *buf, size_t cap)
{
    size_t len;

    if (!read_bounded(path, buf, cap, &len) || len == 0)
        return 0;
    c->data = buf;
    c->data_len = len;
    return 1;
}

/* --- mini BearSSL TLS server (one connection, canned response) --------- */

static int server_addr(uint16_t port, struct sockaddr_in *where)
{
    memset(where, 0, sizeof *where);
    where->sin_family = AF_INET;
    where->sin_port = htons(port);
    where->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return 1;
}

static int tls_server_flush(br_ssl_server_context *sc, int fd,
                            int64_t deadline)
{
    for (;;) {
        size_t len;
        unsigned char *buf;
        struct pollfd pfd;
        ssize_t w;
        int64_t remain = deadline - now_ms();

        buf = br_ssl_engine_sendrec_buf(&sc->eng, &len);
        if (len == 0)
            return 1;
        if (remain < 0)
            return 0;
        pfd.fd = fd;
        pfd.events = POLLOUT;
        pfd.revents = 0;
        if (poll(&pfd, 1, (int)remain) <= 0)
            return 0;
        w = send(fd, buf, len, MSG_NOSIGNAL);
        if (w <= 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            return 0;
        }
        br_ssl_engine_sendrec_ack(&sc->eng, (size_t)w);
    }
}

static int tls_server_ingest(br_ssl_server_context *sc, int fd,
                             int64_t deadline)
{
    for (;;) {
        size_t len;
        unsigned char *buf;
        struct pollfd pfd;
        ssize_t r;
        int64_t remain = deadline - now_ms();

        buf = br_ssl_engine_recvrec_buf(&sc->eng, &len);
        if (len == 0)
            return 1;
        if (remain < 0)
            return 0;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (poll(&pfd, 1, (int)remain) <= 0)
            return 0;
        r = recv(fd, buf, len, 0);
        if (r == 0)
            return 0;
        if (r < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            return 0;
        }
        br_ssl_engine_recvrec_ack(&sc->eng, (size_t)r);
        return 1;
    }
}

static int find_request_end(const unsigned char *req, size_t reqlen)
{
    size_t i;

    if (reqlen >= 4)
        for (i = 0; i + 3 < reqlen; i++)
            if (req[i] == '\r' && req[i + 1] == '\n' &&
                req[i + 2] == '\r' && req[i + 3] == '\n')
                return 1;
    return 0;
}

/* Serve exactly one HTTPS connection on 'sock' (an accepted socket) using
 * the referenced identity files under pki.  Returns server rc (0 = clean).
 * The identity is: chain = [leaf.der, ca.der], key = key.der. */
static int tls_server_serve_identity(int conn, const char *pki,
                                     const char *leaf, const char *ca,
                                     const char *keyname)
{
    br_ssl_server_context sc;
    br_x509_certificate chain[2];
    unsigned char leafbuf[8192];
    unsigned char cabuf[8192];
    unsigned char keybuf[8192];
    unsigned char entropy[64];
    unsigned char sbuf[BR_SSL_BUFSIZE_MONO];
    unsigned char req[2048];
    size_t reqlen = 0;
    char path[512];
    br_skey_decoder_context key_decoder;
    const br_rsa_private_key *sk;
    int fd = conn;
    int64_t deadline = now_ms() + 15000;
    unsigned st;
    int got_request = 0;
    int sent_response = 0;
    int closing = 0;
    int entlen = 0;

    memset(&sc, 0, sizeof sc);
    snprintf(path, sizeof path, "%s/%s", pki, leaf);
    if (!load_cert(path, &chain[0], leafbuf, sizeof leafbuf)) {
        fprintf(stderr, "DBG srv S1 leaf load fail\n");
        close(fd);
        return 1;
    }
    snprintf(path, sizeof path, "%s/%s", pki, ca);
    if (!load_cert(path, &chain[1], cabuf, sizeof cabuf)) {
        fprintf(stderr, "DBG srv S2 ca load fail\n");
        close(fd);
        return 1;
    }
    snprintf(path, sizeof path, "%s/%s", pki, keyname);
    sk = load_rsa_key(path, &key_decoder, keybuf, sizeof keybuf);
    if (!sk) {
        fprintf(stderr, "DBG srv S3 key load fail\n");
        close(fd);
        return 1;
    }
    {
        int efd = open("/dev/urandom", O_RDONLY);
        if (efd < 0) {
            fprintf(stderr, "DBG srv S4 urandom open fail\n");
            close(fd);
            return 1;
        }
        while (entlen < (int)sizeof entropy) {
            ssize_t r = read(efd, entropy + entlen,
                             sizeof entropy - (size_t)entlen);
            if (r < 0) {
                if (errno == EINTR)
                    continue;
                close(efd);
                fprintf(stderr, "DBG srv S5 urandom read fail\n");
                close(fd);
                return 1;
            }
            if (r == 0)
                break;
            entlen += (int)r;
        }
        close(efd);
    }
    if (entlen < 32) {
        fprintf(stderr, "DBG srv S6 no entropy\n");
        close(fd);
        return 1;
    }

    br_ssl_server_init_full_rsa(&sc, chain, 2, sk);
    br_ssl_engine_inject_entropy(&sc.eng, entropy, (size_t)entlen);
    br_ssl_engine_set_buffer(&sc.eng, sbuf, sizeof sbuf, 0);
    if (!br_ssl_server_reset(&sc)) {
        fprintf(stderr, "DBG srv S7 reset fail\n");
        close(fd);
        return 1;
    }

    for (;;) {
        st = br_ssl_engine_current_state(&sc.eng);
        if (st & BR_SSL_CLOSED)
            break;
        if (st & BR_SSL_SENDREC) {
            if (!tls_server_flush(&sc, fd, deadline)) {
                fprintf(stderr, "DBG srv S8 flush fail\n");
                close(fd);
                return 1;
            }
            continue;
        }
        if (st & BR_SSL_RECVAPP) {
            size_t len;
            unsigned char *buf = br_ssl_engine_recvapp_buf(&sc.eng, &len);
            size_t room = sizeof req - reqlen;
            size_t chunk = len < room ? len : room;

            if (chunk == 0 || room == 0) {
                fprintf(stderr, "DBG srv S10 recvapp no room\n");
                close(fd);
                return 1;
            }
            memcpy(req + reqlen, buf, chunk);
            reqlen += chunk;
            br_ssl_engine_recvapp_ack(&sc.eng, chunk);
            if (find_request_end(req, reqlen))
                got_request = 1;
            else if (reqlen >= sizeof req)
                break;
            continue;
        }
        if ((st & BR_SSL_SENDAPP) && got_request) {
            if (!sent_response) {
                size_t len;
                unsigned char *buf = br_ssl_engine_sendapp_buf(&sc.eng, &len);
                size_t chunk = sizeof tls_test_response - 1;

                if (chunk > len)
                    chunk = len;
                if (chunk == 0) {
                    fprintf(stderr, "DBG srv S11 sendapp no room\n");
                    close(fd);
                    return 1;
                }
                memcpy(buf, tls_test_response, chunk);
                br_ssl_engine_sendapp_ack(&sc.eng, chunk);
                sent_response = 1;
            } else if (got_request && sent_response && !closing) {
                br_ssl_engine_close(&sc.eng);
                closing = 1;
            } else {
                break;
            }
            continue;
        }
        if (st & BR_SSL_RECVREC) {
            if (!tls_server_ingest(&sc, fd, deadline)) {
                if (!got_request) {
                    close(fd);
                    return 0; /* client intentionally rejected the handshake */
                }
                fprintf(stderr, "DBG srv S9 ingest fail\n");
                close(fd);
                return 1;
            }
            continue;
        }
        if (st & BR_SSL_SENDAPP)
            continue;
    }

    /* best-effort flush of anything left (incl. close_notify) */
    tls_server_flush(&sc, fd, now_ms() + 2000);
    close(fd);
    fprintf(stderr, "DBG srv done st=%x err=%d ent=%d reqlen=%u got=%d sent=%d\n",
            (unsigned)st, (int)br_ssl_engine_last_error(&sc.eng), entlen,
            (unsigned)reqlen, got_request, sent_response);
    return 0;
}

static pid_t tls_server_child = -1;

/* Fork a one-shot TLS server serving the referenced identity under pki.
 * The listener is bound+listening before returning the port, so the parent
 * may connect immediately; the child accepts one connection and exits.
 * The caller must tls_server_reap() the child and check the status.
 * Returns the listening port (>0) or 0 on failure. */
static int tls_server_for_child(const char *pki, const char *leaf,
                                const char *ca, const char *keyname)
{
    struct sockaddr_in where;
    socklen_t bound_len = sizeof where;
    int listener;
    pid_t child;

    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0)
        return 0;
    memset(&where, 0, sizeof where);
    where.sin_family = AF_INET;
    where.sin_port = 0;
    where.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, (struct sockaddr *)&where, sizeof where) < 0 ||
        listen(listener, 2) < 0) {
        close(listener);
        return 0;
    }
    if (getsockname(listener, (struct sockaddr *)&where, &bound_len) < 0) {
        close(listener);
        return 0;
    }
    child = fork();
    if (child == 0) {
        int conn = accept(listener, NULL, NULL);
        int rc;

        close(listener);
        if (conn >= 0)
            rc = tls_server_serve_identity(conn, pki, leaf, ca, keyname);
        else
            rc = 1;
        _exit(rc == 0 ? 0 : 1);
    }
    close(listener);
    if (child < 0)
        return 0;
    tls_server_child = child;
    return (int)ntohs(where.sin_port);
}

static void tls_server_reap(int *failures)
{
    int status = 0;

    if (tls_server_child < 0)
        return;
    waitpid(tls_server_child, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "TLS test server exited cleanly", failures);
    tls_server_child = -1;
}

/* Connect to 127.0.0.1:port and run a full TLS exchange via the net layer. */
static cn_netsimple_result tls_net_exchange(int port,
                                            const cn_tls_config *cfg,
                                            char *buf, size_t buf_cap,
                                            cn_netsimple_response *parsed)
{
    cn_netsimple_request req;
    char port_text[CN_NETSIMPLE_PORT_MAX_TEXT + 1];

    snprintf(port_text, sizeof port_text, "%u",
             (unsigned)(uint16_t)port);
    memset(&req, 0, sizeof req);
    req.method = CN_NETSIMPLE_METHOD_GET;
    req.host = "secure.test.local";
    req.connect_host = "127.0.0.1";
    req.port = port_text;
    req.path = "/";
    req.tls = cfg;
    return cn_netsimple_exchange(&req, buf, buf_cap,
                                 CN_NETSIMPLE_DEFAULT_CONNECT_MS,
                                 CN_NETSIMPLE_DEFAULT_RECV_MS, parsed);
}

/* Connect to loopback and drive a direct cn_tls_open session (no DNS).
 * On success the response (full exchange) has been received into buf and
 * buf_len is the number of bytes captured; returns CN_TLS_OK (a clean
 * peer close after data is reported as success). */
static cn_tls_result tls_direct_exchange(int port, const cn_tls_config *cfg,
                                         const char *server_name,
                                         char *buf, size_t buf_cap,
                                         size_t *buf_len)
{
    struct sockaddr_in where;
    cn_tls_conn conn;
    cn_tls_result r;
    int fd;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return CN_TLS_INTERNAL;
    server_addr((uint16_t)port, &where);
    if (connect(fd, (struct sockaddr *)&where, sizeof where) < 0) {
        close(fd);
        return CN_TLS_INTERNAL;
    }
    r = cn_tls_open(&conn, fd, cfg, server_name,
                    now_ms() + CN_NETSIMPLE_DEFAULT_RECV_MS);
    if (r != CN_TLS_OK)
        return r;
    {
        const char request[] = "GET / HTTP/1.0\r\nHost: t\r\n\r\n";
        int64_t deadline = now_ms() + CN_NETSIMPLE_DEFAULT_RECV_MS;
        size_t total = 0;

        r = cn_tls_send_all(&conn, request, strlen(request), deadline);
        if (r == CN_TLS_OK) {
            for (;;) {
                size_t got = 0;
                int closed = 0;
                cn_tls_result rv;

                if (total >= buf_cap) {
                    r = CN_TLS_PROTOCOL_FAILED;
                    break;
                }
                rv = cn_tls_recv_some(&conn, buf + total, buf_cap - total,
                                      deadline, &got, &closed);
                total += got;
                if (rv != CN_TLS_OK) {
                    if (rv == CN_TLS_CLOSED && total != 0)
                        break; /* response fully received, peer closed */
                    r = rv;
                    break;
                }
                if (closed || got == 0)
                    break;
            }
            if (r == CN_TLS_OK || r == CN_TLS_CLOSED) {
                if (total == 0)
                    r = CN_TLS_PROTOCOL_FAILED;
                else
                    r = CN_TLS_OK;
            }
        }
        if (buf_len)
            *buf_len = total;
        if (total < buf_cap)
            buf[total] = '\0';
    }
    cn_tls_close(&conn);
    return r;
}

static cn_tls_config tls_cfg_ca(const char *pki, int fail_entropy,
                                int fixed_time)
{
    cn_tls_config cfg;

    memset(&cfg, 0, sizeof cfg);
    {
        static char cpath[512];
        static char epath[512];

        snprintf(cpath, sizeof cpath, "%s/%s", pki, TLS_TEST_CA);
        cfg.ca_path = cpath;
        if (fail_entropy) {
            snprintf(epath, sizeof epath, "%s/%s", pki,
                     "no-such-entropy-source");
            cfg.entropy_path = epath;
        }
        if (fixed_time)
            cfg.get_time = tls_fixed_time;
    }
    return cfg;
}

static cn_tls_config tls_cfg_bad_ca(const char *pki)
{
    static char cpath[512];

    snprintf(cpath, sizeof cpath, "%s/%s", pki, "no-such-ca.pem");
    return (cn_tls_config){ cpath, NULL, NULL };
}

static cn_tls_config tls_cfg_garbage_ca(const char *pki)
{
    cn_tls_config cfg;
    static char cpath[512];

    snprintf(cpath, sizeof cpath, "%s/%s", pki, "garbage-ca.pem");
    memset(&cfg, 0, sizeof cfg);
    cfg.ca_path = cpath;
    return cfg;
}

/* Spawn a fresh one-shot TLS server for a single client check. */
static int tls_spawn_ident(const char *pki, const char *leaf, const char *ca,
                           const char *key, const char *what, int *failures)
{
    int port;

    port = tls_server_for_child(pki, leaf, ca, key);
    check(port > 0, what, failures);
    if (port <= 0)
        tls_server_reap(failures);
    return port;
}

static int run_tls_self(const char *pki)
{
    int failures = 0;
    char buf[CN_NETSIMPLE_RESPONSE_MAX];
    char bufd[CN_NETSIMPLE_RESPONSE_MAX];
    size_t buflen = 0;
    cn_netsimple_response parsed;
    cn_tls_config cfg;
    cn_netsimple_result nresult;
    cn_tls_result r;
    int port;

    tls_server_child = -1;
    tls_fixed_now = 1789948800; /* 2026-09-21T00:00:00Z */
    cfg = tls_cfg_ca(pki, 0, 1);

    port = tls_spawn_ident(pki, TLS_TEST_SERVER_CERT, TLS_TEST_CA_DER,
                           TLS_TEST_SERVER_KEY,
                           "TLS self-test spawned the nominal server",
                           &failures);
    if (port) {
        nresult = tls_net_exchange(port, &cfg, buf, sizeof buf, &parsed);
        check(nresult == CN_NETSIMPLE_OK && parsed.status == 200 &&
                  parsed.body_bytes == strlen("hello world") &&
                  strncmp(buf + parsed.header_bytes, "hello world",
                          strlen("hello world")) == 0,
              "HTTPS GET routes by IP and verifies its DNS identity",
              &failures);
        tls_server_reap(&failures);
    }

    port = tls_spawn_ident(pki, TLS_TEST_SERVER_CERT, TLS_TEST_CA_DER,
                           TLS_TEST_SERVER_KEY,
                           "TLS self-test spawned the SNI server",
                           &failures);
    if (port) {
        r = tls_direct_exchange(port, &cfg, "secure.test.local",
                                bufd, sizeof bufd, &buflen);
        check(r == CN_TLS_OK && buflen != 0 && strstr(bufd, "200 OK") != NULL,
              "TLS direct open with matching dNSName (SNI path) succeeds",
              &failures);
        tls_server_reap(&failures);
    }

    port = tls_spawn_ident(pki, TLS_TEST_SERVER_CERT, TLS_TEST_CA_DER,
                           TLS_TEST_SERVER_KEY,
                           "TLS self-test spawned the localhost server",
                           &failures);
    if (port) {
        r = tls_direct_exchange(port, &cfg, "localhost",
                                bufd, sizeof bufd, &buflen);
        check(r == CN_TLS_OK && buflen != 0 && strstr(bufd, "200 OK") != NULL,
              "TLS direct open with second dNSName succeeds", &failures);
        tls_server_reap(&failures);
    }

    port = tls_spawn_ident(pki, TLS_TEST_SERVER_CERT, TLS_TEST_CA_DER,
                           TLS_TEST_SERVER_KEY,
                           "TLS self-test spawned the mismatch server",
                           &failures);
    if (port) {
        r = tls_direct_exchange(port, &cfg, "wrong.test",
                                bufd, sizeof bufd, NULL);
        check(r == CN_TLS_HOSTNAME_MISMATCH,
              "TLS direct open with mismatched dNSName is rejected",
              &failures);
        tls_server_reap(&failures);
    }

    printf("NETTEST TLS SELF failures=%d -> %s\n", failures,
           failures == 0 ? "OK" : "FAIL");
    return failures == 0 ? 0 : 1;
}

static int run_tls_distrust(const char *pki)
{
    int failures = 0;
    char buf[CN_NETSIMPLE_RESPONSE_MAX];
    char bufd[CN_NETSIMPLE_RESPONSE_MAX];
    size_t buflen = 0;
    cn_netsimple_response parsed;
    cn_tls_config cfg;
    cn_netsimple_result nresult;
    cn_tls_result r;
    int port;

    tls_server_child = -1;
    tls_fixed_now = 1789948800;
    cfg = tls_cfg_ca(pki, 0, 1);

    port = tls_spawn_ident(pki, TLS_TEST_DISTRUST_CERT, TLS_TEST_OTHERCA_DER,
                           TLS_TEST_DISTRUST_KEY,
                           "TLS distrust test spawned the untrusted server",
                           &failures);
    if (port) {
        nresult = tls_net_exchange(port, &cfg, buf, sizeof buf, &parsed);
        check(nresult == CN_NETSIMPLE_TLS_TRUST_FAILED,
              "HTTPS from an untrusted CA is rejected (trust-failed)",
              &failures);
        tls_server_reap(&failures);
    }

    port = tls_spawn_ident(pki, TLS_TEST_DISTRUST_CERT, TLS_TEST_OTHERCA_DER,
                           TLS_TEST_DISTRUST_KEY,
                           "TLS direct distrust server spawned",
                           &failures);
    if (port) {
        r = tls_direct_exchange(port, &cfg, "secure.test.local",
                                bufd, sizeof bufd, &buflen);
        check(r == CN_TLS_TRUST_FAILED,
              "TLS direct open from an untrusted CA is rejected", &failures);
        tls_server_reap(&failures);
    }

    printf("NETTEST TLS DISTRUST failures=%d -> %s\n", failures,
           failures == 0 ? "OK" : "FAIL");
    return failures == 0 ? 0 : 1;
}

static void run_tls_time_case(const char *pki, const char *leaf, int *failures)
{
    char buf[CN_NETSIMPLE_RESPONSE_MAX];
    char bufd[CN_NETSIMPLE_RESPONSE_MAX];
    cn_netsimple_response parsed;
    cn_tls_config cfg = tls_cfg_ca(pki, 0, 1);
    cn_netsimple_result nresult;
    cn_tls_result r;
    int port;

    tls_fixed_now = 1748736000; /* 2025-06-01T00:00:00Z */

    port = tls_spawn_ident(pki, leaf, TLS_TEST_CA_DER,
                           TLS_TEST_SERVER_KEY,
                           "TLS time test spawned its server", failures);
    if (port) {
        nresult = tls_net_exchange(port, &cfg, buf, sizeof buf, &parsed);
        check(nresult == CN_NETSIMPLE_TLS_CERT_TIME_FAILED,
              "HTTPS leaf outside validity window -> cert-time-failed",
              failures);
        tls_server_reap(failures);
    }

    port = tls_spawn_ident(pki, leaf, TLS_TEST_CA_DER,
                           TLS_TEST_SERVER_KEY,
                           "TLS direct time test server spawned", failures);
    if (port) {
        r = tls_direct_exchange(port, &cfg, "secure.test.local",
                                bufd, sizeof bufd, NULL);
        check(r == CN_TLS_CERT_TIME_FAILED,
              "TLS direct open with bad leaf validity rejected", failures);
        tls_server_reap(failures);
    }
}

static int run_tls_time(const char *pki)
{
    int failures = 0;

    tls_server_child = -1;
    run_tls_time_case(pki, "expired.crt.der", &failures);
    run_tls_time_case(pki, "notyet.crt.der", &failures);
    printf("NETTEST TLS TIME failures=%d -> %s\n", failures,
           failures == 0 ? "OK" : "FAIL");
    return failures == 0 ? 0 : 1;
}

static int run_tls_infra(const char *pki)
{
    int failures = 0;
    int port;
    char buf[CN_NETSIMPLE_RESPONSE_MAX];
    cn_netsimple_response parsed;
    cn_netsimple_result nresult;

    tls_fixed_now = 1789948800;

    check(strcmp(cn_netsimple_result_name(CN_NETSIMPLE_TLS_TRUST_FAILED),
                 "tls-trust-failed") == 0 &&
              strcmp(cn_netsimple_result_name(CN_NETSIMPLE_TLS_INVALID_CA),
                     "tls-invalid-ca") == 0 &&
              strcmp(cn_netsimple_result_name(
                         CN_NETSIMPLE_TLS_HOSTNAME_MISMATCH),
                     "tls-hostname-mismatch") == 0,
          "New netsimple TLS result names are stable", &failures);
    check(strcmp(cn_tls_result_name(CN_TLS_ENTROPY_FAILED),
                 "tls-entropy-failed") == 0 &&
              strcmp(cn_tls_result_name(CN_TLS_HOSTNAME_MISMATCH),
                     "tls-hostname-mismatch") == 0 &&
              strcmp(cn_tls_result_name(CN_TLS_LENGTH), "tls-unknown") == 0,
          "cn_tls_result names are stable and bounded", &failures);

    tls_server_child = -1;
    port = tls_spawn_ident(pki, TLS_TEST_SERVER_CERT, TLS_TEST_CA_DER,
                           TLS_TEST_SERVER_KEY,
                           "TLS infra test spawned the nominal server",
                           &failures);
    if (port) {
        cn_tls_config bad = tls_cfg_bad_ca(pki);

        nresult = tls_net_exchange(port, &bad, buf, sizeof buf, &parsed);
        check(nresult == CN_NETSIMPLE_TLS_INVALID_CA,
              "HTTPS with a missing CA file is rejected (invalid-ca)",
              &failures);
        tls_server_reap(&failures);
    }

    port = tls_spawn_ident(pki, TLS_TEST_SERVER_CERT, TLS_TEST_CA_DER,
                           TLS_TEST_SERVER_KEY,
                           "TLS infra garbage-CA server spawned", &failures);
    if (port) {
        cn_tls_config garbage = tls_cfg_garbage_ca(pki);

        nresult = tls_net_exchange(port, &garbage, buf, sizeof buf, &parsed);
        check(nresult == CN_NETSIMPLE_TLS_INVALID_CA,
              "HTTPS with a garbage CA file is rejected (invalid-ca)",
              &failures);
        tls_server_reap(&failures);
    }

    port = tls_spawn_ident(pki, TLS_TEST_SERVER_CERT, TLS_TEST_CA_DER,
                           TLS_TEST_SERVER_KEY,
                           "TLS infra entropy server spawned", &failures);
    if (port) {
        cn_tls_config noent = tls_cfg_ca(pki, 1, 1);

        nresult = tls_net_exchange(port, &noent, buf, sizeof buf, &parsed);
        check(nresult == CN_NETSIMPLE_TLS_ENTROPY_FAILED,
              "HTTPS with a missing entropy source is rejected cleanly",
              &failures);
        tls_server_reap(&failures);
    }

    /* Plain-HTTP must not be silently upgraded for a TLS server. */
    port = tls_spawn_ident(pki, TLS_TEST_SERVER_CERT, TLS_TEST_CA_DER,
                           TLS_TEST_SERVER_KEY,
                           "TLS infra plain-HTTP server spawned", &failures);
    if (port) {
        char port_text[CN_NETSIMPLE_PORT_MAX_TEXT + 1];

        snprintf(port_text, sizeof port_text, "%u",
                 (unsigned)(uint16_t)port);
        nresult = cn_netsimple_get("127.0.0.1", port_text, "/", buf,
                                   sizeof buf, CN_NETSIMPLE_DEFAULT_CONNECT_MS,
                                   CN_NETSIMPLE_DEFAULT_RECV_MS, &parsed);
        check(nresult == CN_NETSIMPLE_BAD_RESPONSE ||
                  nresult == CN_NETSIMPLE_RECV_ERROR ||
                  nresult == CN_NETSIMPLE_SEND_ERROR,
              "Plain-HTTP client is not confused by a TLS server", &failures);
        tls_server_reap(&failures);
    }

    printf("NETTEST TLS INFRA failures=%d -> %s\n", failures,
           failures == 0 ? "OK" : "FAIL");
    return failures == 0 ? 0 : 1;
}

static void check(int condition, const char *name, int *failures)
{
    printf("[%s] %s\n", condition ? "OK" : "FAIL", name);
    if (!condition)
        (*failures)++;
}

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static int run_api_smoke(void)
{
    unsigned char octets[4];
    int port;
    char request[256];
    size_t request_len;
    char buffer[1024];
    cn_netsimple_response parsed;
    int failures = 0;

    check(cn_netsimple_parse_ipv4("192.168.1.1", octets) == 1 &&
              octets[0] == 192 && octets[1] == 168 &&
              octets[2] == 1 && octets[3] == 1,
          "IPv4 literal parses with correct octets", &failures);
    check(cn_netsimple_parse_ipv4("999.1.1.1", NULL) == 0,
          "IPv4 rejects an out-of-range octet", &failures);
    check(cn_netsimple_parse_ipv4("1.2.3", NULL) == 0,
          "IPv4 rejects too few octets", &failures);
    check(cn_netsimple_parse_ipv4("1.2.3.4.5", NULL) == 0,
          "IPv4 rejects too many octets", &failures);
    check(cn_netsimple_parse_ipv4("1..3.4", NULL) == 0,
          "IPv4 rejects an empty octet", &failures);

    check(cn_netsimple_parse_port("80", &port) == 1 && port == 80,
          "Port accepts a valid value", &failures);
    check(cn_netsimple_parse_port("0", NULL) == 0,
          "Port rejects zero", &failures);
    check(cn_netsimple_parse_port("65536", NULL) == 0,
          "Port rejects an out-of-range value", &failures);
    check(cn_netsimple_parse_port("abc", NULL) == 0,
          "Port rejects non-digits", &failures);
    check(cn_netsimple_parse_port("", NULL) == 0,
          "Port rejects an empty string", &failures);

    check(cn_netsimple_validate("192.168.1.1", "80", "/") == 1,
          "Validate accepts a literal address", &failures);
    check(cn_netsimple_validate("fileserver.local", "80", "/") == 1 &&
              cn_netsimple_validate("host-1.base", "8080", "/d/") == 1,
          "Validate accepts hostnames", &failures);
    check(cn_netsimple_validate("bad host", "80", "/") == 0,
          "Validate rejects whitespace in a host", &failures);
    check(cn_netsimple_validate("bad\r\nhost", "80", "/") == 0,
          "Validate rejects controls in a host", &failures);
    check(cn_netsimple_validate("evil/host", "80", "/") == 0,
          "Validate rejects a slash in a host", &failures);
    check(cn_netsimple_validate("", "80", "/") == 0,
          "Validate rejects an empty host", &failures);
    check(cn_netsimple_validate("192.168.1.1", "0", "/") == 0,
          "Validate rejects an invalid port", &failures);
    check(cn_netsimple_validate("192.168.1.1", "80", "nope") == 0,
          "Validate requires an absolute path", &failures);
    check(cn_netsimple_validate("192.168.1.1", "80", "@") == 0,
          "Validate rejects a non-slash path", &failures);
    check(cn_netsimple_validate("192.168.1.1", "80",
                                "/ok\r\nInjected: yes") == 0,
          "Validate rejects request-target header injection", &failures);

    check(cn_netsimple_build_request("192.168.1.1", "81", "/d/",
                                     request, sizeof request,
                                     &request_len) == CN_NETSIMPLE_OK &&
              strcmp(request,
                     "GET /d/ HTTP/1.0\r\nHost: 192.168.1.1\r\n\r\n") == 0,
          "Request builder emits a bounded HTTP/1.0 GET", &failures);
    check(cn_netsimple_build_request("h", "80", "/", request, 8,
                                     &request_len) == CN_NETSIMPLE_INVALID,
          "Request builder rejects an undersized buffer", &failures);

    strcpy(buffer,
           "HTTP/1.1 200 OK\r\n"
           "Content-Length: 2\r\n"
           "\r\n"
           "hi");
    check(cn_netsimple_parse_status(buffer, strlen(buffer), &parsed) ==
              CN_NETSIMPLE_OK &&
              parsed.status == 200 && parsed.header_bytes == 38 &&
              parsed.body_bytes == 2 &&
              parsed.status_text_bytes == 2 &&
              strncmp(parsed.status_text, "OK", 2) == 0,
          "Status parser splits a canned 200 response", &failures);
    memset(&parsed, 0, sizeof parsed);
    strcpy(buffer, "HTTP/1.0 404 Not Found\r\n\r\n");
    check(cn_netsimple_parse_status(buffer, strlen(buffer), &parsed) ==
              CN_NETSIMPLE_OK && parsed.status == 404 &&
              strncmp(parsed.status_text, "Not Found", 9) == 0,
          "Status parser handles a 404", &failures);
    memset(&parsed, 0, sizeof parsed);
    strcpy(buffer, "this is not http");
    check(cn_netsimple_parse_status(buffer, strlen(buffer), &parsed) ==
              CN_NETSIMPLE_BAD_RESPONSE,
          "Status parser rejects garbage", &failures);
    check(strcmp(cn_netsimple_result_name(CN_NETSIMPLE_OK), "ok") == 0 &&
              strcmp(cn_netsimple_result_name(CN_NETSIMPLE_CONNECT_TIMEOUT),
                     "connect-timeout") == 0 &&
              strcmp(cn_netsimple_result_name(CN_NETSIMPLE_NETWORK_UNREACHABLE),
                     "network-unreachable") == 0 &&
              strcmp(cn_netsimple_result_name(CN_NETSIMPLE_HOST_UNREACHABLE),
                     "host-unreachable") == 0,
          "Result names are stable", &failures);

    check(cn_netsimple_connect_error(ECONNREFUSED) ==
              CN_NETSIMPLE_CONNECT_REFUSED,
          "ECONNREFUSED maps to connect-refused", &failures);
    check(cn_netsimple_connect_error(ENETUNREACH) ==
              CN_NETSIMPLE_NETWORK_UNREACHABLE,
          "ENETUNREACH maps to network-unreachable", &failures);
    check(cn_netsimple_connect_error(EHOSTUNREACH) ==
              CN_NETSIMPLE_HOST_UNREACHABLE,
          "EHOSTUNREACH maps to host-unreachable", &failures);
    check(cn_netsimple_connect_error(ETIMEDOUT) ==
              CN_NETSIMPLE_CONNECT_TIMEOUT &&
              cn_netsimple_connect_error(EINPROGRESS) ==
                  CN_NETSIMPLE_CONNECT_TIMEOUT,
          "ETIMEDOUT/EINPROGRESS map to connect-timeout", &failures);
    check(cn_netsimple_connect_error(EACCES) == CN_NETSIMPLE_CONNECT_ERROR &&
              cn_netsimple_connect_error(EADDRNOTAVAIL) ==
                  CN_NETSIMPLE_CONNECT_ERROR &&
              cn_netsimple_connect_error(EPERM) == CN_NETSIMPLE_CONNECT_ERROR &&
              cn_netsimple_connect_error(ENOBUFS) ==
                  CN_NETSIMPLE_CONNECT_ERROR,
          "other errno values map to connect-error, not connect-refused",
          &failures);

    printf("NETTEST API SMOKE failures=%d -> %s\n", failures,
           failures == 0 ? "OK" : "FAIL");
    return failures == 0 ? 0 : 1;
}

static const char self_test_server[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Length: 11\r\n"
    "Connection: close\r\n"
    "\r\n"
    "hello world";

static int run_self_test(void)
{
    struct sockaddr_in where;
    struct sockaddr_in bound;
    socklen_t bound_len = sizeof bound;
    char port_text[CN_NETSIMPLE_PORT_MAX_TEXT + 1];
    char buffer[CN_NETSIMPLE_RESPONSE_MAX];
    cn_netsimple_response parsed;
    int listener;
    int failures = 0;
    pid_t child;
    int status;

    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        fprintf(stderr, "[FAIL] self-test cannot open a listener\n");
        return 1;
    }
    memset(&where, 0, sizeof where);
    where.sin_family = AF_INET;
    where.sin_port = 0;
    where.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, (struct sockaddr *)&where, sizeof where) < 0 ||
        listen(listener, 2) < 0) {
        perror("listener");
        return 1;
    }
    if (getsockname(listener, (struct sockaddr *)&bound, &bound_len) < 0) {
        perror("getsockname");
        return 1;
    }
    snprintf(port_text, sizeof port_text, "%u",
             (unsigned)(uint16_t)ntohs(bound.sin_port));

    child = fork();
    if (child == 0) {
        int conn = accept(listener, NULL, NULL);
        if (conn >= 0) {
            (void)write(conn, self_test_server,
                        (size_t)strlen(self_test_server));
            close(conn);
        }
        _exit(0);
    }
    close(listener);

    check(cn_netsimple_get("127.0.0.1", port_text, "/",
                           buffer, sizeof buffer,
                           CN_NETSIMPLE_DEFAULT_CONNECT_MS,
                           CN_NETSIMPLE_DEFAULT_RECV_MS,
                           &parsed) == CN_NETSIMPLE_OK &&
              parsed.status == 200 &&
              parsed.body_bytes == strlen("hello world") &&
              strncmp(buffer + parsed.header_bytes, "hello world",
                      strlen("hello world")) == 0,
          "Self-test GETs a canned 200 response over loopback", &failures);
    waitpid(child, &status, 0);
    printf("NETTEST SELF TEST failures=%d -> %s\n", failures,
           failures == 0 ? "OK" : "FAIL");
    return failures == 0 ? 0 : 1;
}

static int run_self_refused(void)
{
    char buffer[CN_NETSIMPLE_RESPONSE_MAX];
    cn_netsimple_response parsed;
    cn_netsimple_result result;
    int failures = 0;

    result = cn_netsimple_get("127.0.0.2", "1", "/",
                              buffer, sizeof buffer,
                              CN_NETSIMPLE_DEFAULT_CONNECT_MS,
                              CN_NETSIMPLE_DEFAULT_RECV_MS, &parsed);
    check(result == CN_NETSIMPLE_CONNECT_REFUSED ||
              result == CN_NETSIMPLE_CONNECT_ERROR,
          "Self-refused connect fails cleanly", &failures);
    printf("NETTEST SELF REFUSED failures=%d -> %s\n", failures,
           failures == 0 ? "OK" : "FAIL");
    return failures == 0 ? 0 : 1;
}

static int run_self_timeout(void)
{
    char buffer[CN_NETSIMPLE_RESPONSE_MAX];
    cn_netsimple_response parsed;
    cn_netsimple_result result;
    long started = now_ms();
    long elapsed;
    int failures = 0;

    result = cn_netsimple_get("192.0.2.1", "80", "/",
                              buffer, sizeof buffer, 600, 1000, &parsed);
    elapsed = now_ms() - started;
    check(result != CN_NETSIMPLE_OK,
          "Self-timeout connect reports a failure", &failures);
    check(elapsed < 6000,
          "Self-timeout returns well inside six seconds", &failures);
    printf("NETTEST SELF TIMEOUT failures=%d result=%s elapsed=%ldms\n",
           failures, cn_netsimple_result_name(result), elapsed);
    return failures == 0 ? 0 : 1;
}

static int run_live(const char *host, const char *port, const char *path)
{
    char buffer[CN_NETSIMPLE_RESPONSE_MAX];
    cn_netsimple_response parsed;
    cn_netsimple_result result;
    char reason[17];

    result = cn_netsimple_get(host, port, path,
                              buffer, sizeof buffer,
                              CN_NETSIMPLE_DEFAULT_CONNECT_MS,
                              CN_NETSIMPLE_DEFAULT_RECV_MS, &parsed);
    if (result != CN_NETSIMPLE_OK) {
        fprintf(stderr, "NESTEST GET FAIL %s %s %s %s\n", host, port,
                path, cn_netsimple_result_name(result));
        return 1;
    }
    memset(reason, 0, sizeof reason);
    if (parsed.status_text) {
        size_t n = parsed.status_text_bytes;
        if (n > 16)
            n = 16;
        memcpy(reason, parsed.status_text, n);
    } else {
        strcpy(reason, "-");
    }
    printf("NESTEST GET %d %llu %llu %llu %s\n", parsed.status,
           (unsigned long long)parsed.header_bytes,
           (unsigned long long)parsed.body_bytes,
           (unsigned long long)parsed.total_bytes, reason);
    return 0;
}

static int parse_positive_long(const char *text, long *value)
{
    char *end;
    long parsed;

    errno = 0;
    parsed = strtol(text, &end, 10);
    if (errno || !text[0] || *end || parsed <= 0)
        return 0;
    *value = parsed;
    return 1;
}

static int run_https_live(int argc, char **argv, int put)
{
    cn_tls_config tls;
    cn_netsimple_request request;
    cn_netsimple_response response;
    cn_netsimple_result result;
    char buffer[CN_NETSIMPLE_RESPONSE_MAX];
    long timeout = CN_NETSIMPLE_DEFAULT_RECV_MS;
    int minimum = put ? 8 : 7;

    if (argc < minimum || argc > minimum + 2)
        return 2;
    memset(&tls, 0, sizeof tls);
    tls.ca_path = argv[5];
    if (argc >= minimum + 1) {
        long epoch;

        if (!parse_positive_long(argv[minimum], &epoch))
            return 2;
        tls_fixed_now = (time_t)epoch;
        tls.get_time = tls_fixed_time;
    }
    if (argc == minimum + 2 &&
        !parse_positive_long(argv[minimum + 1], &timeout))
        return 2;

    memset(&request, 0, sizeof request);
    request.method = put ? CN_NETSIMPLE_METHOD_PUT : CN_NETSIMPLE_METHOD_GET;
    request.connect_host = argv[2];
    request.port = argv[3];
    request.host = argv[4];
    request.path = argv[6];
    request.tls = &tls;
    if (put) {
        request.content_type = "text/plain";
        request.body = argv[7];
        request.body_len = strlen(argv[7]);
    }
    result = cn_netsimple_exchange(&request, buffer, sizeof buffer,
                                   (unsigned)timeout, (unsigned)timeout,
                                   &response);
    if (result != CN_NETSIMPLE_OK) {
        fprintf(stderr, "NETTEST HTTPS %s FAIL %s\n", put ? "PUT" : "GET",
                cn_netsimple_result_name(result));
        return 1;
    }
    printf("NETTEST HTTPS %s %d %llu %llu OK\n", put ? "PUT" : "GET",
           response.status, (unsigned long long)response.body_bytes,
           (unsigned long long)response.total_bytes);
    return response.status == 200 ? 0 : 1;
}

static int run_tls_write_timeout(int argc, char **argv)
{
    struct addrinfo hints;
    struct addrinfo *addresses = NULL;
    struct addrinfo *address;
    cn_tls_config tls;
    cn_tls_conn conn;
    cn_tls_result result = CN_TLS_INTERNAL;
    unsigned char chunk[4096];
    long epoch;
    long timeout;
    int fd = -1;
    int flags;
    int i;

    if (argc != 8 || !parse_positive_long(argv[6], &epoch) ||
        !parse_positive_long(argv[7], &timeout))
        return 2;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(argv[2], argv[3], &hints, &addresses) != 0)
        return 1;
    for (address = addresses; address; address = address->ai_next) {
        fd = socket(address->ai_family, address->ai_socktype,
                    address->ai_protocol);
        if (fd >= 0 && connect(fd, address->ai_addr, address->ai_addrlen) == 0)
            break;
        if (fd >= 0)
            close(fd);
        fd = -1;
    }
    freeaddrinfo(addresses);
    if (fd < 0)
        return 1;
    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(fd);
        return 1;
    }
    memset(&tls, 0, sizeof tls);
    tls.ca_path = argv[5];
    tls_fixed_now = (time_t)epoch;
    tls.get_time = tls_fixed_time;
    result = cn_tls_open(&conn, fd, &tls, argv[4], now_ms() + 5000);
    if (result != CN_TLS_OK) {
        fprintf(stderr, "NETTEST TLS WRITE setup=%s\n",
                cn_tls_result_name(result));
        return 1;
    }
    {
        static const char header[] =
            "PUT /https/write-stall HTTP/1.0\r\n"
            "Host: secure.test.local\r\n"
            "Content-Type: application/octet-stream\r\n"
            "Content-Length: 268435456\r\n\r\n";

        result = cn_tls_send_all(&conn, header, sizeof header - 1,
                                 now_ms() + 5000);
    }
    memset(chunk, 'w', sizeof chunk);
    for (i = 0; i < 65536 && result == CN_TLS_OK; i++)
        result = cn_tls_send_all(&conn, chunk, sizeof chunk,
                                 now_ms() + timeout);
    cn_tls_close(&conn);
    printf("NETTEST TLS WRITE result=%s chunks=%d -> %s\n",
           cn_tls_result_name(result), i,
           result == CN_TLS_SEND_TIMEOUT ? "OK" : "FAIL");
    return result == CN_TLS_SEND_TIMEOUT ? 0 : 1;
}

static void print_usage(void)
{
    fprintf(stderr,
            "usage: crossnook-net-test <host> <port> <path>\n"
            "       crossnook-net-test --api-smoke\n"
            "       crossnook-net-test --self-test\n"
            "       crossnook-net-test --self-refused\n"
            "       crossnook-net-test --self-timeout\n"
            "       crossnook-net-test --tls-self <pki-dir>\n"
            "       crossnook-net-test --tls-distrust <pki-dir>\n"
            "       crossnook-net-test --tls-time <pki-dir>\n"
            "       crossnook-net-test --tls-infra <pki-dir>\n"
            "       crossnook-net-test --https-get <connect-host> <port> <server-name> <ca> <path> [epoch] [timeout-ms]\n"
            "       crossnook-net-test --https-put <connect-host> <port> <server-name> <ca> <path> <body> [epoch] [timeout-ms]\n"
            "       crossnook-net-test --tls-write-timeout <connect-host> <port> <server-name> <ca> <epoch> <timeout-ms>\n"
            "       crossnook-net-test --help\n");
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--api-smoke") == 0)
        return run_api_smoke();
    if (argc == 2 && strcmp(argv[1], "--self-test") == 0)
        return run_self_test();
    if (argc == 2 && strcmp(argv[1], "--self-refused") == 0)
        return run_self_refused();
    if (argc == 2 && strcmp(argv[1], "--self-timeout") == 0)
        return run_self_timeout();
    if (argc == 3 && strcmp(argv[1], "--tls-self") == 0)
        return run_tls_self(argv[2]);
    if (argc == 3 && strcmp(argv[1], "--tls-distrust") == 0)
        return run_tls_distrust(argv[2]);
    if (argc == 3 && strcmp(argv[1], "--tls-time") == 0)
        return run_tls_time(argv[2]);
    if (argc == 3 && strcmp(argv[1], "--tls-infra") == 0)
        return run_tls_infra(argv[2]);
    if (argc > 1 && strcmp(argv[1], "--https-get") == 0)
        return run_https_live(argc, argv, 0);
    if (argc > 1 && strcmp(argv[1], "--https-put") == 0)
        return run_https_live(argc, argv, 1);
    if (argc > 1 && strcmp(argv[1], "--tls-write-timeout") == 0)
        return run_tls_write_timeout(argc, argv);
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        print_usage();
        return 0;
    }
    if (argc == 4)
        return run_live(argv[1], argv[2], argv[3]);
    print_usage();
    return 2;
}

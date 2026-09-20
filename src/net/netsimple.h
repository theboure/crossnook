/* Minimal bounded HTTP/1.0 client over plain TCP or verified TLS. */
#ifndef CN_NET_NETSIMPLE_H
#define CN_NET_NETSIMPLE_H

#include <stddef.h>

#define CN_NETSIMPLE_HOST_MAX 255
#define CN_NETSIMPLE_PORT_MAX_TEXT 8
#define CN_NETSIMPLE_PATH_MAX 2048
#define CN_NETSIMPLE_REQUEST_MAX 4096
#define CN_NETSIMPLE_RESPONSE_MAX 65536
#define CN_NETSIMPLE_HEADER_MAX 8
#define CN_NETSIMPLE_DEFAULT_CONNECT_MS 20000
#define CN_NETSIMPLE_DEFAULT_RECV_MS 10000

typedef enum cn_netsimple_method {
    CN_NETSIMPLE_METHOD_GET = 0,
    CN_NETSIMPLE_METHOD_PUT
} cn_netsimple_method;

struct cn_tls_config;
struct cn_tls_conn;

typedef struct cn_netsimple_header {
    const char *name;
    const char *value;
} cn_netsimple_header;

typedef struct cn_netsimple_request {
    cn_netsimple_method method;
    const char *host;         /* HTTP Host and TLS identity */
    const char *connect_host; /* optional DNS/numeric routing override */
    const char *port;
    const char *path;
    const cn_netsimple_header *headers;
    size_t header_count;
    const char *content_type;
    const void *body;
    size_t body_len;
    const struct cn_tls_config *tls; /* NULL => plain HTTP (unchanged) */
} cn_netsimple_request;

typedef enum cn_netsimple_result {
    CN_NETSIMPLE_OK = 0,
    CN_NETSIMPLE_INVALID,
    CN_NETSIMPLE_RESOLVE_ERROR,
    CN_NETSIMPLE_CONNECT_REFUSED,
    CN_NETSIMPLE_NETWORK_UNREACHABLE,
    CN_NETSIMPLE_HOST_UNREACHABLE,
    CN_NETSIMPLE_CONNECT_TIMEOUT,
    CN_NETSIMPLE_CONNECT_ERROR,
    CN_NETSIMPLE_SEND_ERROR,
    CN_NETSIMPLE_RECV_ERROR,
    CN_NETSIMPLE_RECV_TIMEOUT,
    CN_NETSIMPLE_TRUNCATED,
    CN_NETSIMPLE_BAD_RESPONSE,
    CN_NETSIMPLE_SEND_TIMEOUT,
    CN_NETSIMPLE_TLS_ENTROPY_FAILED,
    CN_NETSIMPLE_TLS_INVALID_CA,
    CN_NETSIMPLE_TLS_HANDSHAKE_FAILED,
    CN_NETSIMPLE_TLS_HANDSHAKE_TIMEOUT,
    CN_NETSIMPLE_TLS_TRUST_FAILED,
    CN_NETSIMPLE_TLS_HOSTNAME_MISMATCH,
    CN_NETSIMPLE_TLS_CERT_TIME_FAILED,
    CN_NETSIMPLE_TLS_CERT_INVALID,
    CN_NETSIMPLE_TLS_PROTOCOL_FAILED,
    CN_NETSIMPLE_TLS_INTERNAL,
    CN_NETSIMPLE_TLS_RECV_TIMEOUT,
    CN_NETSIMPLE_LENGTH
} cn_netsimple_result;

typedef struct cn_netsimple_response {
    int status;
    size_t header_bytes;
    size_t body_bytes;
    size_t total_bytes;
    const char *status_text;
    size_t status_text_bytes;
} cn_netsimple_response;

int cn_netsimple_parse_ipv4(const char *text, unsigned char out[4]);
int cn_netsimple_parse_port(const char *text, int *out);
int cn_netsimple_validate(const char *host, const char *port,
                          const char *path);
cn_netsimple_result cn_netsimple_build_request(const char *host,
                                               const char *port,
                                               const char *path,
                                               char *request,
                                               size_t request_cap,
                                               size_t *request_len);
cn_netsimple_result cn_netsimple_build_exchange_request(
    const cn_netsimple_request *spec,
    char *request, size_t request_cap, size_t *request_len);
cn_netsimple_result cn_netsimple_parse_status(const char *buf, size_t len,
                                              cn_netsimple_response *out);
cn_netsimple_result cn_netsimple_exchange(
    const cn_netsimple_request *spec,
    char *buffer, size_t buffer_cap,
    unsigned connect_ms, unsigned recv_ms,
    cn_netsimple_response *out);
cn_netsimple_result cn_netsimple_get(const char *host, const char *port,
                                     const char *path,
                                     char *buffer, size_t buffer_cap,
                                     unsigned connect_ms, unsigned recv_ms,
                                     cn_netsimple_response *out);
cn_netsimple_result cn_netsimple_connect_error(int error);
const char *cn_netsimple_result_name(cn_netsimple_result result);

#endif /* CN_NET_NETSIMPLE_H */

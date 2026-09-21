/* Minimal bounded DNS A-record resolver over UDP/IPv4. */
#ifndef CN_NET_DNSSIMPLE_H
#define CN_NET_DNSSIMPLE_H

#include <stddef.h>

#define CN_DNS_HOST_MAX 253
#define CN_DNS_MAX_SERVERS 2
#define CN_DNS_MAX_IPV4 4
#define CN_DNS_IPV4_TEXT_MAX 16
#define CN_DNS_DEFAULT_PORT 53
#define CN_DNS_DEFAULT_TIMEOUT_MS 2000

typedef enum cn_dns_result {
    CN_DNS_OK = 0,
    CN_DNS_INVALID,
    CN_DNS_INVALID_HOSTNAME,
    CN_DNS_ENTROPY_FAILED,
    CN_DNS_SOCKET_FAILED,
    CN_DNS_NETWORK_FAILED,
    CN_DNS_TIMEOUT,
    CN_DNS_PACKET_SIZE,
    CN_DNS_MALFORMED_RESPONSE,
    CN_DNS_TRUNCATED_RESPONSE,
    CN_DNS_NXDOMAIN,
    CN_DNS_SERVER_FAILURE,
    CN_DNS_NO_ADDRESS,
    CN_DNS_UNSUPPORTED_CNAME,
    CN_DNS_RESULT_COUNT
} cn_dns_result;

typedef struct cn_dns_config {
    const char *servers[CN_DNS_MAX_SERVERS];
    size_t server_count;
    unsigned port;       /* zero selects CN_DNS_DEFAULT_PORT */
    unsigned timeout_ms; /* zero selects CN_DNS_DEFAULT_TIMEOUT_MS */
} cn_dns_config;

typedef struct cn_dns_answer {
    char ipv4[CN_DNS_MAX_IPV4][CN_DNS_IPV4_TEXT_MAX];
    size_t count;
    size_t server_index;
} cn_dns_answer;

cn_dns_result cn_dnssimple_resolve_a(const cn_dns_config *config,
                                     const char *hostname,
                                     cn_dns_answer *answer);
const char *cn_dnssimple_result_name(cn_dns_result result);

#endif /* CN_NET_DNSSIMPLE_H */

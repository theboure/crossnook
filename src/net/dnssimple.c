#include <arpa/inet.h>
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

#if defined(CN_DNS_DIAGNOSTIC_TRACE) && CN_DNS_DIAGNOSTIC_TRACE
#include <stdarg.h>
#endif

#include "net/dnssimple.h"

#define CN_DNS_HEADER_BYTES 12
#define CN_DNS_PACKET_MAX 512
#define CN_DNS_RECV_CAP (CN_DNS_PACKET_MAX + 1)
#define CN_DNS_NAME_WIRE_MAX 255
#define CN_DNS_POINTER_HOPS_MAX 16
#define CN_DNS_RECORDS_MAX 32

static int monotonic_ns(int64_t *out)
{
    struct timespec ts;

    if (!out || clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    *out = (int64_t)ts.tv_sec * INT64_C(1000000000) + ts.tv_nsec;
    return 1;
}

#if defined(CN_DNS_DIAGNOSTIC_TRACE) && CN_DNS_DIAGNOSTIC_TRACE
static void dns_trace(const char *format, ...)
{
    int64_t now = -1;
    va_list arguments;

    (void)monotonic_ns(&now);
    fprintf(stderr, "DNS TRACE mono_ns=%lld ", (long long)now);
    va_start(arguments, format);
    vfprintf(stderr, format, arguments);
    va_end(arguments);
    fputc('\n', stderr);
    fflush(stderr);
}
#define DNS_TRACE(...) dns_trace(__VA_ARGS__)
#else
#define DNS_TRACE(...) ((void)0)
#endif

static int poll_timeout_ms(int64_t deadline_ns)
{
    int64_t now;
    int64_t remaining;
    int64_t milliseconds;

    if (!monotonic_ns(&now))
        return -2;
    remaining = deadline_ns - now;
    if (remaining <= 0)
        return -1;
    milliseconds = (remaining + INT64_C(999999)) / INT64_C(1000000);
    if (milliseconds > INT_MAX)
        return INT_MAX;
    return (int)milliseconds;
}

static int wait_for_socket(int fd, short events, int64_t deadline_ns)
{
    struct pollfd descriptor;

    descriptor.fd = fd;
    descriptor.events = events;
    descriptor.revents = 0;
    while (1) {
        int timeout = poll_timeout_ms(deadline_ns);
        int polled;

        if (timeout == -2)
            return -1;
        if (timeout < 0)
            return 0;
        polled = poll(&descriptor, 1, timeout);
        if (polled == 0)
            return 0;
        if (polled < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (descriptor.revents & events) {
            timeout = poll_timeout_ms(deadline_ns);
            if (timeout == -2)
                return -1;
            return timeout < 0 ? 0 : 1;
        }
        if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))
            return -1;
    }
}

static uint16_t load_u16(const unsigned char *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void store_u16(unsigned char *p, uint16_t value)
{
    p[0] = (unsigned char)(value >> 8);
    p[1] = (unsigned char)value;
}

static int ascii_alnum(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9');
}

static int label_ok(const unsigned char *label, size_t length)
{
    size_t i;

    if (length == 0 || length > 63 || label[0] == '-' ||
        label[length - 1] == '-')
        return 0;
    for (i = 0; i < length; i++) {
        if (!ascii_alnum(label[i]) && label[i] != '-')
            return 0;
    }
    return 1;
}

static int hostname_ok(const char *hostname)
{
    size_t length;
    size_t label_start = 0;
    size_t i;

    if (!hostname)
        return 0;
    length = strlen(hostname);
    if (length == 0 || length > CN_DNS_HOST_MAX || hostname[length - 1] == '.')
        return 0;
    for (i = 0; i <= length; i++) {
        if (hostname[i] != '.' && hostname[i] != '\0')
            continue;
        if (!label_ok((const unsigned char *)hostname + label_start,
                      i - label_start))
            return 0;
        label_start = i + 1;
    }
    return 1;
}

static int name_equal(const char *left, const char *right)
{
    size_t i;

    for (i = 0; left[i] && right[i]; i++) {
        unsigned char a = (unsigned char)left[i];
        unsigned char b = (unsigned char)right[i];

        if (a >= 'A' && a <= 'Z')
            a = (unsigned char)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z')
            b = (unsigned char)(b + ('a' - 'A'));
        if (a != b)
            return 0;
    }
    return left[i] == right[i];
}

static int read_transaction_id(uint16_t *id)
{
    unsigned char bytes[2];
    ssize_t got;
    int fd = open("/dev/urandom", O_RDONLY | O_NONBLOCK);

    if (fd < 0)
        return 0;
    got = read(fd, bytes, sizeof bytes);
    close(fd);
    if (got != (ssize_t)sizeof bytes)
        return 0;
    *id = load_u16(bytes);
    return 1;
}

static cn_dns_result build_query(const char *hostname, unsigned char *query,
                                 size_t *query_len, uint16_t *id)
{
    size_t input = 0;
    size_t output = CN_DNS_HEADER_BYTES;

    if (!read_transaction_id(id))
        return CN_DNS_ENTROPY_FAILED;
    memset(query, 0, CN_DNS_PACKET_MAX);
    store_u16(query, *id);
    store_u16(query + 2, 0x0100); /* recursion desired */
    store_u16(query + 4, 1);

    while (hostname[input]) {
        size_t label_start = input;
        size_t label_length;

        while (hostname[input] && hostname[input] != '.')
            input++;
        label_length = input - label_start;
        if (output + 1 + label_length >= CN_DNS_PACKET_MAX)
            return CN_DNS_INVALID_HOSTNAME;
        query[output++] = (unsigned char)label_length;
        memcpy(query + output, hostname + label_start, label_length);
        output += label_length;
        if (hostname[input] == '.')
            input++;
    }
    query[output++] = 0;
    store_u16(query + output, 1); /* A */
    output += 2;
    store_u16(query + output, 1); /* IN */
    output += 2;
    *query_len = output;
    return CN_DNS_OK;
}

static int decode_name(const unsigned char *packet, size_t packet_len,
                        size_t offset, char out[CN_DNS_HOST_MAX + 1],
                        int *hostname_syntax, size_t *next_offset)
{
    unsigned char seen[CN_DNS_PACKET_MAX / 8];
    size_t position = offset;
    size_t output = 0;
    size_t expanded = 1;
    unsigned hops = 0;
    int syntax = 1;
    int jumped = 0;

    memset(seen, 0, sizeof seen);
    if (offset >= packet_len)
        return 0;
    while (1) {
        unsigned char length;

        if (position >= packet_len)
            return 0;
        length = packet[position];
        if ((length & 0xc0) == 0xc0) {
            size_t pointer;

            if (position + 1 >= packet_len || hops >= CN_DNS_POINTER_HOPS_MAX)
                return 0;
            pointer = (size_t)((length & 0x3f) << 8) | packet[position + 1];
            if (pointer >= packet_len ||
                (seen[pointer / 8] & (1U << (pointer % 8))))
                return 0;
            seen[pointer / 8] |= (unsigned char)(1U << (pointer % 8));
            if (!jumped) {
                *next_offset = position + 2;
                jumped = 1;
            }
            position = pointer;
            hops++;
            continue;
        }
        if (length & 0xc0)
            return 0;
        position++;
        if (length == 0) {
            if (!jumped)
                *next_offset = position;
            if (syntax)
                out[output] = '\0';
            else
                out[0] = '\0';
            *hostname_syntax = syntax;
            return 1;
        }
        if (length > 63 || position + length > packet_len ||
            expanded > 255 - (size_t)length - 1)
            return 0;
        expanded += (size_t)length + 1;
        if (syntax && !label_ok(packet + position, length)) {
            syntax = 0;
            output = 0;
        }
        if (syntax) {
            if (output != 0) {
                if (output >= CN_DNS_HOST_MAX)
                    return 0;
                out[output++] = '.';
            }
            if (length > CN_DNS_HOST_MAX - output)
                return 0;
            memcpy(out + output, packet + position, length);
            output += length;
        }
        position += length;
    }
}

static int answer_contains(const cn_dns_answer *answer, const char *address)
{
    size_t i;

    for (i = 0; i < answer->count; i++) {
        if (strcmp(answer->ipv4[i], address) == 0)
            return 1;
    }
    return 0;
}

static cn_dns_result parse_response(const unsigned char *packet,
                                    size_t packet_len, uint16_t expected_id,
                                    const char *hostname,
                                    cn_dns_answer *answer)
{
    char decoded[CN_DNS_HOST_MAX + 1];
    uint16_t flags;
    uint16_t question_count;
    uint16_t answer_count;
    uint16_t authority_count;
    uint16_t additional_count;
    unsigned rcode;
    size_t offset;
    size_t total_records;
    size_t i;
    int decoded_syntax;
    int saw_cname = 0;

    if (packet_len < CN_DNS_HEADER_BYTES)
        return CN_DNS_MALFORMED_RESPONSE;
    if (load_u16(packet) != expected_id)
        return CN_DNS_TIMEOUT; /* caller filters unrelated IDs first */
    flags = load_u16(packet + 2);
    if (!(flags & 0x8000) || (flags & 0x7800) || (flags & 0x0040))
        return CN_DNS_MALFORMED_RESPONSE;
    question_count = load_u16(packet + 4);
    answer_count = load_u16(packet + 6);
    authority_count = load_u16(packet + 8);
    additional_count = load_u16(packet + 10);
    if (question_count != 1)
        return CN_DNS_MALFORMED_RESPONSE;
    if (flags & 0x0200)
        return CN_DNS_TRUNCATED_RESPONSE;

    offset = CN_DNS_HEADER_BYTES;
    if (!decode_name(packet, packet_len, offset, decoded, &decoded_syntax,
                     &offset) || !decoded_syntax ||
        !name_equal(decoded, hostname) || offset + 4 > packet_len ||
        load_u16(packet + offset) != 1 ||
        load_u16(packet + offset + 2) != 1)
        return CN_DNS_MALFORMED_RESPONSE;
    offset += 4;

    total_records = (size_t)answer_count + authority_count + additional_count;
    if (total_records > CN_DNS_RECORDS_MAX)
        return CN_DNS_MALFORMED_RESPONSE;
    for (i = 0; i < total_records; i++) {
        char owner[CN_DNS_HOST_MAX + 1];
        uint16_t type;
        uint16_t class_value;
        uint16_t data_length;
        size_t data_offset;
        int owner_syntax;

        if (!decode_name(packet, packet_len, offset, owner, &owner_syntax,
                         &offset) || offset + 10 > packet_len)
            return CN_DNS_MALFORMED_RESPONSE;
        type = load_u16(packet + offset);
        class_value = load_u16(packet + offset + 2);
        data_length = load_u16(packet + offset + 8);
        data_offset = offset + 10;
        if (data_offset + data_length > packet_len)
            return CN_DNS_MALFORMED_RESPONSE;

        if (type == 1 && class_value == 1 && data_length != 4)
            return CN_DNS_MALFORMED_RESPONSE;
        if (i < answer_count && type == 1 && class_value == 1 &&
            owner_syntax && name_equal(owner, hostname)) {
            char address[CN_DNS_IPV4_TEXT_MAX];

            snprintf(address, sizeof address, "%u.%u.%u.%u",
                     packet[data_offset], packet[data_offset + 1],
                     packet[data_offset + 2], packet[data_offset + 3]);
            if (!answer_contains(answer, address) &&
                answer->count < CN_DNS_MAX_IPV4) {
                strcpy(answer->ipv4[answer->count], address);
                answer->count++;
            }
        } else if (i < answer_count && type == 5 && class_value == 1 &&
                   owner_syntax && name_equal(owner, hostname)) {
            size_t cname_end;

            if (!decode_name(packet, packet_len, data_offset, decoded,
                             &decoded_syntax, &cname_end) ||
                cname_end != data_offset + data_length)
                return CN_DNS_MALFORMED_RESPONSE;
            saw_cname = 1;
        }
        offset = data_offset + data_length;
    }
    if (offset != packet_len)
        return CN_DNS_MALFORMED_RESPONSE;

    rcode = flags & 0x000f;
    if (rcode != 0)
        memset(answer, 0, sizeof *answer);
    if (rcode == 3)
        return CN_DNS_NXDOMAIN;
    if (rcode != 0)
        return CN_DNS_SERVER_FAILURE;
    if (answer->count != 0)
        return CN_DNS_OK;
    return saw_cname ? CN_DNS_UNSUPPORTED_CNAME : CN_DNS_NO_ADDRESS;
}

static cn_dns_result query_server(const char *server, unsigned port,
                                  unsigned timeout_ms, const char *hostname,
                                  cn_dns_answer *answer)
{
    unsigned char query[CN_DNS_PACKET_MAX];
    unsigned char response[CN_DNS_RECV_CAP];
    struct sockaddr_in address;
    int64_t start;
    int64_t deadline;
    int64_t retransmit_deadlines[2];
    int64_t timeout_ns;
    size_t query_len;
    uint16_t transaction_id;
    int fd = -1;
    int flags;
    unsigned sends = 0;
    cn_dns_result result;

    memset(&address, 0, sizeof address);
    address.sin_family = AF_INET;
    address.sin_port = htons((unsigned short)port);
    if (!server || inet_pton(AF_INET, server, &address.sin_addr) != 1)
        return CN_DNS_INVALID;
    result = build_query(hostname, query, &query_len, &transaction_id);
    if (result != CN_DNS_OK)
        return result;
    if (!monotonic_ns(&start))
        return CN_DNS_NETWORK_FAILED;
    timeout_ns = (int64_t)timeout_ms * INT64_C(1000000);
    deadline = start + timeout_ns;
    retransmit_deadlines[0] = start + timeout_ns / 4;
    retransmit_deadlines[1] = start + timeout_ns / 2;
    DNS_TRACE("attempt server=%s:%u host=%s id=0x%04x timeout_ms=%u "
              "deadline_ns=%lld", server, port, hostname,
              (unsigned)transaction_id, timeout_ms, (long long)deadline);

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return CN_DNS_SOCKET_FAILED;
    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        result = CN_DNS_SOCKET_FAILED;
        goto done;
    }
    while (1) {
        while (1) {
            ssize_t sent;
            int remaining = poll_timeout_ms(deadline);

            if (remaining == -2) {
                result = CN_DNS_NETWORK_FAILED;
                goto done;
            }
            if (remaining < 0) {
                DNS_TRACE("timeout phase=send server=%s:%u id=0x%04x",
                          server, port, (unsigned)transaction_id);
                result = CN_DNS_TIMEOUT;
                goto done;
            }
            sent = sendto(fd, query, query_len, 0,
                          (const struct sockaddr *)&address, sizeof address);

            if (sent == (ssize_t)query_len) {
                DNS_TRACE("send %s server=%s:%u bytes=%u id=0x%04x",
                          sends == 0 ? "initial" :
                          sends == 1 ? "retransmit 1" : "retransmit 2",
                          server, port, (unsigned)query_len,
                          (unsigned)transaction_id);
                sends++;
                break;
            }
            if (sent < 0 && errno == EINTR)
                continue;
            if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                int ready = wait_for_socket(fd, POLLOUT, deadline);

                if (ready == 0) {
                    DNS_TRACE("timeout phase=send server=%s:%u id=0x%04x",
                              server, port, (unsigned)transaction_id);
                    result = CN_DNS_TIMEOUT;
                    goto done;
                }
                if (ready < 0) {
                    DNS_TRACE("wait-error phase=send server=%s:%u id=0x%04x",
                              server, port, (unsigned)transaction_id);
                    result = CN_DNS_NETWORK_FAILED;
                    goto done;
                }
                continue;
            }
            DNS_TRACE("send-error server=%s:%u id=0x%04x sent=%lld errno=%d",
                      server, port, (unsigned)transaction_id, (long long)sent,
                      sent < 0 ? errno : 0);
            result = CN_DNS_NETWORK_FAILED;
            goto done;
        }

        while (1) {
            struct sockaddr_in peer;
            socklen_t peer_len = sizeof peer;
            ssize_t received;
            int64_t wait_deadline = sends < 3
                                        ? retransmit_deadlines[sends - 1]
                                        : deadline;
            int ready = wait_for_socket(fd, POLLIN, wait_deadline);

            if (ready == 0) {
                if (sends < 3) {
                    int remaining = poll_timeout_ms(deadline);

                    if (remaining == -2) {
                        result = CN_DNS_NETWORK_FAILED;
                        goto done;
                    }
                    if (remaining >= 0)
                        break;
                }
                DNS_TRACE("timeout phase=receive server=%s:%u id=0x%04x",
                          server, port, (unsigned)transaction_id);
                result = CN_DNS_TIMEOUT;
                goto done;
            }
            if (ready < 0) {
                DNS_TRACE("wait-error phase=receive server=%s:%u id=0x%04x",
                          server, port, (unsigned)transaction_id);
                result = CN_DNS_NETWORK_FAILED;
                goto done;
            }
            memset(&peer, 0, sizeof peer);
            received = recvfrom(fd, response, sizeof response, 0,
                                (struct sockaddr *)&peer, &peer_len);
            if (received < 0 && (errno == EINTR || errno == EAGAIN ||
                                 errno == EWOULDBLOCK))
                continue;
            if (received < 0) {
                DNS_TRACE("recv-error server=%s:%u id=0x%04x errno=%d", server,
                          port, (unsigned)transaction_id, errno);
                result = CN_DNS_NETWORK_FAILED;
                goto done;
            }
            DNS_TRACE("recv bytes=%lld peer=%u.%u.%u.%u:%u id=%s0x%04x",
                      (long long)received,
                      (unsigned)((ntohl(peer.sin_addr.s_addr) >> 24) & 0xff),
                      (unsigned)((ntohl(peer.sin_addr.s_addr) >> 16) & 0xff),
                      (unsigned)((ntohl(peer.sin_addr.s_addr) >> 8) & 0xff),
                      (unsigned)(ntohl(peer.sin_addr.s_addr) & 0xff),
                      (unsigned)ntohs(peer.sin_port),
                      received >= 2 ? "" : "none/",
                      received >= 2 ? (unsigned)load_u16(response) : 0U);
            if (peer_len != sizeof peer || peer.sin_family != AF_INET ||
                peer.sin_port != address.sin_port ||
                peer.sin_addr.s_addr != address.sin_addr.s_addr) {
                DNS_TRACE("ignore reason=wrong-source expected=%s:%u id=0x%04x",
                          server, port, (unsigned)transaction_id);
                continue;
            }
            if (received < 2 || load_u16(response) != transaction_id) {
                DNS_TRACE("ignore reason=wrong-id expected=0x%04x",
                          (unsigned)transaction_id);
                continue;
            }
            if ((size_t)received > CN_DNS_PACKET_MAX) {
                DNS_TRACE("reject result=packet-size id=0x%04x",
                          (unsigned)transaction_id);
                result = CN_DNS_PACKET_SIZE;
                goto done;
            }
            result = parse_response(response, (size_t)received, transaction_id,
                                    hostname, answer);
            DNS_TRACE("parse result=%s id=0x%04x",
                      cn_dnssimple_result_name(result),
                      (unsigned)transaction_id);
            goto done;
        }
    }

done:
    close(fd);
    return result;
}

cn_dns_result cn_dnssimple_resolve_a(const cn_dns_config *config,
                                     const char *hostname,
                                     cn_dns_answer *answer)
{
    cn_dns_result last = CN_DNS_INVALID;
    unsigned port;
    unsigned timeout_ms;
    size_t i;

    if (!config || !answer || config->server_count == 0 ||
        config->server_count > CN_DNS_MAX_SERVERS)
        return CN_DNS_INVALID;
    if (!hostname_ok(hostname))
        return CN_DNS_INVALID_HOSTNAME;
    port = config->port ? config->port : CN_DNS_DEFAULT_PORT;
    timeout_ms = config->timeout_ms ? config->timeout_ms
                                    : CN_DNS_DEFAULT_TIMEOUT_MS;
    if (port > 65535)
        return CN_DNS_INVALID;

    memset(answer, 0, sizeof *answer);
    for (i = 0; i < config->server_count; i++) {
        memset(answer, 0, sizeof *answer);
        last = query_server(config->servers[i], port, timeout_ms, hostname,
                            answer);
        if (last == CN_DNS_OK) {
            answer->server_index = i;
            return CN_DNS_OK;
        }
        if (last == CN_DNS_NXDOMAIN || last == CN_DNS_NO_ADDRESS ||
            last == CN_DNS_UNSUPPORTED_CNAME ||
            last == CN_DNS_ENTROPY_FAILED ||
            last == CN_DNS_INVALID || last == CN_DNS_INVALID_HOSTNAME)
            return last;
    }
    memset(answer, 0, sizeof *answer);
    return last;
}

const char *cn_dnssimple_result_name(cn_dns_result result)
{
    static const char *const names[CN_DNS_RESULT_COUNT] = {
        "ok", "invalid", "invalid-hostname", "entropy-failed",
        "socket-failed", "network-failed", "timeout", "packet-size",
        "malformed-response", "truncated-response", "nxdomain",
        "server-failure", "no-address", "unsupported-cname"
    };

    if (result < 0 || result >= CN_DNS_RESULT_COUNT)
        return "unknown";
    return names[result];
}

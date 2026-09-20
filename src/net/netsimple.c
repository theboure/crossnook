#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <stdint.h>
#include <stdlib.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include "net/netsimple.h"
#include "net/tlssimple.h"

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int poll_interval_ms(int64_t deadline)
{
    int64_t remaining = deadline - now_ms();
    if (remaining < 0)
        return -1;
    if (remaining > INT_MAX)
        return INT_MAX;
    return (int)remaining;
}

static cn_netsimple_result cn_tls_map(cn_tls_result r)
{
    switch (r) {
    case CN_TLS_OK:
        return CN_NETSIMPLE_OK;
    case CN_TLS_INVALID:
        return CN_NETSIMPLE_INVALID;
    case CN_TLS_INTERNAL:
        return CN_NETSIMPLE_TLS_INTERNAL;
    case CN_TLS_ENTROPY_FAILED:
        return CN_NETSIMPLE_TLS_ENTROPY_FAILED;
    case CN_TLS_INVALID_CA:
        return CN_NETSIMPLE_TLS_INVALID_CA;
    case CN_TLS_HANDSHAKE_FAILED:
        return CN_NETSIMPLE_TLS_HANDSHAKE_FAILED;
    case CN_TLS_HANDSHAKE_TIMEOUT:
        return CN_NETSIMPLE_TLS_HANDSHAKE_TIMEOUT;
    case CN_TLS_TRUST_FAILED:
        return CN_NETSIMPLE_TLS_TRUST_FAILED;
    case CN_TLS_HOSTNAME_MISMATCH:
        return CN_NETSIMPLE_TLS_HOSTNAME_MISMATCH;
    case CN_TLS_CERT_TIME_FAILED:
        return CN_NETSIMPLE_TLS_CERT_TIME_FAILED;
    case CN_TLS_CERT_INVALID:
        return CN_NETSIMPLE_TLS_CERT_INVALID;
    case CN_TLS_PROTOCOL_FAILED:
        return CN_NETSIMPLE_TLS_PROTOCOL_FAILED;
    case CN_TLS_CLOSED:
        return CN_NETSIMPLE_TLS_PROTOCOL_FAILED;
    case CN_TLS_RECV_TIMEOUT:
        return CN_NETSIMPLE_TLS_RECV_TIMEOUT;
    case CN_TLS_SEND_TIMEOUT:
        return CN_NETSIMPLE_SEND_TIMEOUT;
    default:
        return CN_NETSIMPLE_TLS_INTERNAL;
    }
}

int cn_netsimple_parse_ipv4(const char *text, unsigned char out[4])
{
    int octets[4];
    int current = 0;
    int count = 0;
    int started = 0;

    if (!text)
        return 0;
    while (1) {
        char c = *text++;
        if (c >= '0' && c <= '9') {
            if (current > 255)
                return 0;
            current = current * 10 + (c - '0');
            if (current > 255)
                return 0;
            started = 1;
        } else if (c == '.' && count < 3 && started) {
            octets[count++] = current;
            current = 0;
            started = 0;
        } else if (c == '\0') {
            if (count != 3 || !started)
                return 0;
            octets[count] = current;
            if (out) {
                out[0] = (unsigned char)octets[0];
                out[1] = (unsigned char)octets[1];
                out[2] = (unsigned char)octets[2];
                out[3] = (unsigned char)octets[3];
            }
            return 1;
        } else {
            return 0;
        }
    }
}

int cn_netsimple_parse_port(const char *text, int *out)
{
    long value = 0;
    size_t digits = 0;
    const char *p;

    if (!text)
        return 0;
    for (p = text; *p; ++p) {
        if (*p < '0' || *p > '9')
            return 0;
        if (digits >= CN_NETSIMPLE_PORT_MAX_TEXT)
            return 0;
        value = value * 10 + (*p - '0');
        if (value > 65535)
            return 0;
        ++digits;
    }
    if (digits == 0 || value < 1)
        return 0;
    if (out)
        *out = (int)value;
    return 1;
}

static int host_text_ok(const char *host)
{
    size_t n;
    if (!host || host[0] == '\0')
        return 0;
    for (n = 0; host[n] != '\0'; ++n) {
        if (n >= CN_NETSIMPLE_HOST_MAX)
            return 0;
        if ((unsigned char)host[n] <= 0x20 || host[n] == 0x7f ||
            host[n] == '/' || host[n] == '\\')
            return 0;
    }
    return 1;
}

int cn_netsimple_validate(const char *host, const char *port, const char *path)
{
    int parsed_port;

    if (!host || !port || !path)
        return 0;
    if (!host_text_ok(host))
        return 0;
    if (!cn_netsimple_parse_port(port, &parsed_port))
        return 0;
    if (path[0] != '/')
        return 0;
    {
        size_t i;
        for (i = 0; path[i] != '\0'; ++i) {
            unsigned char c = (unsigned char)path[i];
            if (i >= CN_NETSIMPLE_PATH_MAX || c <= 0x20 || c == 0x7f ||
                c == '\\')
                return 0;
        }
    }
    (void)parsed_port;
    return 1;
}

cn_netsimple_result cn_netsimple_build_request(const char *host,
                                               const char *port,
                                               const char *path,
                                               char *request,
                                               size_t request_cap,
                                               size_t *request_len)
{
    int written;
    if (!host || !port || !path || !request || !request_len)
        return CN_NETSIMPLE_INVALID;
    written = snprintf(request, request_cap,
                       "GET %s HTTP/1.0\r\n"
                       "Host: %s\r\n"
                       "\r\n",
                       path, host);
    if (written < 0)
        return CN_NETSIMPLE_INVALID;
    if ((size_t)written >= request_cap)
        return CN_NETSIMPLE_INVALID;
    *request_len = (size_t)written;
    return CN_NETSIMPLE_OK;
}

static int header_name_ok(const char *name)
{
    size_t i;
    if (!name || name[0] == '\0')
        return 0;
    for (i = 0; name[i] != '\0'; ++i) {
        char c = name[i];
        if (i >= 63 || !((c >= 'a' && c <= 'z') ||
                         (c >= 'A' && c <= 'Z') ||
                         (c >= '0' && c <= '9') || c == '-'))
            return 0;
    }
    return 1;
}

static int header_value_ok(const char *value)
{
    size_t i;
    if (!value)
        return 0;
    for (i = 0; value[i] != '\0'; ++i) {
        unsigned char c = (unsigned char)value[i];
        if (i >= 2048 || c < 0x20 || c == 0x7f)
            return 0;
    }
    return 1;
}

static cn_netsimple_result append_text(char *request, size_t request_cap,
                                       size_t *used, const char *text)
{
    size_t len = strlen(text);
    if (*used >= request_cap || len >= request_cap - *used)
        return CN_NETSIMPLE_INVALID;
    memcpy(request + *used, text, len);
    *used += len;
    request[*used] = '\0';
    return CN_NETSIMPLE_OK;
}

cn_netsimple_result cn_netsimple_build_exchange_request(
    const cn_netsimple_request *spec,
    char *request, size_t request_cap, size_t *request_len)
{
    const char *method;
    size_t used = 0;
    size_t i;
    char length_line[64];
    int written;

    if (!spec || !request || !request_len || request_cap == 0 ||
        !cn_netsimple_validate(spec->host, spec->port, spec->path) ||
        (spec->connect_host && !host_text_ok(spec->connect_host)) ||
        spec->header_count > CN_NETSIMPLE_HEADER_MAX ||
        (spec->header_count != 0 && !spec->headers) ||
        (spec->body_len != 0 && !spec->body))
        return CN_NETSIMPLE_INVALID;
    if (spec->method == CN_NETSIMPLE_METHOD_GET) {
        if (spec->body || spec->body_len || spec->content_type)
            return CN_NETSIMPLE_INVALID;
        method = "GET";
    } else if (spec->method == CN_NETSIMPLE_METHOD_PUT) {
        if (!spec->body || !spec->content_type ||
            !header_value_ok(spec->content_type))
            return CN_NETSIMPLE_INVALID;
        method = "PUT";
    } else {
        return CN_NETSIMPLE_INVALID;
    }
    for (i = 0; i < spec->header_count; ++i) {
        if (!header_name_ok(spec->headers[i].name) ||
            !header_value_ok(spec->headers[i].value))
            return CN_NETSIMPLE_INVALID;
    }

    request[0] = '\0';
    if (append_text(request, request_cap, &used, method) != CN_NETSIMPLE_OK ||
        append_text(request, request_cap, &used, " ") != CN_NETSIMPLE_OK ||
        append_text(request, request_cap, &used, spec->path) != CN_NETSIMPLE_OK ||
        append_text(request, request_cap, &used,
                    " HTTP/1.0\r\nHost: ") != CN_NETSIMPLE_OK ||
        append_text(request, request_cap, &used, spec->host) != CN_NETSIMPLE_OK)
        return CN_NETSIMPLE_INVALID;
    if ((spec->tls && strcmp(spec->port, "443") != 0) ||
        (!spec->tls && strcmp(spec->port, "80") != 0)) {
        if (append_text(request, request_cap, &used, ":") != CN_NETSIMPLE_OK ||
            append_text(request, request_cap, &used,
                        spec->port) != CN_NETSIMPLE_OK)
            return CN_NETSIMPLE_INVALID;
    }
    if (append_text(request, request_cap, &used, "\r\n") != CN_NETSIMPLE_OK)
        return CN_NETSIMPLE_INVALID;
    for (i = 0; i < spec->header_count; ++i) {
        if (append_text(request, request_cap, &used,
                        spec->headers[i].name) != CN_NETSIMPLE_OK ||
            append_text(request, request_cap, &used, ": ") != CN_NETSIMPLE_OK ||
            append_text(request, request_cap, &used,
                        spec->headers[i].value) != CN_NETSIMPLE_OK ||
            append_text(request, request_cap, &used, "\r\n") != CN_NETSIMPLE_OK)
            return CN_NETSIMPLE_INVALID;
    }
    if (spec->method == CN_NETSIMPLE_METHOD_PUT) {
        if (append_text(request, request_cap, &used,
                        "Content-Type: ") != CN_NETSIMPLE_OK ||
            append_text(request, request_cap, &used,
                        spec->content_type) != CN_NETSIMPLE_OK ||
            append_text(request, request_cap, &used, "\r\n") != CN_NETSIMPLE_OK)
            return CN_NETSIMPLE_INVALID;
        written = snprintf(length_line, sizeof length_line,
                           "Content-Length: %llu\r\n",
                           (unsigned long long)spec->body_len);
        if (written < 0 || (size_t)written >= sizeof length_line ||
            append_text(request, request_cap, &used,
                        length_line) != CN_NETSIMPLE_OK)
            return CN_NETSIMPLE_INVALID;
    }
    if (append_text(request, request_cap, &used, "\r\n") != CN_NETSIMPLE_OK)
        return CN_NETSIMPLE_INVALID;
    *request_len = used;
    return CN_NETSIMPLE_OK;
}

static cn_netsimple_result parse_code(const char *buf, size_t len, size_t *pos,
                                      int *code, const char **code_text)
{
    size_t i = *pos;
    int value = 0;
    int digits = 0;

    while (i < len && (buf[i] == ' ' || buf[i] == '\t'))
        ++i;
    for (; i < len; ++i) {
        if (buf[i] < '0' || buf[i] > '9')
            return CN_NETSIMPLE_BAD_RESPONSE;
        value = value * 10 + (buf[i] - '0');
        ++digits;
        if (digits == 3) {
            ++i;
            break;
        }
    }
    if (digits != 3)
        return CN_NETSIMPLE_BAD_RESPONSE;
    *code = value;
    *pos = i;
    while (*pos < len && (buf[*pos] == ' ' || buf[*pos] == '\t'))
        ++*pos;
    *code_text = buf + *pos;
    return CN_NETSIMPLE_OK;
}

cn_netsimple_result cn_netsimple_parse_status(const char *buf, size_t len,
                                              cn_netsimple_response *out)
{
    size_t i = 0;
    size_t crlfcrlf;
    size_t lflf;
    int code = 0;
    const char *code_text = NULL;
    cn_netsimple_result result;

    if (!buf || !out)
        return CN_NETSIMPLE_INVALID;
    memset(out, 0, sizeof *out);

    while (i < len && (buf[i] == '\r' || buf[i] == '\n'))
        ++i;
    if (len - i < 5 || memcmp(buf + i, "HTTP/", 5) != 0)
        return CN_NETSIMPLE_BAD_RESPONSE;
    i += 5;
    if (i >= len || buf[i] < '0' || buf[i] > '9')
        return CN_NETSIMPLE_BAD_RESPONSE;
    ++i;
    if (i >= len || buf[i] != '.')
        return CN_NETSIMPLE_BAD_RESPONSE;
    ++i;
    if (i >= len || buf[i] < '0' || buf[i] > '9')
        return CN_NETSIMPLE_BAD_RESPONSE;
    ++i;
    if (i < len && buf[i] >= '0' && buf[i] <= '9')
        ++i;
    if (i >= len || (buf[i] != ' ' && buf[i] != '\t' && buf[i] != '\r'))
        return CN_NETSIMPLE_BAD_RESPONSE;

    result = parse_code(buf, len, &i, &code, &code_text);
    if (result != CN_NETSIMPLE_OK)
        return result;
    out->status = code;
    out->status_text = code_text;
    while (i + out->status_text_bytes < len &&
           code_text[out->status_text_bytes] != '\r' &&
           code_text[out->status_text_bytes] != '\n')
        out->status_text_bytes++;

    crlfcrlf = len;
    lflf = len;
    for (i = 0; i + 3 < len; ++i)
        if (buf[i] == '\r' && buf[i + 1] == '\n' &&
            buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            crlfcrlf = i;
            break;
        }
    for (i = 0; i + 1 < len; ++i)
        if (buf[i] == '\n' && buf[i + 1] == '\n') {
            lflf = i;
            break;
        }
    if (crlfcrlf == len && lflf == len) {
        out->header_bytes = len;
    } else if (crlfcrlf <= lflf) {
        out->header_bytes = crlfcrlf + 4;
    } else {
        out->header_bytes = lflf + 2;
    }
    if (out->header_bytes > len)
        out->header_bytes = len;
    out->body_bytes = len - out->header_bytes;
    out->total_bytes = len;
    return CN_NETSIMPLE_OK;
}

/* Return 2 with a framed response size, 1 for complete unframed headers,
 * 0 for incomplete headers, and -1 for malformed/overflowing length. */
static int response_content_length(const char *buf, size_t len,
                                   size_t *expected)
{
    size_t header_end = 0;
    size_t line;
    size_t body_offset;
    size_t content_length = 0;
    int found = 0;

    for (line = 0; line + 3 < len; line++) {
        if (buf[line] == '\r' && buf[line + 1] == '\n' &&
            buf[line + 2] == '\r' && buf[line + 3] == '\n') {
            header_end = line;
            body_offset = line + 4;
            break;
        }
    }
    if (!header_end)
        return 0;
    line = 0;
    while (line < header_end && buf[line] != '\n')
        line++;
    if (line < header_end)
        line++;
    while (line < header_end) {
        static const char name[] = "content-length";
        size_t end = line;
        size_t colon;
        size_t i;
        size_t value = 0;
        int digits = 0;
        int name_match;

        while (end < header_end && buf[end] != '\n')
            end++;
        colon = line;
        while (colon < end && buf[colon] != ':')
            colon++;
        name_match = colon - line == sizeof name - 1;
        for (i = 0; name_match && i < sizeof name - 1; i++) {
            unsigned char c = (unsigned char)buf[line + i];

            if (c >= 'A' && c <= 'Z')
                c = (unsigned char)(c - 'A' + 'a');
            if (c != (unsigned char)name[i])
                name_match = 0;
        }
        if (name_match) {
            i = colon + 1;
            while (i < end && (buf[i] == ' ' || buf[i] == '\t'))
                i++;
            while (i < end && buf[i] >= '0' && buf[i] <= '9') {
                unsigned digit = (unsigned)(buf[i] - '0');

                if (value > (SIZE_MAX - digit) / 10)
                    return -1;
                value = value * 10 + digit;
                digits = 1;
                i++;
            }
            while (i < end && (buf[i] == ' ' || buf[i] == '\t' ||
                               buf[i] == '\r'))
                i++;
            if (!digits || i != end || found)
                return -1;
            content_length = value;
            found = 1;
        }
        line = end + 1;
    }
    if (!found)
        return 1;
    if (content_length > SIZE_MAX - body_offset)
        return -1;
    *expected = body_offset + content_length;
    return 2;
}

cn_netsimple_result cn_netsimple_connect_error(int error)
{
    switch (error) {
    case ECONNREFUSED:
        return CN_NETSIMPLE_CONNECT_REFUSED;
    case ENETUNREACH:
        return CN_NETSIMPLE_NETWORK_UNREACHABLE;
    case EHOSTUNREACH:
        return CN_NETSIMPLE_HOST_UNREACHABLE;
    case ETIMEDOUT:
    case EINPROGRESS:
        return CN_NETSIMPLE_CONNECT_TIMEOUT;
    default:
        return CN_NETSIMPLE_CONNECT_ERROR;
    }
}

static cn_netsimple_result connect_with_timeout(int fd,
                                                const struct sockaddr *address,
                                                socklen_t address_len,
                                                unsigned timeout_ms)
{
    int flags;
    int value;
    socklen_t value_len = sizeof value;
    struct pollfd descriptor;
    int64_t deadline;
    int poll_ms;
    int polled;

    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return CN_NETSIMPLE_CONNECT_ERROR;

    if (connect(fd, address, address_len) == 0) {
        if (fcntl(fd, F_SETFL, flags) < 0)
            return CN_NETSIMPLE_CONNECT_ERROR;
        return CN_NETSIMPLE_OK;
    }
    if (errno != EINPROGRESS) {
        fcntl(fd, F_SETFL, flags);
        return cn_netsimple_connect_error(errno);
    }

    deadline = now_ms() + (int64_t)timeout_ms;
    descriptor.fd = fd;
    descriptor.events = POLLOUT;
    descriptor.revents = 0;
    while (1) {
        poll_ms = poll_interval_ms(deadline);
        if (poll_ms < 0)
            return CN_NETSIMPLE_CONNECT_TIMEOUT;
        polled = poll(&descriptor, 1, poll_ms);
        if (polled == 0)
            return CN_NETSIMPLE_CONNECT_TIMEOUT;
        if (polled < 0) {
            if (errno == EINTR)
                continue;
            fcntl(fd, F_SETFL, flags);
            return CN_NETSIMPLE_CONNECT_ERROR;
        }
        break;
    }

    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &value, &value_len) < 0) {
        fcntl(fd, F_SETFL, flags);
        return CN_NETSIMPLE_CONNECT_ERROR;
    }
    fcntl(fd, F_SETFL, flags);
    if (value != 0)
        return cn_netsimple_connect_error(value);
    return CN_NETSIMPLE_OK;
}

static cn_netsimple_result send_buffer(int fd, const char *data, size_t length,
                                       int64_t deadline, int *early_response)
{
    size_t sent_total = 0;
    while (sent_total < length) {
        struct pollfd descriptor;
        int poll_ms = poll_interval_ms(deadline);
        int polled;
        ssize_t sent;
        if (poll_ms < 0)
            return CN_NETSIMPLE_SEND_TIMEOUT;
        descriptor.fd = fd;
        descriptor.events = POLLOUT | POLLIN;
        descriptor.revents = 0;
        polled = poll(&descriptor, 1, poll_ms);
        if (polled == 0)
            return CN_NETSIMPLE_SEND_TIMEOUT;
        if (polled < 0) {
            if (errno == EINTR)
                continue;
            return CN_NETSIMPLE_SEND_ERROR;
        }
        if (descriptor.revents & POLLIN) {
            *early_response = 1;
            return CN_NETSIMPLE_OK;
        }
        if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))
            return CN_NETSIMPLE_SEND_ERROR;
        sent = send(fd, data + sent_total, length - sent_total,
                    MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            return CN_NETSIMPLE_SEND_ERROR;
        }
        if (sent == 0)
            return CN_NETSIMPLE_SEND_ERROR;
        sent_total += (size_t)sent;
    }
    return CN_NETSIMPLE_OK;
}

cn_netsimple_result cn_netsimple_exchange(
    const cn_netsimple_request *spec,
    char *buffer, size_t buffer_cap,
    unsigned connect_ms, unsigned recv_ms,
    cn_netsimple_response *out)
{
    unsigned char address_bytes[4];
    struct sockaddr_in address6;
    struct addrinfo hints;
    struct addrinfo *resolved = NULL;
    struct addrinfo *entry;
    char request[CN_NETSIMPLE_REQUEST_MAX];
    size_t request_len;
    size_t total = 0;
    int fd = -1;
    int connected = 0;
    int truncated = 0;
    cn_netsimple_result result = CN_NETSIMPLE_INVALID;
    int64_t recv_deadline;
    int send_flags;
    int64_t send_deadline;
    int early_response = 0;
    const char *connect_host;
    int tls_framing = 0;
    size_t tls_expected = 0;

    if (!spec || !out)
        return CN_NETSIMPLE_INVALID;
    memset(out, 0, sizeof *out);
    if (!buffer || buffer_cap == 0)
        return CN_NETSIMPLE_INVALID;

    result = cn_netsimple_build_exchange_request(spec, request,
                                                  sizeof request,
                                                  &request_len);
    if (result != CN_NETSIMPLE_OK)
        return result;
    connect_host = spec->connect_host ? spec->connect_host : spec->host;

    memset(&address6, 0, sizeof address6);
    address6.sin_family = AF_INET;
    address6.sin_port = htons((unsigned short)atoi(spec->port));
    if (cn_netsimple_parse_ipv4(connect_host, address_bytes)) {
        memcpy(&address6.sin_addr.s_addr, address_bytes, 4);
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            return CN_NETSIMPLE_CONNECT_ERROR;
        result = connect_with_timeout(fd,
                                      (const struct sockaddr *)&address6,
                                      (socklen_t)sizeof address6,
                                      connect_ms);
        if (result == CN_NETSIMPLE_OK) {
            connected = 1;
        } else {
            close(fd);
            fd = -1;
        }
    } else {
        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_NUMERICSERV;
        resolved = NULL;
        if (getaddrinfo(connect_host, spec->port, &hints, &resolved) != 0 ||
            !resolved)
            return CN_NETSIMPLE_RESOLVE_ERROR;
        for (entry = resolved; entry; entry = entry->ai_next) {
            if (entry->ai_family != AF_INET)
                continue;
            fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) {
                result = CN_NETSIMPLE_CONNECT_ERROR;
                continue;
            }
            result = connect_with_timeout(fd, entry->ai_addr,
                                          (socklen_t)entry->ai_addrlen,
                                          connect_ms);
            if (result == CN_NETSIMPLE_OK) {
                connected = 1;
                break;
            }
            close(fd);
            fd = -1;
        }
        freeaddrinfo(resolved);
    }
    if (!connected)
        return result;

    send_flags = fcntl(fd, F_GETFL, 0);
    if (send_flags < 0 ||
        fcntl(fd, F_SETFL, send_flags | O_NONBLOCK) < 0) {
        result = CN_NETSIMPLE_SEND_ERROR;
        goto done;
    }

    if (spec->tls) {
        cn_tls_conn tls_conn;
        cn_tls_result tls_result;
        int64_t tls_deadline = now_ms() + (int64_t)recv_ms;

        tls_result = cn_tls_open(&tls_conn, fd, spec->tls, spec->host,
                                 tls_deadline);
        fd = -1; /* the TLS layer owns the descriptor from here on */
        if (tls_result != CN_TLS_OK) {
            result = cn_tls_map(tls_result);
            goto done;
        }
        tls_result = cn_tls_send_all(&tls_conn, request, request_len,
                                     tls_deadline);
        if (tls_result == CN_TLS_OK && spec->body_len != 0)
            tls_result = cn_tls_send_all(&tls_conn, spec->body,
                                         spec->body_len, tls_deadline);
        if (tls_result != CN_TLS_OK) {
            result = tls_result == CN_TLS_SEND_TIMEOUT
                         ? CN_NETSIMPLE_SEND_TIMEOUT
                         : CN_NETSIMPLE_SEND_ERROR;
            cn_tls_close(&tls_conn);
            goto done;
        }

        total = 0;
        recv_deadline = now_ms() + (int64_t)recv_ms;
        while (1) {
            size_t got = 0;
            int closed = 0;

            if (total >= buffer_cap) {
                truncated = 1;
                break;
            }
            tls_result = cn_tls_recv_some(&tls_conn, buffer + total,
                                          buffer_cap - total, recv_deadline,
                                          &got, &closed);
            total += got;
            {
                int framed = response_content_length(buffer, total,
                                                     &tls_expected);

                if (framed < 0 || (framed == 2 && total > tls_expected)) {
                    result = CN_NETSIMPLE_BAD_RESPONSE;
                    cn_tls_close(&tls_conn);
                    goto done;
                }
                tls_framing = framed;
                if (framed == 2 && total == tls_expected)
                    break;
            }
            if (tls_result == CN_TLS_CLOSED) {
                if (tls_framing == 0 ||
                    (tls_framing == 2 && total != tls_expected)) {
                    result = CN_NETSIMPLE_TRUNCATED;
                    cn_tls_close(&tls_conn);
                    goto done;
                }
                break;
            }
            if (tls_result != CN_TLS_OK) {
                result = tls_result == CN_TLS_RECV_TIMEOUT
                             ? CN_NETSIMPLE_TLS_RECV_TIMEOUT
                             : CN_NETSIMPLE_TLS_PROTOCOL_FAILED;
                cn_tls_close(&tls_conn);
                goto done;
            }
            if (closed || got == 0)
                break;
        }
        cn_tls_close(&tls_conn);
        goto recv_done;
    }

    send_deadline = now_ms() + (int64_t)recv_ms;
    result = send_buffer(fd, request, request_len, send_deadline,
                         &early_response);
    if (result == CN_NETSIMPLE_OK && !early_response && spec->body_len != 0)
        result = send_buffer(fd, (const char *)spec->body, spec->body_len,
                             send_deadline, &early_response);
    if (fcntl(fd, F_SETFL, send_flags) < 0 && result == CN_NETSIMPLE_OK)
        result = CN_NETSIMPLE_SEND_ERROR;
    if (result != CN_NETSIMPLE_OK)
        goto done;

    total = 0;
    recv_deadline = now_ms() + (int64_t)recv_ms;
    while (1) {
        struct pollfd descriptor;
        int polled;

        if (total >= buffer_cap) {
            truncated = 1;
            break;
        }
        int poll_ms = poll_interval_ms(recv_deadline);
        if (poll_ms < 0) {
            result = CN_NETSIMPLE_RECV_TIMEOUT;
            goto done;
        }
        descriptor.fd = fd;
        descriptor.events = POLLIN;
        descriptor.revents = 0;
        polled = poll(&descriptor, 1, poll_ms);
        if (polled == 0) {
            result = CN_NETSIMPLE_RECV_TIMEOUT;
            goto done;
        }
        if (polled < 0) {
            if (errno == EINTR)
                continue;
            result = CN_NETSIMPLE_RECV_ERROR;
            goto done;
        }
        if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            if (descriptor.revents & (POLLERR | POLLHUP)) {
                while (total < buffer_cap) {
                    ssize_t got = recv(fd, buffer + total,
                                       buffer_cap - total, 0);
                    if (got == 0)
                        break;
                    if (got < 0) {
                        if (errno == EINTR)
                            continue;
                        if (errno == EAGAIN)
                            goto recv_done;
                        result = CN_NETSIMPLE_RECV_ERROR;
                        goto done;
                    }
                    total += (size_t)got;
                }
                if (total >= buffer_cap)
                    truncated = 1;
            } else {
                result = CN_NETSIMPLE_RECV_ERROR;
                goto done;
            }
            break;
        }
        {
            ssize_t got = recv(fd, buffer + total, buffer_cap - total, 0);
            if (got == 0)
                break;
            if (got < 0) {
                if (errno == EINTR)
                    continue;
                if (errno == EAGAIN)
                    continue;
                result = CN_NETSIMPLE_RECV_ERROR;
                goto done;
            }
            total += (size_t)got;
        }
    }

recv_done:
    {
        cn_netsimple_response parsed;
        cn_netsimple_result parse_result =
            cn_netsimple_parse_status(buffer, total, &parsed);
        if (parse_result != CN_NETSIMPLE_OK) {
            result = truncated ? CN_NETSIMPLE_TRUNCATED : parse_result;
            goto done;
        }
        if (truncated) {
            result = CN_NETSIMPLE_TRUNCATED;
        } else {
            result = CN_NETSIMPLE_OK;
        }
        memcpy(out, &parsed, sizeof parsed);
    }

done:
    if (fd >= 0)
        close(fd);
    return result;
}

cn_netsimple_result cn_netsimple_get(const char *host, const char *port,
                                     const char *path,
                                     char *buffer, size_t buffer_cap,
                                     unsigned connect_ms, unsigned recv_ms,
                                     cn_netsimple_response *out)
{
    cn_netsimple_request request;
    memset(&request, 0, sizeof request);
    request.method = CN_NETSIMPLE_METHOD_GET;
    request.host = host;
    request.port = port;
    request.path = path;
    return cn_netsimple_exchange(&request, buffer, buffer_cap,
                                 connect_ms, recv_ms, out);
}

const char *cn_netsimple_result_name(cn_netsimple_result result)
{
    static const char *const names[] = {
        "ok", "invalid", "resolve-error", "connect-refused",
        "network-unreachable", "host-unreachable", "connect-timeout",
        "connect-error", "send-error", "recv-error", "recv-timeout",
        "truncated", "bad-response", "send-timeout",
        "tls-entropy-failed", "tls-invalid-ca", "tls-handshake-failed",
        "tls-handshake-timeout", "tls-trust-failed",
        "tls-hostname-mismatch", "tls-cert-time-failed", "tls-cert-invalid",
        "tls-protocol-failed", "tls-internal", "tls-recv-timeout"
    };
    if ((size_t)result < CN_NETSIMPLE_LENGTH)
        return names[result];
    return "unknown";
}

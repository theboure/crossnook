#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
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

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
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
        if (host[n] == ' ' || host[n] == '\t' || host[n] == '/' ||
            host[n] == '\\')
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
    if (strlen(path) > CN_NETSIMPLE_PATH_MAX)
        return 0;
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
    long deadline;
    long remaining;
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

    deadline = now_ms() + (long)timeout_ms;
    descriptor.fd = fd;
    descriptor.events = POLLOUT;
    descriptor.revents = 0;
    while (1) {
        remaining = deadline - now_ms();
        if (remaining < 0)
            return CN_NETSIMPLE_CONNECT_TIMEOUT;
        polled = poll(&descriptor, 1, (int)remaining);
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

cn_netsimple_result cn_netsimple_get(const char *host, const char *port,
                                     const char *path,
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
    long recv_deadline;
    long remaining;

    if (!out)
        return CN_NETSIMPLE_INVALID;
    memset(out, 0, sizeof *out);
    if (!cn_netsimple_validate(host, port, path))
        return CN_NETSIMPLE_INVALID;
    if (!buffer || buffer_cap == 0)
        return CN_NETSIMPLE_INVALID;

    result = cn_netsimple_build_request(host, port, path,
                                        request, sizeof request,
                                        &request_len);
    if (result != CN_NETSIMPLE_OK)
        return result;

    memset(&address6, 0, sizeof address6);
    address6.sin_family = AF_INET;
    address6.sin_port = htons((unsigned short)atoi(port));
    if (cn_netsimple_parse_ipv4(host, address_bytes)) {
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
        if (getaddrinfo(host, port, &hints, &resolved) != 0 || !resolved)
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

    while (total < request_len) {
        ssize_t sent = send(fd, request + total, request_len - total,
                            MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR)
                continue;
            result = CN_NETSIMPLE_SEND_ERROR;
            goto done;
        }
        total += (size_t)sent;
    }

    total = 0;
    recv_deadline = now_ms() + (long)recv_ms;
    while (1) {
        struct pollfd descriptor;
        int polled;

        if (total >= buffer_cap) {
            truncated = 1;
            break;
        }
        remaining = recv_deadline - now_ms();
        if (remaining < 0) {
            result = CN_NETSIMPLE_RECV_TIMEOUT;
            goto done;
        }
        descriptor.fd = fd;
        descriptor.events = POLLIN;
        descriptor.revents = 0;
        polled = poll(&descriptor, 1, (int)remaining);
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

const char *cn_netsimple_result_name(cn_netsimple_result result)
{
    static const char *const names[] = {
        "ok", "invalid", "resolve-error", "connect-refused",
        "network-unreachable", "host-unreachable", "connect-timeout",
        "connect-error", "send-error", "recv-error", "recv-timeout",
        "truncated", "bad-response"
    };
    if ((size_t)result < CN_NETSIMPLE_LENGTH)
        return names[result];
    return "unknown";
}
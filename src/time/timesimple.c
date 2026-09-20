#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "time/timesimple.h"

#define CN_SNTP_PACKET_BYTES 48
#define CN_SNTP_RESPONSE_CAP (CN_SNTP_PACKET_BYTES + 1)
#define CN_NTP_UNIX_DELTA INT64_C(2208988800)
#define CN_NTP_ERA_SECONDS INT64_C(4294967296)

static int monotonic_ns(int64_t *out)
{
    struct timespec ts;

    if (!out || clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    *out = (int64_t)ts.tv_sec * INT64_C(1000000000) + ts.tv_nsec;
    return 1;
}

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
        if (descriptor.revents & events)
            return 1;
        if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))
            return -1;
    }
}

static uint32_t load_u32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void store_u32(unsigned char *p, uint32_t value)
{
    p[0] = (unsigned char)(value >> 24);
    p[1] = (unsigned char)(value >> 16);
    p[2] = (unsigned char)(value >> 8);
    p[3] = (unsigned char)value;
}

static int timestamp_nonzero(const unsigned char *p)
{
    size_t i;

    for (i = 0; i < 8; i++) {
        if (p[i] != 0)
            return 1;
    }
    return 0;
}

static int read_fraction_entropy(unsigned char out[4])
{
    size_t total = 0;
    int fd = open("/dev/urandom", O_RDONLY);

    if (fd < 0)
        return 0;
    while (total < 4) {
        ssize_t got = read(fd, out + total, 4 - total);

        if (got > 0) {
            total += (size_t)got;
            continue;
        }
        if (got < 0 && errno == EINTR)
            continue;
        close(fd);
        return 0;
    }
    close(fd);
    return 1;
}

static cn_time_result build_request(unsigned char request[CN_SNTP_PACKET_BYTES])
{
    struct timespec wall;
    unsigned char fraction[4];
    uint32_t ntp_seconds;

    if (clock_gettime(CLOCK_REALTIME, &wall) != 0)
        return CN_TIME_INTERNAL;
    if (!read_fraction_entropy(fraction))
        return CN_TIME_ENTROPY_FAILED;

    memset(request, 0, CN_SNTP_PACKET_BYTES);
    request[0] = (unsigned char)((4U << 3) | 3U); /* LI=0, VN=4, mode=3 */
    ntp_seconds = (uint32_t)((uint64_t)wall.tv_sec +
                             (uint64_t)CN_NTP_UNIX_DELTA);
    store_u32(request + 40, ntp_seconds);
    memcpy(request + 44, fraction, sizeof fraction);
    return CN_TIME_OK;
}

static cn_time_result decode_transmit_time(const unsigned char *timestamp,
                                           int64_t rtt_ns,
                                           cn_time_sample *sample)
{
    uint32_t seconds = load_u32(timestamp);
    uint32_t fraction = load_u32(timestamp + 4);
    int64_t era0 = (int64_t)seconds - CN_NTP_UNIX_DELTA;
    int64_t era1 = (int64_t)seconds + CN_NTP_ERA_SECONDS -
                   CN_NTP_UNIX_DELTA;
    int64_t candidate = 0;
    int matches = 0;
    int64_t nanoseconds;

    if (era0 >= CN_TIME_RELEASE_MIN_UNIX &&
        era0 <= CN_TIME_TARGET_MAX_UNIX) {
        candidate = era0;
        matches++;
    }
    if (era1 >= CN_TIME_RELEASE_MIN_UNIX &&
        era1 <= CN_TIME_TARGET_MAX_UNIX) {
        candidate = era1;
        matches++;
    }
    if (matches != 1 || rtt_ns < 0)
        return CN_TIME_TIME_RANGE;

    nanoseconds = (int64_t)(((uint64_t)fraction * UINT64_C(1000000000)) >>
                            32);
    nanoseconds += rtt_ns / 2;
    candidate += nanoseconds / INT64_C(1000000000);
    nanoseconds %= INT64_C(1000000000);
    if (candidate < CN_TIME_RELEASE_MIN_UNIX ||
        candidate > CN_TIME_TARGET_MAX_UNIX)
        return CN_TIME_TIME_RANGE;

    sample->unix_seconds = (time_t)candidate;
    sample->nanoseconds = (long)nanoseconds;
    sample->rtt_ms = (unsigned)((rtt_ns + INT64_C(999999)) /
                                INT64_C(1000000));
    return CN_TIME_OK;
}

static cn_time_result validate_response(
    const unsigned char *response, size_t response_len,
    const unsigned char request[CN_SNTP_PACKET_BYTES], int64_t rtt_ns,
    cn_time_sample *sample)
{
    unsigned leap;
    unsigned version;
    unsigned mode;
    unsigned stratum;

    if (response_len != CN_SNTP_PACKET_BYTES)
        return CN_TIME_PACKET_SIZE;

    leap = response[0] >> 6;
    version = (response[0] >> 3) & 7U;
    mode = response[0] & 7U;
    stratum = response[1];
    if (version != 4)
        return CN_TIME_VERSION;
    if (mode != 4)
        return CN_TIME_MODE;
    if (leap == 3)
        return CN_TIME_UNSYNCHRONIZED;
    if (stratum == 0)
        return CN_TIME_KISS_OF_DEATH;
    if (stratum > 15)
        return CN_TIME_STRATUM;
    if (memcmp(response + 24, request + 40, 8) != 0)
        return CN_TIME_ORIGINATE_MISMATCH;
    if (!timestamp_nonzero(response + 32))
        return CN_TIME_ZERO_RECEIVE;
    if (!timestamp_nonzero(response + 40))
        return CN_TIME_ZERO_TRANSMIT;
    return decode_transmit_time(response + 40, rtt_ns, sample);
}

static cn_time_result query_server(const char *server, unsigned port,
                                   unsigned timeout_ms,
                                   cn_time_sample *sample)
{
    unsigned char request[CN_SNTP_PACKET_BYTES];
    unsigned char response[CN_SNTP_RESPONSE_CAP];
    struct sockaddr_in address;
    struct sockaddr_in peer;
    socklen_t peer_len;
    int64_t attempt_start;
    int64_t deadline;
    int64_t send_time = 0;
    int64_t receive_time;
    int flags;
    int fd = -1;
    cn_time_result result;

    memset(&address, 0, sizeof address);
    address.sin_family = AF_INET;
    address.sin_port = htons((unsigned short)port);
    if (!server || inet_pton(AF_INET, server, &address.sin_addr) != 1)
        return CN_TIME_INVALID;

    result = build_request(request);
    if (result != CN_TIME_OK)
        return result;
    if (!monotonic_ns(&attempt_start))
        return CN_TIME_INTERNAL;
    deadline = attempt_start + (int64_t)timeout_ms * INT64_C(1000000);

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return CN_TIME_SOCKET_FAILED;
    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        result = CN_TIME_SOCKET_FAILED;
        goto done;
    }

    while (1) {
        ssize_t sent;

        if (!monotonic_ns(&send_time)) {
            result = CN_TIME_INTERNAL;
            goto done;
        }
        sent = sendto(fd, request, sizeof request, 0,
                      (const struct sockaddr *)&address, sizeof address);
        if (sent == (ssize_t)sizeof request)
            break;
        if (sent < 0 && errno == EINTR)
            continue;
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            int ready = wait_for_socket(fd, POLLOUT, deadline);

            if (ready == 0) {
                result = CN_TIME_TIMEOUT;
                goto done;
            }
            if (ready < 0) {
                result = CN_TIME_SEND_FAILED;
                goto done;
            }
            continue;
        }
        result = CN_TIME_SEND_FAILED;
        goto done;
    }

    while (1) {
        ssize_t got;
        int ready = wait_for_socket(fd, POLLIN, deadline);

        if (ready == 0) {
            result = CN_TIME_TIMEOUT;
            goto done;
        }
        if (ready < 0) {
            result = CN_TIME_RECV_FAILED;
            goto done;
        }
        memset(&peer, 0, sizeof peer);
        peer_len = sizeof peer;
        got = recvfrom(fd, response, sizeof response, 0,
                       (struct sockaddr *)&peer, &peer_len);
        if (got < 0 && (errno == EINTR || errno == EAGAIN ||
                        errno == EWOULDBLOCK))
            continue;
        if (got < 0) {
            result = CN_TIME_RECV_FAILED;
            goto done;
        }
        if (peer_len != sizeof peer || peer.sin_family != AF_INET ||
            peer.sin_port != address.sin_port ||
            peer.sin_addr.s_addr != address.sin_addr.s_addr) {
            result = CN_TIME_PEER_MISMATCH;
            goto done;
        }
        if (!monotonic_ns(&receive_time)) {
            result = CN_TIME_INTERNAL;
            goto done;
        }
        result = validate_response(response, (size_t)got, request,
                                   receive_time - send_time, sample);
        goto done;
    }

done:
    close(fd);
    return result;
}

cn_time_result cn_timesimple_query(const cn_time_config *config,
                                   cn_time_sample *sample)
{
    cn_time_result last = CN_TIME_INVALID;
    unsigned port;
    unsigned timeout_ms;
    size_t i;

    if (!config || !sample || config->server_count == 0 ||
        config->server_count > CN_TIME_MAX_SERVERS)
        return CN_TIME_INVALID;
    port = config->port ? config->port : CN_TIME_DEFAULT_PORT;
    timeout_ms = config->timeout_ms ? config->timeout_ms
                                    : CN_TIME_DEFAULT_TIMEOUT_MS;
    if (port > 65535)
        return CN_TIME_INVALID;

    memset(sample, 0, sizeof *sample);
    for (i = 0; i < config->server_count; i++) {
        last = query_server(config->servers[i], port, timeout_ms, sample);
        if (last == CN_TIME_OK) {
            sample->server_index = i;
            return CN_TIME_OK;
        }
        if (last == CN_TIME_ENTROPY_FAILED || last == CN_TIME_INTERNAL)
            return last;
    }
    return last;
}

cn_time_result cn_timesimple_sync(const cn_time_config *config,
                                  cn_time_sample *sample)
{
    cn_time_sample local;
    struct timespec target;
    cn_time_result result;

    result = cn_timesimple_query(config, &local);
    if (result != CN_TIME_OK)
        return result;
    target.tv_sec = local.unix_seconds;
    target.tv_nsec = local.nanoseconds;
    if (clock_settime(CLOCK_REALTIME, &target) != 0)
        return CN_TIME_CLOCK_SET_FAILED;
    if (sample)
        *sample = local;
    return CN_TIME_OK;
}

const char *cn_timesimple_result_name(cn_time_result result)
{
    static const char *const names[CN_TIME_RESULT_COUNT] = {
        "ok", "invalid", "internal", "entropy-failed", "socket-failed",
        "send-failed", "timeout", "recv-failed", "peer-mismatch",
        "packet-size", "version", "mode", "unsynchronized",
        "kiss-of-death", "stratum", "originate-mismatch", "zero-receive",
        "zero-transmit", "time-range", "clock-set-failed"
    };

    if (result < 0 || result >= CN_TIME_RESULT_COUNT)
        return "unknown";
    return names[result];
}

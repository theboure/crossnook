/* Bounded SNTPv4 bootstrap for establishing CLOCK_REALTIME. */
#ifndef CN_TIME_TIMESIMPLE_H
#define CN_TIME_TIMESIMPLE_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define CN_TIME_MAX_SERVERS 4
#define CN_TIME_DEFAULT_PORT 123
#define CN_TIME_DEFAULT_TIMEOUT_MS 3000

/* Published HTTPS baseline commit time: 2026-09-20T12:42:27Z. */
#define CN_TIME_RELEASE_MIN_UNIX INT64_C(1789908147)
/* Linux 2.6.29 ARM uses the signed 32-bit kernel wall-clock ABI. */
#define CN_TIME_TARGET_MAX_UNIX INT64_C(2147483647)

typedef enum cn_time_result {
    CN_TIME_OK = 0,
    CN_TIME_INVALID,
    CN_TIME_INTERNAL,
    CN_TIME_ENTROPY_FAILED,
    CN_TIME_SOCKET_FAILED,
    CN_TIME_SEND_FAILED,
    CN_TIME_TIMEOUT,
    CN_TIME_RECV_FAILED,
    CN_TIME_PEER_MISMATCH,
    CN_TIME_PACKET_SIZE,
    CN_TIME_VERSION,
    CN_TIME_MODE,
    CN_TIME_UNSYNCHRONIZED,
    CN_TIME_KISS_OF_DEATH,
    CN_TIME_STRATUM,
    CN_TIME_ORIGINATE_MISMATCH,
    CN_TIME_ZERO_RECEIVE,
    CN_TIME_ZERO_TRANSMIT,
    CN_TIME_TIME_RANGE,
    CN_TIME_CLOCK_SET_FAILED,
    CN_TIME_RESULT_COUNT
} cn_time_result;

typedef struct cn_time_config {
    const char *servers[CN_TIME_MAX_SERVERS];
    size_t server_count;
    unsigned port;       /* zero selects CN_TIME_DEFAULT_PORT */
    unsigned timeout_ms; /* zero selects CN_TIME_DEFAULT_TIMEOUT_MS */
} cn_time_config;

typedef struct cn_time_sample {
    time_t unix_seconds;
    long nanoseconds;
    unsigned rtt_ms;
    size_t server_index;
} cn_time_sample;

/* Query and validate without changing CLOCK_REALTIME. */
cn_time_result cn_timesimple_query(const cn_time_config *config,
                                   cn_time_sample *sample);

/* Success means query validation and clock_settime(CLOCK_REALTIME) succeeded. */
cn_time_result cn_timesimple_sync(const cn_time_config *config,
                                  cn_time_sample *sample);

const char *cn_timesimple_result_name(cn_time_result result);

#endif /* CN_TIME_TIMESIMPLE_H */

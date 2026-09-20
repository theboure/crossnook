#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "time/timesimple.h"

static int parse_unsigned(const char *text, unsigned max, unsigned *out)
{
    char *end;
    unsigned long value;

    if (!text || !text[0] || !out)
        return 0;
    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || *end != '\0' || value == 0 || value > max)
        return 0;
    *out = (unsigned)value;
    return 1;
}

static void usage(const char *program)
{
    fprintf(stderr,
            "usage: %s --query|--sync PORT TIMEOUT_MS IPV4 [IPV4 ...]\n",
            program);
}

int main(int argc, char **argv)
{
    cn_time_config config;
    cn_time_sample sample;
    cn_time_result result;
    int apply;
    int i;

    if (argc < 5 || argc > 4 + CN_TIME_MAX_SERVERS) {
        usage(argv[0]);
        return 2;
    }
    if (strcmp(argv[1], "--query") == 0)
        apply = 0;
    else if (strcmp(argv[1], "--sync") == 0)
        apply = 1;
    else {
        usage(argv[0]);
        return 2;
    }

    memset(&config, 0, sizeof config);
    if (!parse_unsigned(argv[2], 65535, &config.port) ||
        !parse_unsigned(argv[3], 60000, &config.timeout_ms)) {
        usage(argv[0]);
        return 2;
    }
    config.server_count = (size_t)(argc - 4);
    for (i = 4; i < argc; i++)
        config.servers[i - 4] = argv[i];

    result = apply ? cn_timesimple_sync(&config, &sample)
                   : cn_timesimple_query(&config, &sample);
    if (result != CN_TIME_OK) {
        printf("TIMETEST %s FAIL %s\n", apply ? "SYNC" : "QUERY",
               cn_timesimple_result_name(result));
        return 1;
    }
    printf("TIMETEST %s OK unix=%lld nsec=%ld rtt_ms=%u server=%u\n",
           apply ? "SYNC" : "QUERY", (long long)sample.unix_seconds,
           sample.nanoseconds, sample.rtt_ms,
           (unsigned)sample.server_index);
    return 0;
}

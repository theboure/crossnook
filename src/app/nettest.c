/* CLI and self-test driver for the minimal networking client. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include "net/netsimple.h"

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
             (unsigned)ntohs(bound.sin_port));

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
        size_t n = strlen(parsed.status_text);
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

static void print_usage(void)
{
    fprintf(stderr,
            "usage: crossnook-net-test <host> <port> <path>\n"
            "       crossnook-net-test --api-smoke\n"
            "       crossnook-net-test --self-test\n"
            "       crossnook-net-test --self-refused\n"
            "       crossnook-net-test --self-timeout\n"
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
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        print_usage();
        return 0;
    }
    if (argc == 4)
        return run_live(argv[1], argv[2], argv[3]);
    print_usage();
    return 2;
}

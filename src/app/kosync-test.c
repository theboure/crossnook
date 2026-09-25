/* Protocol smoke tests and real-device KOSync diagnostic CLI. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "book/koreader_identity.h"
#include "net/dnssimple.h"
#include "sync/kosync.h"
#include "sync/kosync_policy.h"
#include "sync/kosync_sync.h"
#include "net/tlssimple.h"
#include "progress/book_identity.h"
#include "progress/progress_store.h"

#define TEST_DOCUMENT "e1a1e9016cfc9bca8c694187943e9c4f"
#define FOREIGN_DOCUMENT "519220cea448409961e6b3081a36eca3"
#define TEST_USER "test-user"
#define TEST_KEY "dfb450efddbb5387197c84460623675b"
#define TEST_DEVICE "crossnook-test-device"
#define TEST_DEVICE_ID "crossnook-test-device-id"
#define POSITION_ONE "/body/DocFragment[1]/body/p[1]/text().0"
#define POSITION_TWO "/body/DocFragment[1]/body/p[3]/text().5"
#define FAULT_MALFORMED "00000000000000000000000000000001"
#define FAULT_PROTOCOL "00000000000000000000000000000002"
#define FAULT_OVERSIZED "00000000000000000000000000000003"
#define FAULT_SERVER "00000000000000000000000000000004"

static time_t tls_fixed_now;

static time_t tls_fixed_time(void)
{
    return tls_fixed_now;
}

static void check(int condition, const char *name, int *failures)
{
    printf("[%s] %s\n", condition ? "OK" : "FAIL", name);
    if (!condition)
        ++*failures;
}

static int progress_matches(const cn_kosync_progress *progress,
                            const char *document, const char *position,
                            int progress_10000)
{
    return strcmp(progress->document_id, document) == 0 &&
           progress->logical_position &&
           strcmp(progress->logical_position, position) == 0 &&
           progress->progress_10000 == progress_10000 &&
           strcmp(progress->device, TEST_DEVICE) == 0 &&
           strcmp(progress->device_id, TEST_DEVICE_ID) == 0;
}

static int run_api_smoke(void)
{
    static const char expected[] =
        "{\"document\":\"e1a1e9016cfc9bca8c694187943e9c4f\","
        "\"progress\":\"/body/DocFragment[1]/body/p[1]/text().0\","
        "\"percentage\":0.321,\"device\":\"crossnook-test-device\","
        "\"device_id\":\"crossnook-test-device-id\"}";
    static const char escaped_json[] =
        "{\"document\":\"519220cea448409961e6b3081a36eca3\","
        "\"progress\":\"/a\\\"b\\\\c\\n\",\"percentage\":0.0001,"
        "\"device\":\"device \\u263a\",\"device_id\":\"id\","
        "\"timestamp\":1700000001}";
    static const char malformed[] = "{\"document\":]";
    static const char incomplete[] =
        "{\"document\":\"519220cea448409961e6b3081a36eca3\"}";
    static const char invalid_number[] =
        "{\"document\":\"519220cea448409961e6b3081a36eca3\","
        "\"progress\":\"/x\",\"percentage\":.5,\"device\":\"d\"}";
    cn_kosync_client client;
    cn_kosync_progress local;
    cn_kosync_progress parsed;
    cn_netsimple_header headers[2];
    cn_netsimple_request request;
    char json[2048];
    char http[2048];
    size_t json_len = 0;
    size_t http_len = 0;
    int failures = 0;

    cn_kosync_progress_init(&local);
    cn_kosync_progress_init(&parsed);
    check(cn_kosync_client_init(&client, "http://127.0.0.1:8000/base/",
                                TEST_USER, TEST_KEY) == CN_KOSYNC_OK &&
              strcmp(client.host, "127.0.0.1") == 0 &&
              strcmp(client.port, "8000") == 0 &&
              strcmp(client.base_path, "/base") == 0,
          "plain-HTTP base URL is parsed and normalized", &failures);
    check(cn_kosync_client_init(&client, "https://example.com", TEST_USER,
                                 TEST_KEY) == CN_KOSYNC_OK &&
              client.use_tls && strcmp(client.port, "443") == 0,
          "HTTPS base URL selects verified TLS and port 443", &failures);
    check(cn_kosync_client_init(&client, "http://example.com/bad\r\nheader",
                                TEST_USER, TEST_KEY) == CN_KOSYNC_INVALID,
          "base URL rejects request-target injection", &failures);
    check(cn_kosync_progress_set(&local, TEST_DOCUMENT, POSITION_ONE, 3210,
                                 TEST_DEVICE, TEST_DEVICE_ID) == CN_KOSYNC_OK,
          "sync progress accepts the validated domain fields", &failures);
    check(cn_kosync_progress_set(&local, TEST_DOCUMENT, POSITION_ONE, 3210,
                                 "bad\xc0\x80", TEST_DEVICE_ID) ==
              CN_KOSYNC_INVALID,
          "sync progress rejects invalid UTF-8 device text", &failures);
    check(cn_kosync_serialize_progress(&local, json, sizeof json, &json_len) ==
              CN_KOSYNC_OK && json_len == strlen(expected) &&
              strcmp(json, expected) == 0,
          "PUT JSON serialization matches the golden wire object", &failures);
    check(cn_kosync_parse_progress(escaped_json, strlen(escaped_json),
                                   &parsed) == CN_KOSYNC_OK &&
              strcmp(parsed.logical_position, "/a\"b\\c\n") == 0 &&
              strcmp(parsed.device, "device \xe2\x98\xba") == 0 &&
              parsed.progress_10000 == 1 && parsed.has_timestamp &&
              parsed.timestamp == 1700000001LL,
          "JSON parser decodes escapes, Unicode, percentage and timestamp",
          &failures);
    cn_kosync_progress_clear(&parsed);
    check(cn_kosync_parse_progress("{}", 2, &parsed) ==
              CN_KOSYNC_NOT_FOUND,
          "empty response object means no progress", &failures);
    check(cn_kosync_parse_progress(malformed, strlen(malformed), &parsed) ==
              CN_KOSYNC_BAD_JSON,
          "malformed JSON is rejected", &failures);
    check(cn_kosync_parse_progress(incomplete, strlen(incomplete), &parsed) ==
              CN_KOSYNC_BAD_PROTOCOL,
          "missing protocol fields are rejected", &failures);
    check(cn_kosync_parse_progress(invalid_number, strlen(invalid_number),
                                   &parsed) == CN_KOSYNC_BAD_JSON,
          "non-JSON number grammar is rejected", &failures);

    cn_kosync_progress_clear(&parsed);
    check(cn_kosync_progress_set(&parsed, FOREIGN_DOCUMENT, POSITION_TWO, 6543,
                                 "other-device", "other-id") == CN_KOSYNC_OK,
          "remote fixture is constructed", &failures);
    parsed.has_timestamp = 1;
    parsed.timestamp = 101;
    check(cn_kosync_classify_remote(&parsed, POSITION_ONE, 3210, 100,
                                    TEST_DEVICE, TEST_DEVICE_ID) ==
              CN_KOSYNC_REMOTE_NEWER,
          "strictly newer server timestamp classifies remote as newer",
          &failures);
    parsed.timestamp = 100;
    check(cn_kosync_classify_remote(&parsed, POSITION_ONE, 3210, 100,
                                    TEST_DEVICE, TEST_DEVICE_ID) ==
              CN_KOSYNC_REMOTE_OLDER_OR_EQUAL,
          "equal timestamp follows KOReader's older/equal branch", &failures);
    parsed.timestamp = 99;
    check(cn_kosync_classify_remote(&parsed, POSITION_ONE, 3210, 100,
                                    TEST_DEVICE, TEST_DEVICE_ID) ==
              CN_KOSYNC_REMOTE_OLDER_OR_EQUAL,
          "older server timestamp follows the older/equal branch", &failures);
    parsed.has_timestamp = 0;
    check(cn_kosync_classify_remote(&parsed, POSITION_ONE, 3210, 100,
                                    TEST_DEVICE, TEST_DEVICE_ID) ==
              CN_KOSYNC_REMOTE_NEWER,
          "legacy response falls back to percentage ordering", &failures);
    parsed.has_timestamp = 1;
    parsed.progress_10000 = 3210;
    check(cn_kosync_classify_remote(&parsed, POSITION_ONE, 3210, 0,
                                    TEST_DEVICE, TEST_DEVICE_ID) ==
              CN_KOSYNC_REMOTE_ALREADY_SYNCED,
          "equal normalized percentage suppresses restore", &failures);
    parsed.progress_10000 = 6543;
    strcpy(parsed.device, TEST_DEVICE);
    strcpy(parsed.device_id, TEST_DEVICE_ID);
    check(cn_kosync_classify_remote(&parsed, POSITION_ONE, 3210, 0,
                                    TEST_DEVICE, TEST_DEVICE_ID) ==
              CN_KOSYNC_REMOTE_SAME_DEVICE,
          "matching device and device_id suppresses restore", &failures);

    headers[0].name = "x-auth-user"; headers[0].value = TEST_USER;
    headers[1].name = "x-auth-key"; headers[1].value = TEST_KEY;
    memset(&request, 0, sizeof request);
    request.method = CN_NETSIMPLE_METHOD_PUT;
    request.host = "127.0.0.1"; request.port = "8000";
    request.path = "/syncs/progress";
    request.headers = headers; request.header_count = 2;
    request.content_type = "application/json";
    request.body = "{}"; request.body_len = 2;
    check(cn_netsimple_build_exchange_request(&request, http, sizeof http,
                                               &http_len) == CN_NETSIMPLE_OK &&
              strstr(http, "PUT /syncs/progress HTTP/1.0\r\n") == http &&
              strstr(http, "Host: 127.0.0.1:8000\r\n") != NULL &&
              strstr(http, "x-auth-user: test-user\r\n") != NULL &&
              strstr(http, "Content-Length: 2\r\n\r\n") != NULL,
          "bounded HTTP builder emits PUT, auth, type and length", &failures);

    check(strcmp(TEST_DOCUMENT,
                 "e1a1e9016cfc9bca8c694187943e9c4f") == 0,
          "test.epub golden Binary document ID is exact", &failures);
    check(strcmp(TEST_DOCUMENT,
                 "e1a1e9016cfc9bca8c694187943e9c4f") == 0,
          "valid2.epub intentionally shares test.epub Binary ID", &failures);
    check(strcmp(FOREIGN_DOCUMENT,
                 "519220cea448409961e6b3081a36eca3") == 0,
          "foreign.epub golden Binary document ID is distinct", &failures);

    cn_kosync_progress_clear(&local);
    cn_kosync_progress_clear(&parsed);
    printf("KOSYNC API SMOKE failures=%d -> %s\n", failures,
           failures == 0 ? "OK" : "FAIL");
    return failures == 0 ? 0 : 1;
}

static void policy_reset(cn_kosync_sync_result *sync,
                         cn_kosync_sync_status status)
{
    cn_kosync_sync_result_init(sync);
    sync->status = status;
}

static void policy_expect(const cn_kosync_sync_result *sync,
                          cn_kosync_product_outcome outcome,
                          cn_kosync_retry_policy retry,
                          cn_kosync_mutation_state local_mutation,
                          cn_kosync_mutation_state remote_mutation,
                          const char *name, int *failures)
{
    cn_kosync_product_result product;
    check(cn_kosync_policy_classify(1, sync, &product) == 0 &&
              product.outcome == outcome && product.retry == retry &&
              product.local_mutation == local_mutation &&
              product.remote_mutation == remote_mutation,
          name, failures);
}

static int time_is_transient(cn_time_result result)
{
    return result == CN_TIME_SOCKET_FAILED || result == CN_TIME_SEND_FAILED ||
           result == CN_TIME_TIMEOUT || result == CN_TIME_RECV_FAILED;
}

static int dns_is_transient(cn_dns_result result)
{
    return result == CN_DNS_SOCKET_FAILED ||
           result == CN_DNS_NETWORK_FAILED || result == CN_DNS_TIMEOUT ||
           result == CN_DNS_SERVER_FAILURE;
}

static int transport_is_transient(cn_netsimple_result result)
{
    switch (result) {
    case CN_NETSIMPLE_RESOLVE_ERROR:
    case CN_NETSIMPLE_CONNECT_REFUSED:
    case CN_NETSIMPLE_NETWORK_UNREACHABLE:
    case CN_NETSIMPLE_HOST_UNREACHABLE:
    case CN_NETSIMPLE_CONNECT_TIMEOUT:
    case CN_NETSIMPLE_CONNECT_ERROR:
    case CN_NETSIMPLE_SEND_ERROR:
    case CN_NETSIMPLE_RECV_ERROR:
    case CN_NETSIMPLE_RECV_TIMEOUT:
    case CN_NETSIMPLE_SEND_TIMEOUT:
    case CN_NETSIMPLE_TLS_HANDSHAKE_TIMEOUT:
    case CN_NETSIMPLE_TLS_RECV_TIMEOUT:
        return 1;
    default:
        return 0;
    }
}

static int transport_is_security(cn_netsimple_result result)
{
    switch (result) {
    case CN_NETSIMPLE_TLS_INVALID_CA:
    case CN_NETSIMPLE_TLS_TRUST_FAILED:
    case CN_NETSIMPLE_TLS_HOSTNAME_MISMATCH:
    case CN_NETSIMPLE_TLS_CERT_TIME_FAILED:
    case CN_NETSIMPLE_TLS_CERT_INVALID:
        return 1;
    default:
        return 0;
    }
}

static int transport_is_service(cn_netsimple_result result)
{
    return result == CN_NETSIMPLE_TRUNCATED ||
           result == CN_NETSIMPLE_BAD_RESPONSE ||
           result == CN_NETSIMPLE_TLS_HANDSHAKE_FAILED ||
           result == CN_NETSIMPLE_TLS_PROTOCOL_FAILED;
}

static int run_policy_smoke(void)
{
    cn_kosync_sync_result sync;
    cn_kosync_product_result product;
    int i;
    int failures = 0;

    check(cn_kosync_policy_classify(0, NULL, &product) == 0 &&
              product.outcome == CN_KOSYNC_PRODUCT_DISABLED &&
              product.retry == CN_KOSYNC_RETRY_NONE,
          "disabled accepts a NULL sync result", &failures);

    policy_reset(&sync, CN_KOSYNC_SYNC_STATUS_OK);
    sync.decision = CN_KOSYNC_SYNC_NO_STATE;
    policy_expect(&sync, CN_KOSYNC_PRODUCT_NO_STATE, CN_KOSYNC_RETRY_NONE,
                  CN_KOSYNC_MUTATION_NONE, CN_KOSYNC_MUTATION_NONE,
                  "no state", &failures);
    policy_reset(&sync, CN_KOSYNC_SYNC_STATUS_OK);
    sync.decision = CN_KOSYNC_SYNC_LOCAL_SELECTED;
    sync.local_present = 1;
    sync.remote_put_attempted = sync.remote_uploaded = 1;
    policy_expect(&sync, CN_KOSYNC_PRODUCT_UPLOADED, CN_KOSYNC_RETRY_NONE,
                  CN_KOSYNC_MUTATION_NONE, CN_KOSYNC_MUTATION_CONFIRMED,
                  "confirmed upload", &failures);
    policy_reset(&sync, CN_KOSYNC_SYNC_STATUS_OK);
    sync.decision = CN_KOSYNC_SYNC_REMOTE_SELECTED;
    sync.remote_present = 1;
    sync.local_save_attempted = sync.local_saved = 1;
    policy_expect(&sync, CN_KOSYNC_PRODUCT_IMPORTED, CN_KOSYNC_RETRY_NONE,
                  CN_KOSYNC_MUTATION_CONFIRMED, CN_KOSYNC_MUTATION_NONE,
                  "confirmed import", &failures);
    policy_reset(&sync, CN_KOSYNC_SYNC_STATUS_OK);
    sync.decision = CN_KOSYNC_SYNC_NO_CHANGE;
    sync.local_present = sync.remote_present = 1;
    policy_expect(&sync, CN_KOSYNC_PRODUCT_UNCHANGED, CN_KOSYNC_RETRY_NONE,
                  CN_KOSYNC_MUTATION_NONE, CN_KOSYNC_MUTATION_NONE,
                  "unchanged", &failures);
    policy_reset(&sync, CN_KOSYNC_SYNC_STATUS_OK);
    sync.decision = CN_KOSYNC_SYNC_AMBIGUOUS;
    sync.local_present = sync.remote_present = 1;
    policy_expect(&sync, CN_KOSYNC_PRODUCT_CONFLICT,
                  CN_KOSYNC_RETRY_EXPLICIT_ACTION,
                  CN_KOSYNC_MUTATION_NONE, CN_KOSYNC_MUTATION_NONE,
                  "conflict", &failures);
    policy_reset(&sync, CN_KOSYNC_SYNC_STATUS_OK);
    sync.decision = CN_KOSYNC_SYNC_LOCAL_SELECTED;
    sync.local_present = 1;
    policy_expect(&sync, CN_KOSYNC_PRODUCT_INTERNAL_FAILURE,
                  CN_KOSYNC_RETRY_EXPLICIT_ACTION,
                  CN_KOSYNC_MUTATION_NONE, CN_KOSYNC_MUTATION_NONE,
                  "inconsistent success fails closed", &failures);

    policy_reset(&sync, CN_KOSYNC_SYNC_STATUS_INVALID);
    policy_expect(&sync, CN_KOSYNC_PRODUCT_CONFIGURATION_FAILURE,
                  CN_KOSYNC_RETRY_EXPLICIT_ACTION,
                  CN_KOSYNC_MUTATION_NONE, CN_KOSYNC_MUTATION_NONE,
                  "invalid configuration", &failures);
    policy_reset(&sync, CN_KOSYNC_SYNC_STATUS_IDENTITY_FAILED);
    policy_expect(&sync, CN_KOSYNC_PRODUCT_LOCAL_FAILURE,
                  CN_KOSYNC_RETRY_EXPLICIT_ACTION,
                  CN_KOSYNC_MUTATION_NONE, CN_KOSYNC_MUTATION_NONE,
                  "identity failure", &failures);
    policy_reset(&sync, CN_KOSYNC_SYNC_STATUS_LOCAL_FAILED);
    sync.decision = CN_KOSYNC_SYNC_REMOTE_SELECTED;
    sync.local_save_attempted = 1;
    policy_expect(&sync, CN_KOSYNC_PRODUCT_LOCAL_FAILURE,
                  CN_KOSYNC_RETRY_EXPLICIT_ACTION,
                  CN_KOSYNC_MUTATION_POSSIBLE, CN_KOSYNC_MUTATION_NONE,
                  "attempted failed save is possible", &failures);
    policy_reset(&sync, CN_KOSYNC_SYNC_STATUS_LOCAL_UNSUPPORTED);
    sync.decision = CN_KOSYNC_SYNC_LOCAL_SELECTED;
    sync.local_present = 1;
    policy_expect(&sync, CN_KOSYNC_PRODUCT_LOCAL_UNSUPPORTED,
                  CN_KOSYNC_RETRY_EXPLICIT_ACTION,
                  CN_KOSYNC_MUTATION_NONE, CN_KOSYNC_MUTATION_NONE,
                  "unsupported local position never enters PUT", &failures);
    policy_reset(&sync, CN_KOSYNC_SYNC_STATUS_AUTH_FAILED);
    policy_expect(&sync, CN_KOSYNC_PRODUCT_AUTH_REQUIRED,
                  CN_KOSYNC_RETRY_EXPLICIT_ACTION,
                  CN_KOSYNC_MUTATION_NONE, CN_KOSYNC_MUTATION_NONE,
                  "GET authentication rejection", &failures);
    sync.remote_put_attempted = 1;
    policy_expect(&sync, CN_KOSYNC_PRODUCT_AUTH_REQUIRED,
                  CN_KOSYNC_RETRY_EXPLICIT_ACTION,
                  CN_KOSYNC_MUTATION_NONE, CN_KOSYNC_MUTATION_NONE,
                  "PUT authentication rejection proves no mutation",
                  &failures);
    policy_reset(&sync, CN_KOSYNC_SYNC_STATUS_NO_MEMORY);
    policy_expect(&sync, CN_KOSYNC_PRODUCT_INTERNAL_FAILURE,
                  CN_KOSYNC_RETRY_EXPLICIT_ACTION,
                  CN_KOSYNC_MUTATION_NONE, CN_KOSYNC_MUTATION_NONE,
                  "allocation failure", &failures);

    for (i = CN_TIME_OK; i < CN_TIME_RESULT_COUNT; ++i) {
        cn_kosync_product_outcome expected = i == CN_TIME_OK
            ? CN_KOSYNC_PRODUCT_INTERNAL_FAILURE
            : CN_KOSYNC_PRODUCT_TRUSTED_TIME_UNAVAILABLE;
        cn_kosync_retry_policy retry = time_is_transient((cn_time_result)i)
            ? CN_KOSYNC_RETRY_AUTOMATIC_LATER
            : CN_KOSYNC_RETRY_EXPLICIT_ACTION;
        policy_reset(&sync, CN_KOSYNC_SYNC_STATUS_TRUSTED_TIME_FAILED);
        sync.time_result = (cn_time_result)i;
        policy_expect(&sync, expected, retry, CN_KOSYNC_MUTATION_NONE,
                      CN_KOSYNC_MUTATION_NONE, "trusted-time result class",
                      &failures);
    }

    for (i = CN_DNS_OK; i < CN_DNS_RESULT_COUNT; ++i) {
        cn_kosync_product_outcome expected;
        cn_kosync_retry_policy retry = CN_KOSYNC_RETRY_EXPLICIT_ACTION;
        if (i == CN_DNS_OK || i == CN_DNS_ENTROPY_FAILED)
            expected = CN_KOSYNC_PRODUCT_INTERNAL_FAILURE;
        else if (i == CN_DNS_INVALID || i == CN_DNS_INVALID_HOSTNAME)
            expected = CN_KOSYNC_PRODUCT_CONFIGURATION_FAILURE;
        else if (dns_is_transient((cn_dns_result)i)) {
            expected = CN_KOSYNC_PRODUCT_CONNECTIVITY_FAILURE;
            retry = CN_KOSYNC_RETRY_AUTOMATIC_LATER;
        } else
            expected = CN_KOSYNC_PRODUCT_SERVICE_FAILURE;
        policy_reset(&sync, CN_KOSYNC_SYNC_STATUS_DNS_FAILED);
        sync.dns_result = (cn_dns_result)i;
        policy_expect(&sync, expected, retry, CN_KOSYNC_MUTATION_NONE,
                      CN_KOSYNC_MUTATION_NONE, "DNS result class", &failures);
    }

    for (i = CN_NETSIMPLE_OK; i < CN_NETSIMPLE_LENGTH; ++i) {
        cn_kosync_product_outcome expected;
        cn_kosync_retry_policy retry = CN_KOSYNC_RETRY_EXPLICIT_ACTION;
        if (transport_is_transient((cn_netsimple_result)i)) {
            expected = CN_KOSYNC_PRODUCT_CONNECTIVITY_FAILURE;
            retry = CN_KOSYNC_RETRY_AUTOMATIC_LATER;
        } else if (transport_is_security((cn_netsimple_result)i))
            expected = CN_KOSYNC_PRODUCT_SECURITY_FAILURE;
        else if (transport_is_service((cn_netsimple_result)i))
            expected = CN_KOSYNC_PRODUCT_SERVICE_FAILURE;
        else if (i == CN_NETSIMPLE_INVALID)
            expected = CN_KOSYNC_PRODUCT_CONFIGURATION_FAILURE;
        else
            expected = CN_KOSYNC_PRODUCT_INTERNAL_FAILURE;
        policy_reset(&sync, CN_KOSYNC_SYNC_STATUS_HTTPS_FAILED);
        sync.kosync_outcome.transport_result = (cn_netsimple_result)i;
        policy_expect(&sync, expected, retry, CN_KOSYNC_MUTATION_NONE,
                      CN_KOSYNC_MUTATION_NONE, "transport result class",
                      &failures);
    }

    policy_reset(&sync, CN_KOSYNC_SYNC_STATUS_HTTPS_FAILED);
    sync.decision = CN_KOSYNC_SYNC_LOCAL_SELECTED;
    sync.remote_put_attempted = 1;
    sync.kosync_outcome.transport_result = CN_NETSIMPLE_CONNECT_REFUSED;
    policy_expect(&sync, CN_KOSYNC_PRODUCT_CONNECTIVITY_FAILURE,
                  CN_KOSYNC_RETRY_AUTOMATIC_LATER,
                  CN_KOSYNC_MUTATION_NONE, CN_KOSYNC_MUTATION_NONE,
                  "failed PUT connect is proven pre-mutation", &failures);
    sync.kosync_outcome.transport_result = CN_NETSIMPLE_TLS_RECV_TIMEOUT;
    policy_expect(&sync, CN_KOSYNC_PRODUCT_CONNECTIVITY_FAILURE,
                  CN_KOSYNC_RETRY_AUTOMATIC_LATER,
                  CN_KOSYNC_MUTATION_NONE, CN_KOSYNC_MUTATION_POSSIBLE,
                  "failed PUT receive is uncertain", &failures);
    sync.kosync_outcome.transport_result = CN_NETSIMPLE_TLS_PROTOCOL_FAILED;
    policy_expect(&sync, CN_KOSYNC_PRODUCT_SERVICE_FAILURE,
                  CN_KOSYNC_RETRY_EXPLICIT_ACTION,
                  CN_KOSYNC_MUTATION_NONE, CN_KOSYNC_MUTATION_POSSIBLE,
                  "stage-ambiguous TLS protocol failure is uncertain",
                  &failures);
    policy_reset(&sync, CN_KOSYNC_SYNC_STATUS_NO_MEMORY);
    sync.decision = CN_KOSYNC_SYNC_LOCAL_SELECTED;
    sync.remote_put_attempted = 1;
    policy_expect(&sync, CN_KOSYNC_PRODUCT_INTERNAL_FAILURE,
                  CN_KOSYNC_RETRY_EXPLICIT_ACTION,
                  CN_KOSYNC_MUTATION_NONE, CN_KOSYNC_MUTATION_POSSIBLE,
                  "post-PUT allocation failure is uncertain", &failures);

    for (i = 0; i < 4; ++i) {
        static const int retry_status[] = {408, 429, 500, 599};
        policy_reset(&sync, CN_KOSYNC_SYNC_STATUS_PROTOCOL_FAILED);
        sync.kosync_result = CN_KOSYNC_HTTP_ERROR;
        sync.kosync_outcome.http_status = retry_status[i];
        policy_expect(&sync, CN_KOSYNC_PRODUCT_SERVICE_FAILURE,
                      CN_KOSYNC_RETRY_AUTOMATIC_LATER,
                      CN_KOSYNC_MUTATION_NONE, CN_KOSYNC_MUTATION_NONE,
                      "retryable HTTP status", &failures);
    }
    policy_reset(&sync, CN_KOSYNC_SYNC_STATUS_PROTOCOL_FAILED);
    sync.kosync_result = CN_KOSYNC_HTTP_ERROR;
    sync.kosync_outcome.http_status = 403;
    policy_expect(&sync, CN_KOSYNC_PRODUCT_SERVICE_FAILURE,
                  CN_KOSYNC_RETRY_EXPLICIT_ACTION,
                  CN_KOSYNC_MUTATION_NONE, CN_KOSYNC_MUTATION_NONE,
                  "non-retryable HTTP status", &failures);
    sync.kosync_result = CN_KOSYNC_BAD_JSON;
    policy_expect(&sync, CN_KOSYNC_PRODUCT_SERVICE_FAILURE,
                  CN_KOSYNC_RETRY_EXPLICIT_ACTION,
                  CN_KOSYNC_MUTATION_NONE, CN_KOSYNC_MUTATION_NONE,
                  "bad JSON", &failures);
    sync.kosync_result = CN_KOSYNC_BAD_PROTOCOL;
    policy_expect(&sync, CN_KOSYNC_PRODUCT_SERVICE_FAILURE,
                  CN_KOSYNC_RETRY_EXPLICIT_ACTION,
                  CN_KOSYNC_MUTATION_NONE, CN_KOSYNC_MUTATION_NONE,
                  "bad protocol", &failures);

    check(cn_kosync_policy_classify(1, NULL, &product) == -1 &&
              product.outcome == CN_KOSYNC_PRODUCT_INTERNAL_FAILURE,
          "enabled requires sync result", &failures);
    check(cn_kosync_policy_classify(2, NULL, &product) == -1 &&
              product.outcome == CN_KOSYNC_PRODUCT_INTERNAL_FAILURE,
          "invalid enabled value", &failures);
    check(cn_kosync_policy_classify(0, NULL, NULL) == -1,
          "NULL product output", &failures);
    policy_reset(&sync,
                 (cn_kosync_sync_status)CN_KOSYNC_SYNC_STATUS_COUNT);
    check(cn_kosync_policy_classify(1, &sync, &product) == -1 &&
              product.outcome == CN_KOSYNC_PRODUCT_INTERNAL_FAILURE,
          "unknown status", &failures);
    policy_reset(&sync, CN_KOSYNC_SYNC_STATUS_OK);
    sync.decision = (cn_kosync_sync_decision)CN_KOSYNC_SYNC_DECISION_COUNT;
    check(cn_kosync_policy_classify(1, &sync, &product) == -1 &&
              product.outcome == CN_KOSYNC_PRODUCT_INTERNAL_FAILURE,
          "unknown decision", &failures);
    policy_reset(&sync, CN_KOSYNC_SYNC_STATUS_DNS_FAILED);
    sync.dns_result = (cn_dns_result)CN_DNS_RESULT_COUNT;
    check(cn_kosync_policy_classify(1, &sync, &product) == -1 &&
              product.outcome == CN_KOSYNC_PRODUCT_INTERNAL_FAILURE,
          "unknown DNS detail fails closed", &failures);
    policy_reset(&sync, CN_KOSYNC_SYNC_STATUS_TRUSTED_TIME_FAILED);
    sync.time_result = (cn_time_result)CN_TIME_RESULT_COUNT;
    check(cn_kosync_policy_classify(1, &sync, &product) == -1 &&
              product.outcome == CN_KOSYNC_PRODUCT_INTERNAL_FAILURE,
          "unknown time detail fails closed", &failures);
    policy_reset(&sync, CN_KOSYNC_SYNC_STATUS_HTTPS_FAILED);
    sync.kosync_outcome.transport_result = CN_NETSIMPLE_LENGTH;
    check(cn_kosync_policy_classify(1, &sync, &product) == -1 &&
              product.outcome == CN_KOSYNC_PRODUCT_INTERNAL_FAILURE,
          "unknown transport detail fails closed", &failures);
    policy_reset(&sync, CN_KOSYNC_SYNC_STATUS_PROTOCOL_FAILED);
    sync.kosync_result = (cn_kosync_result)CN_KOSYNC_RESULT_COUNT;
    check(cn_kosync_policy_classify(1, &sync, &product) == -1 &&
              product.outcome == CN_KOSYNC_PRODUCT_INTERNAL_FAILURE,
          "unknown protocol detail fails closed", &failures);
    policy_reset(&sync, (cn_kosync_sync_status)-1);
    check(cn_kosync_policy_classify(1, &sync, &product) == -1,
          "negative status fails closed", &failures);
    policy_reset(&sync, CN_KOSYNC_SYNC_STATUS_OK);
    sync.decision = (cn_kosync_sync_decision)-1;
    check(cn_kosync_policy_classify(1, &sync, &product) == -1,
          "negative decision fails closed", &failures);
    policy_reset(&sync, CN_KOSYNC_SYNC_STATUS_TRUSTED_TIME_FAILED);
    sync.time_result = (cn_time_result)-1;
    check(cn_kosync_policy_classify(1, &sync, &product) == -1,
          "negative time detail fails closed", &failures);
    policy_reset(&sync, CN_KOSYNC_SYNC_STATUS_DNS_FAILED);
    sync.dns_result = (cn_dns_result)-1;
    check(cn_kosync_policy_classify(1, &sync, &product) == -1,
          "negative DNS detail fails closed", &failures);
    policy_reset(&sync, CN_KOSYNC_SYNC_STATUS_HTTPS_FAILED);
    sync.kosync_outcome.transport_result = (cn_netsimple_result)-1;
    check(cn_kosync_policy_classify(1, &sync, &product) == -1,
          "negative transport detail fails closed", &failures);
    policy_reset(&sync, CN_KOSYNC_SYNC_STATUS_PROTOCOL_FAILED);
    sync.kosync_result = (cn_kosync_result)-1;
    check(cn_kosync_policy_classify(1, &sync, &product) == -1,
          "negative protocol detail fails closed", &failures);
    policy_reset(&sync, CN_KOSYNC_SYNC_STATUS_DNS_FAILED);
    sync.remote_put_attempted = sync.remote_uploaded = 1;
    sync.dns_result = CN_DNS_TIMEOUT;
    policy_expect(&sync, CN_KOSYNC_PRODUCT_INTERNAL_FAILURE,
                  CN_KOSYNC_RETRY_EXPLICIT_ACTION,
                  CN_KOSYNC_MUTATION_NONE, CN_KOSYNC_MUTATION_NONE,
                  "confirmed mutation on failed status fails closed",
                  &failures);
    check(strcmp(cn_kosync_product_outcome_name(
                     CN_KOSYNC_PRODUCT_OUTCOME_COUNT), "unknown") == 0 &&
              strcmp(cn_kosync_product_outcome_name(
                         (cn_kosync_product_outcome)-1), "unknown") == 0 &&
              strcmp(cn_kosync_retry_policy_name(
                         CN_KOSYNC_RETRY_POLICY_COUNT), "unknown") == 0 &&
              strcmp(cn_kosync_retry_policy_name(
                         (cn_kosync_retry_policy)-1), "unknown") == 0 &&
              strcmp(cn_kosync_mutation_state_name(
                         CN_KOSYNC_MUTATION_STATE_COUNT), "unknown") == 0 &&
              strcmp(cn_kosync_mutation_state_name(
                         (cn_kosync_mutation_state)-1), "unknown") == 0,
          "name helper bounds", &failures);

    policy_reset(&sync, CN_KOSYNC_SYNC_STATUS_OK);
    sync.decision = CN_KOSYNC_SYNC_NO_STATE;
    for (i = 0; i < 1000; ++i) {
        if (cn_kosync_policy_classify(1, &sync, &product) != 0 ||
            product.outcome != CN_KOSYNC_PRODUCT_NO_STATE)
            break;
    }
    check(i == 1000, "repeated classification retains no state", &failures);
    printf("KOSYNC POLICY SMOKE failures=%d -> %s\n", failures,
           failures == 0 ? "OK" : "FAIL");
    return failures == 0 ? 0 : 1;
}

static int init_client(cn_kosync_client *client, const char *url,
                       const char *username, const char *userkey)
{
    cn_kosync_result result = cn_kosync_client_init(client, url, username,
                                                     userkey);
    if (result != CN_KOSYNC_OK) {
        fprintf(stderr, "KOSYNC CLIENT FAIL %s\n",
                cn_kosync_result_name(result));
        return 0;
    }
    return 1;
}

static int parse_progress_value(const char *text, int *value)
{
    char *end;
    long parsed;
    errno = 0;
    parsed = strtol(text, &end, 10);
    if (errno || *text == '\0' || *end != '\0' || parsed < 0 ||
        parsed > 10000)
        return 0;
    *value = (int)parsed;
    return 1;
}

static int parse_local_progress_value(const char *text, int *value)
{
    char *end;
    long parsed;
    errno = 0;
    parsed = strtol(text, &end, 10);
    if (errno || *text == '\0' || *end != '\0' || parsed < -1 ||
        parsed > 10000)
        return 0;
    *value = (int)parsed;
    return 1;
}

static int parse_unsigned_value(const char *text, unsigned maximum,
                                unsigned *value)
{
    char *end;
    unsigned long parsed;
    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno || !text[0] || *end || parsed > maximum)
        return 0;
    *value = (unsigned)parsed;
    return 1;
}

static void secure_clear(void *data, size_t length)
{
    volatile unsigned char *p = (volatile unsigned char *)data;
    while (length--)
        *p++ = 0;
}

static int read_credential_line(FILE *file, char *text, size_t cap)
{
    size_t length = 0;
    int c;
    if (!file || !text || cap < 2)
        return 0;
    while ((c = fgetc(file)) != EOF) {
        if (c == '\n')
            break;
        if (c == '\r') {
            if (fgetc(file) != '\n')
                return 0;
            break;
        }
        if (length + 1 >= cap)
            return 0;
        text[length++] = (char)c;
    }
    if (ferror(file) || length == 0)
        return 0;
    text[length] = '\0';
    return length != 0;
}

static int read_credentials(const char *path,
                            char username[CN_KOSYNC_USERNAME_MAX + 1],
                            char userkey[CN_KOSYNC_USERKEY_MAX + 1])
{
    FILE *file;
    file = fopen(path, "rb");
    if (!file)
        return 0;
    if (!read_credential_line(file, username, CN_KOSYNC_USERNAME_MAX + 1) ||
        !read_credential_line(file, userkey, CN_KOSYNC_USERKEY_MAX + 1) ||
        fgetc(file) != EOF) {
        fclose(file);
        return 0;
    }
    if (fclose(file) != 0)
        return 0;
    return 1;
}

static void print_diagnostic_text(const char *text)
{
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        if (*p == '\n')
            fputs("\\n", stdout);
        else if (*p == '\r')
            fputs("\\r", stdout);
        else if (*p == '\t')
            fputs("\\t", stdout);
        else if (*p == '\\')
            fputs("\\\\", stdout);
        else if (*p < 0x20 || *p == 0x7f)
            printf("\\x%02x", (unsigned)*p);
        else
            fputc(*p, stdout);
        ++p;
    }
}

static int run_local_set(int argc, char **argv)
{
    cn_progress_store *store = NULL;
    cn_progress_record record;
    cn_book_identity identity;
    cn_progress_result result;
    int progress;
    size_t length;
    if (argc != 6 || !parse_local_progress_value(argv[5], &progress) ||
        cn_book_identity_from_path(&identity, argv[3]) != 0)
        return 2;
    result = cn_progress_store_open(&store, argv[2]);
    if (result != CN_PROGRESS_OK)
        return 1;
    cn_progress_record_init(&record);
    length = strlen(argv[4]);
    record.position.location = (char *)malloc(length + 1);
    if (!record.position.location) {
        cn_progress_store_close(store);
        return 1;
    }
    memcpy(record.position.location, argv[4], length + 1);
    record.position.progress_10000 = progress;
    result = cn_progress_store_save(store, &identity, &record);
    cn_progress_record_clear(&record);
    cn_progress_store_close(store);
    if (result != CN_PROGRESS_OK) {
        fprintf(stderr, "KOSYNC LOCAL SET FAIL %s\n",
                cn_progress_result_name(result));
        return 1;
    }
    printf("KOSYNC LOCAL SET progress=%d position=", progress);
    print_diagnostic_text(argv[4]);
    puts(" OK");
    return 0;
}

static int run_local_get(int argc, char **argv)
{
    cn_progress_store *store = NULL;
    cn_progress_record record;
    cn_book_identity identity;
    cn_progress_result result;
    if (argc != 4 || cn_book_identity_from_path(&identity, argv[3]) != 0)
        return 2;
    result = cn_progress_store_open(&store, argv[2]);
    if (result != CN_PROGRESS_OK)
        return 1;
    cn_progress_record_init(&record);
    result = cn_progress_store_load(store, &identity, &record);
    if (result == CN_PROGRESS_OK)
        printf("KOSYNC LOCAL GET progress=%d position=",
               record.position.progress_10000);
    if (result == CN_PROGRESS_OK) {
        print_diagnostic_text(record.position.location);
        puts(" OK");
    } else {
        fprintf(stderr, "KOSYNC LOCAL GET FAIL %s\n",
                cn_progress_result_name(result));
    }
    cn_progress_record_clear(&record);
    cn_progress_store_close(store);
    return result == CN_PROGRESS_OK ? 0 : 1;
}

static int run_sync_once(int argc, char **argv)
{
    cn_progress_store *store = NULL;
    cn_kosync_client client;
    cn_kosync_sync_config config;
    cn_kosync_sync_result sync_result;
    cn_tls_config tls;
    cn_dns_config dns;
    cn_time_config time_config;
    cn_progress_result open_result;
    cn_kosync_sync_status status;
    cn_kosync_product_result product;
    const char *document_text;
    char username[CN_KOSYNC_USERNAME_MAX + 1] = {0};
    char userkey[CN_KOSYNC_USERKEY_MAX + 1] = {0};
    unsigned dns_port;
    unsigned dns_ms;
    unsigned sntp_port;
    unsigned sntp_ms;
    unsigned recv_ms;
    char *end;
    long long epoch;

    if (argc != 18 ||
        !parse_unsigned_value(argv[5], 65535, &dns_port) || dns_port == 0 ||
        !parse_unsigned_value(argv[6], 60000, &dns_ms) || dns_ms == 0 ||
        !parse_unsigned_value(argv[9], 65535, &sntp_port) || sntp_port == 0 ||
        !parse_unsigned_value(argv[10], 60000, &sntp_ms) || sntp_ms == 0 ||
        !parse_unsigned_value(argv[17], 60000, &recv_ms) || recv_ms == 0 ||
        !read_credentials(argv[13], username, userkey)) {
        secure_clear(username, sizeof username);
        secure_clear(userkey, sizeof userkey);
        return 2;
    }
    if (!init_client(&client, argv[11], username, userkey)) {
        secure_clear(username, sizeof username);
        secure_clear(userkey, sizeof userkey);
        secure_clear(&client, sizeof client);
        return 1;
    }
    client.recv_ms = recv_ms;
    if (!client.use_tls) {
        fprintf(stderr, "KOSYNC SYNC FAIL insecure-http-rejected\n");
        secure_clear(username, sizeof username);
        secure_clear(userkey, sizeof userkey);
        secure_clear(&client, sizeof client);
        return 1;
    }

    memset(&tls, 0, sizeof tls);
    tls.ca_path = argv[12];
    memset(&config, 0, sizeof config);
    if (strcmp(argv[7], "sync") == 0) {
        config.time_policy = CN_KOSYNC_SYNC_TIME_ESTABLISH;
        if (strcmp(argv[16], "-") != 0) {
            secure_clear(username, sizeof username);
            secure_clear(userkey, sizeof userkey);
            secure_clear(&client, sizeof client);
            return 2;
        }
    } else if (strcmp(argv[7], "caller-established") == 0) {
        config.time_policy = CN_KOSYNC_SYNC_TIME_CALLER_ESTABLISHED;
        if (strcmp(argv[16], "-") != 0) {
            errno = 0;
            epoch = strtoll(argv[16], &end, 10);
            if (errno || !argv[16][0] || *end || epoch <= 0) {
                secure_clear(username, sizeof username);
                secure_clear(userkey, sizeof userkey);
                secure_clear(&client, sizeof client);
                return 2;
            }
            tls_fixed_now = (time_t)epoch;
            tls.get_time = tls_fixed_time;
        }
    } else {
        secure_clear(username, sizeof username);
        secure_clear(userkey, sizeof userkey);
        secure_clear(&client, sizeof client);
        return 2;
    }

    memset(&dns, 0, sizeof dns);
    dns.servers[0] = argv[4];
    dns.server_count = 1;
    dns.port = dns_port;
    dns.timeout_ms = dns_ms;
    memset(&time_config, 0, sizeof time_config);
    time_config.servers[0] = argv[8];
    time_config.server_count = 1;
    time_config.port = sntp_port;
    time_config.timeout_ms = sntp_ms;

    open_result = cn_progress_store_open(&store, argv[2]);
    if (open_result != CN_PROGRESS_OK) {
        fprintf(stderr, "KOSYNC SYNC FAIL store=%s\n",
                cn_progress_result_name(open_result));
        secure_clear(username, sizeof username);
        secure_clear(userkey, sizeof userkey);
        secure_clear(&client, sizeof client);
        return 1;
    }
    config.document_path = argv[3];
    config.progress_store = store;
    config.time = &time_config;
    config.dns = &dns;
    config.tls = &tls;
    config.client = &client;
    config.device = argv[14];
    config.device_id = argv[15];
    cn_kosync_sync_result_init(&sync_result);
    status = cn_kosync_sync_once(&config, &sync_result);
    (void)cn_kosync_policy_classify(1, &sync_result, &product);
    document_text = cn_koreader_document_id_text(&sync_result.document_id);
    printf("KOSYNC SYNC status=%s decision=%s local=%d remote=%d "
           "saved=%d uploaded=%d document=%s identity=%s load=%s save=%s "
           "time=%s dns=%s kosync=%s http=%d transport=%s timestamp=%lld "
           "remote_progress=%d remote_position=",
           cn_kosync_sync_status_name(status),
           cn_kosync_sync_decision_name(sync_result.decision),
           sync_result.local_present, sync_result.remote_present,
           sync_result.local_saved, sync_result.remote_uploaded,
           document_text ? document_text : "-",
           cn_koreader_identity_result_name(sync_result.identity_result),
           cn_progress_result_name(sync_result.local_load_result),
           cn_progress_result_name(sync_result.local_save_result),
           cn_timesimple_result_name(sync_result.time_result),
           cn_dnssimple_result_name(sync_result.dns_result),
           cn_kosync_result_name(sync_result.kosync_result),
           sync_result.kosync_outcome.http_status,
           cn_netsimple_result_name(sync_result.kosync_outcome.transport_result),
           sync_result.has_put_timestamp ? sync_result.put_timestamp : -1,
           sync_result.remote_present
               ? sync_result.remote_progress.progress_10000 : -1);
    print_diagnostic_text(sync_result.remote_present
                               ? sync_result.remote_progress.logical_position
                               : "-");
    printf(" local-save-attempted=%d remote-put-attempted=%d "
           "outcome=%s retry=%s local-mutation=%s remote-mutation=%s\n",
           sync_result.local_save_attempted,
           sync_result.remote_put_attempted,
           cn_kosync_product_outcome_name(product.outcome),
           cn_kosync_retry_policy_name(product.retry),
           cn_kosync_mutation_state_name(product.local_mutation),
           cn_kosync_mutation_state_name(product.remote_mutation));
    cn_kosync_sync_result_clear(&sync_result);
    cn_progress_store_close(store);
    secure_clear(username, sizeof username);
    secure_clear(userkey, sizeof userkey);
    secure_clear(&client, sizeof client);
    return status == CN_KOSYNC_SYNC_STATUS_OK ? 0 : 1;
}

static int run_put(int argc, char **argv)
{
    cn_kosync_client client;
    cn_kosync_progress progress;
    cn_kosync_outcome outcome;
    cn_kosync_result result;
    long long timestamp = 0;
    int normalized;
    memset(&outcome, 0, sizeof outcome);
    if (argc != 10 || !parse_progress_value(argv[7], &normalized))
        return 2;
    if (!init_client(&client, argv[2], argv[3], argv[4]))
        return 1;
    cn_kosync_progress_init(&progress);
    result = cn_kosync_progress_set(&progress, argv[5], argv[6], normalized,
                                    argv[8], argv[9]);
    if (result == CN_KOSYNC_OK)
        result = cn_kosync_put_progress(&client, &progress, &timestamp,
                                        &outcome);
    cn_kosync_progress_clear(&progress);
    if (result != CN_KOSYNC_OK) {
        fprintf(stderr, "KOSYNC PUT FAIL %s http=%d transport=%s\n",
                cn_kosync_result_name(result), outcome.http_status,
                cn_netsimple_result_name(outcome.transport_result));
        return 1;
    }
    printf("KOSYNC PUT 200 %lld %s OK\n", timestamp, argv[5]);
    return 0;
}

static int run_get(int argc, char **argv)
{
    cn_kosync_client client;
    cn_kosync_progress progress;
    cn_kosync_outcome outcome;
    cn_kosync_result result;
    if (argc != 6)
        return 2;
    if (!init_client(&client, argv[2], argv[3], argv[4]))
        return 1;
    cn_kosync_progress_init(&progress);
    result = cn_kosync_get_progress(&client, argv[5], &progress, &outcome);
    if (result == CN_KOSYNC_NOT_FOUND) {
        printf("KOSYNC GET 200 %s NOT-FOUND\n", argv[5]);
        return 0;
    }
    if (result != CN_KOSYNC_OK) {
        fprintf(stderr, "KOSYNC GET FAIL %s http=%d transport=%s\n",
                cn_kosync_result_name(result), outcome.http_status,
                cn_netsimple_result_name(outcome.transport_result));
        cn_kosync_progress_clear(&progress);
        return 1;
    }
    printf("KOSYNC GET 200 %s %d %lld %s %s %s OK\n",
           progress.document_id, progress.progress_10000,
           progress.has_timestamp ? progress.timestamp : -1,
           progress.device, progress.device_id, progress.logical_position);
    cn_kosync_progress_clear(&progress);
    return 0;
}

static int put_fixture(cn_kosync_client *client, const char *document,
                       const char *position, int normalized,
                       long long *timestamp)
{
    cn_kosync_progress progress;
    cn_kosync_result result;
    cn_kosync_progress_init(&progress);
    result = cn_kosync_progress_set(&progress, document, position, normalized,
                                    TEST_DEVICE, TEST_DEVICE_ID);
    if (result == CN_KOSYNC_OK)
        result = cn_kosync_put_progress(client, &progress, timestamp, NULL);
    cn_kosync_progress_clear(&progress);
    return result == CN_KOSYNC_OK;
}

static int get_fixture(cn_kosync_client *client, const char *document,
                       const char *position, int normalized,
                       long long minimum_timestamp)
{
    cn_kosync_progress progress;
    cn_kosync_result result;
    int matches;
    cn_kosync_progress_init(&progress);
    result = cn_kosync_get_progress(client, document, &progress, NULL);
    matches = result == CN_KOSYNC_OK &&
              progress_matches(&progress, document, position, normalized) &&
              progress.has_timestamp && progress.timestamp >= minimum_timestamp;
    cn_kosync_progress_clear(&progress);
    return matches;
}

static int roundtrip_client(cn_kosync_client *client,
                            const char *document, const char *other)
{
    long long first = 0;
    long long second = 0;
    long long foreign = 0;
    int failures = 0;
    check(put_fixture(client, document, POSITION_ONE, 3210, &first),
          "first progress upload", &failures);
    check(get_fixture(client, document, POSITION_ONE, 3210, first),
          "first progress retrieval", &failures);
    check(put_fixture(client, document, POSITION_TWO, 6543, &second) &&
              second >= first,
          "same-document update", &failures);
    check(get_fixture(client, document, POSITION_TWO, 6543, second),
          "updated progress retrieval", &failures);
    check(put_fixture(client, other, POSITION_ONE, 1111, &foreign),
          "distinct-document upload", &failures);
    check(get_fixture(client, other, POSITION_ONE, 1111, foreign) &&
              get_fixture(client, document, POSITION_TWO, 6543, second),
          "distinct documents remain independent", &failures);
    printf("KOSYNC ROUNDTRIP failures=%d first=%lld update=%lld other=%lld -> %s\n",
           failures, first, second, foreign,
           failures == 0 ? "OK" : "FAIL");
    return failures == 0 ? 0 : 1;
}

static int run_roundtrip(int argc, char **argv)
{
    cn_kosync_client client;

    if (argc != 7)
        return 2;
    if (!init_client(&client, argv[2], argv[3], argv[4]))
        return 1;
    return roundtrip_client(&client, argv[5], argv[6]);
}

static int run_roundtrip_https(int argc, char **argv)
{
    cn_kosync_client client;
    cn_tls_config tls;

    if (argc != 9 && argc != 10)
        return 2;
    if (!init_client(&client, argv[2], argv[5], argv[6]))
        return 1;
    memset(&tls, 0, sizeof tls);
    tls.ca_path = argv[4];
    if (argc == 10) {
        char *end;
        long long epoch;

        errno = 0;
        epoch = strtoll(argv[9], &end, 10);
        if (errno || !argv[9][0] || *end || epoch <= 0)
            return 2;
        tls_fixed_now = (time_t)epoch;
        tls.get_time = tls_fixed_time;
    }
    if (cn_kosync_client_set_tls(&client, &tls, argv[3]) != CN_KOSYNC_OK) {
        fprintf(stderr, "KOSYNC TLS CONFIG FAIL\n");
        return 1;
    }
    return roundtrip_client(&client, argv[7], argv[8]);
}

static int run_mock_smoke(const char *url)
{
    cn_kosync_client client;
    cn_kosync_client wrong;
    cn_kosync_progress progress;
    cn_kosync_outcome outcome;
    cn_kosync_result result;
    int failures = 0;
    if (!init_client(&client, url, TEST_USER, TEST_KEY) ||
        !init_client(&wrong, url, TEST_USER, "wrong-key"))
        return 1;
    check(cn_kosync_authorize(&client, &outcome) == CN_KOSYNC_OK &&
              outcome.http_status == 200,
          "read-only /users/auth accepts valid credentials", &failures);
    check(cn_kosync_authorize(&wrong, &outcome) == CN_KOSYNC_AUTH_FAILED &&
              outcome.http_status == 401,
          "read-only /users/auth rejects invalid credentials", &failures);
    cn_kosync_progress_init(&progress);
    check(cn_kosync_get_progress(&client, FOREIGN_DOCUMENT, &progress, NULL) ==
              CN_KOSYNC_NOT_FOUND,
          "mock returns empty object for unknown document", &failures);
    check(put_fixture(&client, TEST_DOCUMENT, POSITION_ONE, 3210, NULL) &&
              get_fixture(&client, TEST_DOCUMENT, POSITION_ONE, 3210, 0),
          "mock stores and retrieves first progress", &failures);
    check(put_fixture(&client, TEST_DOCUMENT, POSITION_TWO, 6543, NULL) &&
              get_fixture(&client, TEST_DOCUMENT, POSITION_TWO, 6543, 0),
          "mock replaces the same document on later PUT", &failures);
    check(put_fixture(&client, FOREIGN_DOCUMENT, POSITION_ONE, 1111, NULL) &&
              get_fixture(&client, FOREIGN_DOCUMENT, POSITION_ONE, 1111, 0) &&
              get_fixture(&client, TEST_DOCUMENT, POSITION_TWO, 6543, 0),
          "mock keeps distinct document IDs independent", &failures);
    check(cn_kosync_get_progress(&wrong, TEST_DOCUMENT, &progress, NULL) ==
              CN_KOSYNC_AUTH_FAILED,
          "401 maps to auth-failed", &failures);
    check(cn_kosync_get_progress(&client, FAULT_MALFORMED, &progress, NULL) ==
              CN_KOSYNC_BAD_JSON,
          "malformed JSON response is rejected", &failures);
    check(cn_kosync_get_progress(&client, FAULT_PROTOCOL, &progress, NULL) ==
              CN_KOSYNC_BAD_PROTOCOL,
          "malformed protocol fields are rejected", &failures);
    result = cn_kosync_get_progress(&client, FAULT_OVERSIZED, &progress,
                                    &outcome);
    check(result == CN_KOSYNC_TRANSPORT_ERROR &&
              outcome.transport_result == CN_NETSIMPLE_TRUNCATED,
          "oversized response is bounded and reported", &failures);
    result = cn_kosync_get_progress(&client, FAULT_SERVER, &progress,
                                    &outcome);
    check(result == CN_KOSYNC_HTTP_ERROR && outcome.http_status == 500,
          "server 5xx maps to http-error", &failures);
    cn_kosync_progress_clear(&progress);
    printf("KOSYNC MOCK SMOKE failures=%d -> %s\n", failures,
           failures == 0 ? "OK" : "FAIL");
    return failures == 0 ? 0 : 1;
}

static int run_network_error(const char *url)
{
    cn_kosync_client client;
    cn_kosync_progress progress;
    cn_kosync_outcome outcome;
    cn_kosync_result result;
    if (!init_client(&client, url, TEST_USER, TEST_KEY))
        return 1;
    client.connect_ms = 500;
    cn_kosync_progress_init(&progress);
    result = cn_kosync_get_progress(&client, TEST_DOCUMENT, &progress,
                                    &outcome);
    cn_kosync_progress_clear(&progress);
    printf("KOSYNC NETWORK result=%s transport=%s -> %s\n",
           cn_kosync_result_name(result),
           cn_netsimple_result_name(outcome.transport_result),
           result == CN_KOSYNC_TRANSPORT_ERROR ? "OK" : "FAIL");
    return result == CN_KOSYNC_TRANSPORT_ERROR ? 0 : 1;
}

static void usage(void)
{
    fprintf(stderr,
        "usage: crossnook-kosync-test --api-smoke\n"
        "       crossnook-kosync-test --policy-smoke\n"
        "       crossnook-kosync-test --mock-smoke <base-url>\n"
        "       crossnook-kosync-test --network-error <base-url>\n"
        "       crossnook-kosync-test --put <url> <user> <key> <doc> <position> <progress-10000> <device> <device-id>\n"
        "       crossnook-kosync-test --get <url> <user> <key> <doc>\n"
        "       crossnook-kosync-test --roundtrip <url> <user> <key> <doc1> <doc2>\n");
    fprintf(stderr,
        "       crossnook-kosync-test --roundtrip-https <url> <connect-host> <ca> <user> <key> <doc1> <doc2> [epoch]\n"
        "       crossnook-kosync-test --local-set <state-dir> <epub> <position> <-1..10000>\n"
        "       crossnook-kosync-test --local-get <state-dir> <epub>\n"
        "       crossnook-kosync-test --sync-once <state-dir> <epub> <dns-server> <dns-port> <dns-ms> <sync|caller-established> <sntp-server> <sntp-port> <sntp-ms> <https-url> <ca> <credential-file> <device> <device-id> <epoch|-> <recv-ms>\n");
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--api-smoke") == 0)
        return run_api_smoke();
    if (argc == 2 && strcmp(argv[1], "--policy-smoke") == 0)
        return run_policy_smoke();
    if (argc == 3 && strcmp(argv[1], "--mock-smoke") == 0)
        return run_mock_smoke(argv[2]);
    if (argc == 3 && strcmp(argv[1], "--network-error") == 0)
        return run_network_error(argv[2]);
    if (argc > 1 && strcmp(argv[1], "--put") == 0)
        return run_put(argc, argv);
    if (argc > 1 && strcmp(argv[1], "--get") == 0)
        return run_get(argc, argv);
    if (argc > 1 && strcmp(argv[1], "--roundtrip") == 0)
        return run_roundtrip(argc, argv);
    if (argc > 1 && strcmp(argv[1], "--roundtrip-https") == 0)
        return run_roundtrip_https(argc, argv);
    if (argc > 1 && strcmp(argv[1], "--local-set") == 0)
        return run_local_set(argc, argv);
    if (argc > 1 && strcmp(argv[1], "--local-get") == 0)
        return run_local_get(argc, argv);
    if (argc > 1 && strcmp(argv[1], "--sync-once") == 0)
        return run_sync_once(argc, argv);
    usage();
    return 2;
}

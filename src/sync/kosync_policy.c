#include "sync/kosync_policy.h"

static void fail_closed(cn_kosync_product_result *result)
{
    result->outcome = CN_KOSYNC_PRODUCT_INTERNAL_FAILURE;
    result->retry = CN_KOSYNC_RETRY_EXPLICIT_ACTION;
    result->local_mutation = CN_KOSYNC_MUTATION_NONE;
    result->remote_mutation = CN_KOSYNC_MUTATION_NONE;
}

static int boolean_fields_valid(const cn_kosync_sync_result *sync)
{
    return (sync->local_present == 0 || sync->local_present == 1) &&
           (sync->remote_present == 0 || sync->remote_present == 1) &&
           (sync->local_save_attempted == 0 ||
            sync->local_save_attempted == 1) &&
           (sync->remote_put_attempted == 0 ||
            sync->remote_put_attempted == 1) &&
           (sync->local_saved == 0 || sync->local_saved == 1) &&
           (sync->remote_uploaded == 0 || sync->remote_uploaded == 1);
}

static int transport_valid(cn_netsimple_result transport)
{
    return transport >= CN_NETSIMPLE_OK &&
           transport < CN_NETSIMPLE_LENGTH;
}

static int transport_is_pre_put(cn_netsimple_result transport)
{
    switch (transport) {
    case CN_NETSIMPLE_INVALID:
    case CN_NETSIMPLE_RESOLVE_ERROR:
    case CN_NETSIMPLE_CONNECT_REFUSED:
    case CN_NETSIMPLE_NETWORK_UNREACHABLE:
    case CN_NETSIMPLE_HOST_UNREACHABLE:
    case CN_NETSIMPLE_CONNECT_TIMEOUT:
    case CN_NETSIMPLE_CONNECT_ERROR:
    case CN_NETSIMPLE_TLS_ENTROPY_FAILED:
    case CN_NETSIMPLE_TLS_INVALID_CA:
    case CN_NETSIMPLE_TLS_HANDSHAKE_FAILED:
    case CN_NETSIMPLE_TLS_HANDSHAKE_TIMEOUT:
    case CN_NETSIMPLE_TLS_TRUST_FAILED:
    case CN_NETSIMPLE_TLS_HOSTNAME_MISMATCH:
    case CN_NETSIMPLE_TLS_CERT_TIME_FAILED:
    case CN_NETSIMPLE_TLS_CERT_INVALID:
    case CN_NETSIMPLE_TLS_INTERNAL:
        return 1;
    default:
        return 0;
    }
}

static cn_kosync_mutation_state remote_mutation(
    const cn_kosync_sync_result *sync)
{
    if (sync->remote_uploaded)
        return CN_KOSYNC_MUTATION_CONFIRMED;
    if (!sync->remote_put_attempted)
        return CN_KOSYNC_MUTATION_NONE;

    if (sync->status == CN_KOSYNC_SYNC_STATUS_AUTH_FAILED ||
        sync->status == CN_KOSYNC_SYNC_STATUS_INVALID)
        return CN_KOSYNC_MUTATION_NONE;
    if (sync->status == CN_KOSYNC_SYNC_STATUS_HTTPS_FAILED &&
        transport_valid(sync->kosync_outcome.transport_result) &&
        transport_is_pre_put(sync->kosync_outcome.transport_result))
        return CN_KOSYNC_MUTATION_NONE;

    return CN_KOSYNC_MUTATION_POSSIBLE;
}

static int success_invariants_hold(const cn_kosync_sync_result *sync)
{
    if (sync->local_saved && !sync->local_save_attempted)
        return 0;
    if (sync->remote_uploaded && !sync->remote_put_attempted)
        return 0;

    switch (sync->decision) {
    case CN_KOSYNC_SYNC_NO_STATE:
        return !sync->local_present && !sync->remote_present &&
               !sync->local_save_attempted && !sync->remote_put_attempted &&
               !sync->local_saved && !sync->remote_uploaded;
    case CN_KOSYNC_SYNC_LOCAL_SELECTED:
        return sync->local_present && !sync->remote_present &&
               !sync->local_save_attempted && sync->remote_put_attempted &&
               !sync->local_saved && sync->remote_uploaded;
    case CN_KOSYNC_SYNC_REMOTE_SELECTED:
        return !sync->local_present && sync->remote_present &&
               sync->local_save_attempted && !sync->remote_put_attempted &&
               sync->local_saved && !sync->remote_uploaded;
    case CN_KOSYNC_SYNC_NO_CHANGE:
    case CN_KOSYNC_SYNC_AMBIGUOUS:
        return sync->local_present && sync->remote_present &&
               !sync->local_save_attempted && !sync->remote_put_attempted &&
               !sync->local_saved && !sync->remote_uploaded;
    default:
        return 0;
    }
}

static void classify_success(const cn_kosync_sync_result *sync,
                             cn_kosync_product_result *result)
{
    if (!success_invariants_hold(sync)) {
        fail_closed(result);
        return;
    }

    result->retry = sync->decision == CN_KOSYNC_SYNC_AMBIGUOUS
                        ? CN_KOSYNC_RETRY_EXPLICIT_ACTION
                        : CN_KOSYNC_RETRY_NONE;
    switch (sync->decision) {
    case CN_KOSYNC_SYNC_NO_STATE:
        result->outcome = CN_KOSYNC_PRODUCT_NO_STATE;
        break;
    case CN_KOSYNC_SYNC_LOCAL_SELECTED:
        result->outcome = CN_KOSYNC_PRODUCT_UPLOADED;
        break;
    case CN_KOSYNC_SYNC_REMOTE_SELECTED:
        result->outcome = CN_KOSYNC_PRODUCT_IMPORTED;
        break;
    case CN_KOSYNC_SYNC_NO_CHANGE:
        result->outcome = CN_KOSYNC_PRODUCT_UNCHANGED;
        break;
    case CN_KOSYNC_SYNC_AMBIGUOUS:
        result->outcome = CN_KOSYNC_PRODUCT_CONFLICT;
        break;
    default:
        fail_closed(result);
        break;
    }
}

static void classify_time(const cn_kosync_sync_result *sync,
                          cn_kosync_product_result *result)
{
    if (sync->time_result < CN_TIME_OK ||
        sync->time_result >= CN_TIME_RESULT_COUNT ||
        sync->time_result == CN_TIME_OK) {
        fail_closed(result);
        return;
    }
    result->outcome = CN_KOSYNC_PRODUCT_TRUSTED_TIME_UNAVAILABLE;
    switch (sync->time_result) {
    case CN_TIME_SOCKET_FAILED:
    case CN_TIME_SEND_FAILED:
    case CN_TIME_TIMEOUT:
    case CN_TIME_RECV_FAILED:
        result->retry = CN_KOSYNC_RETRY_AUTOMATIC_LATER;
        break;
    default:
        result->retry = CN_KOSYNC_RETRY_EXPLICIT_ACTION;
        break;
    }
}

static void classify_dns(const cn_kosync_sync_result *sync,
                         cn_kosync_product_result *result)
{
    if (sync->dns_result < CN_DNS_OK ||
        sync->dns_result >= CN_DNS_RESULT_COUNT ||
        sync->dns_result == CN_DNS_OK) {
        fail_closed(result);
        return;
    }
    switch (sync->dns_result) {
    case CN_DNS_SOCKET_FAILED:
    case CN_DNS_NETWORK_FAILED:
    case CN_DNS_TIMEOUT:
    case CN_DNS_SERVER_FAILURE:
        result->outcome = CN_KOSYNC_PRODUCT_CONNECTIVITY_FAILURE;
        result->retry = CN_KOSYNC_RETRY_AUTOMATIC_LATER;
        break;
    case CN_DNS_INVALID:
    case CN_DNS_INVALID_HOSTNAME:
        result->outcome = CN_KOSYNC_PRODUCT_CONFIGURATION_FAILURE;
        result->retry = CN_KOSYNC_RETRY_EXPLICIT_ACTION;
        break;
    case CN_DNS_ENTROPY_FAILED:
        result->outcome = CN_KOSYNC_PRODUCT_INTERNAL_FAILURE;
        result->retry = CN_KOSYNC_RETRY_EXPLICIT_ACTION;
        break;
    case CN_DNS_PACKET_SIZE:
    case CN_DNS_MALFORMED_RESPONSE:
    case CN_DNS_TRUNCATED_RESPONSE:
    case CN_DNS_NXDOMAIN:
    case CN_DNS_NO_ADDRESS:
    case CN_DNS_UNSUPPORTED_CNAME:
        result->outcome = CN_KOSYNC_PRODUCT_SERVICE_FAILURE;
        result->retry = CN_KOSYNC_RETRY_EXPLICIT_ACTION;
        break;
    default:
        fail_closed(result);
        break;
    }
}

static void classify_https(const cn_kosync_sync_result *sync,
                           cn_kosync_product_result *result)
{
    cn_netsimple_result transport = sync->kosync_outcome.transport_result;
    if (!transport_valid(transport) || transport == CN_NETSIMPLE_OK) {
        fail_closed(result);
        return;
    }
    switch (transport) {
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
        result->outcome = CN_KOSYNC_PRODUCT_CONNECTIVITY_FAILURE;
        result->retry = CN_KOSYNC_RETRY_AUTOMATIC_LATER;
        break;
    case CN_NETSIMPLE_TLS_INVALID_CA:
    case CN_NETSIMPLE_TLS_TRUST_FAILED:
    case CN_NETSIMPLE_TLS_HOSTNAME_MISMATCH:
    case CN_NETSIMPLE_TLS_CERT_TIME_FAILED:
    case CN_NETSIMPLE_TLS_CERT_INVALID:
        result->outcome = CN_KOSYNC_PRODUCT_SECURITY_FAILURE;
        result->retry = CN_KOSYNC_RETRY_EXPLICIT_ACTION;
        break;
    case CN_NETSIMPLE_TRUNCATED:
    case CN_NETSIMPLE_BAD_RESPONSE:
    case CN_NETSIMPLE_TLS_HANDSHAKE_FAILED:
    case CN_NETSIMPLE_TLS_PROTOCOL_FAILED:
        result->outcome = CN_KOSYNC_PRODUCT_SERVICE_FAILURE;
        result->retry = CN_KOSYNC_RETRY_EXPLICIT_ACTION;
        break;
    case CN_NETSIMPLE_INVALID:
        result->outcome = CN_KOSYNC_PRODUCT_CONFIGURATION_FAILURE;
        result->retry = CN_KOSYNC_RETRY_EXPLICIT_ACTION;
        break;
    case CN_NETSIMPLE_TLS_ENTROPY_FAILED:
    case CN_NETSIMPLE_TLS_INTERNAL:
        result->outcome = CN_KOSYNC_PRODUCT_INTERNAL_FAILURE;
        result->retry = CN_KOSYNC_RETRY_EXPLICIT_ACTION;
        break;
    default:
        fail_closed(result);
        break;
    }
}

static void classify_protocol(const cn_kosync_sync_result *sync,
                              cn_kosync_product_result *result)
{
    if (sync->kosync_result < CN_KOSYNC_OK ||
        sync->kosync_result >= CN_KOSYNC_RESULT_COUNT) {
        fail_closed(result);
        return;
    }
    result->outcome = CN_KOSYNC_PRODUCT_SERVICE_FAILURE;
    result->retry = CN_KOSYNC_RETRY_EXPLICIT_ACTION;
    if (sync->kosync_result == CN_KOSYNC_HTTP_ERROR &&
        (sync->kosync_outcome.http_status == 408 ||
         sync->kosync_outcome.http_status == 429 ||
         (sync->kosync_outcome.http_status >= 500 &&
          sync->kosync_outcome.http_status <= 599)))
        result->retry = CN_KOSYNC_RETRY_AUTOMATIC_LATER;
    else if (sync->kosync_result != CN_KOSYNC_HTTP_ERROR &&
             sync->kosync_result != CN_KOSYNC_BAD_JSON &&
             sync->kosync_result != CN_KOSYNC_BAD_PROTOCOL)
        fail_closed(result);
}

int cn_kosync_policy_classify(int enabled,
                              const cn_kosync_sync_result *sync,
                              cn_kosync_product_result *result)
{
    if (!result)
        return -1;
    fail_closed(result);
    if (enabled != 0 && enabled != 1)
        return -1;
    if (!enabled) {
        result->outcome = CN_KOSYNC_PRODUCT_DISABLED;
        result->retry = CN_KOSYNC_RETRY_NONE;
        return 0;
    }
    if (!sync || !boolean_fields_valid(sync) ||
        sync->status < CN_KOSYNC_SYNC_STATUS_OK ||
        sync->status >= CN_KOSYNC_SYNC_STATUS_COUNT ||
        sync->decision < CN_KOSYNC_SYNC_DECISION_NONE ||
        sync->decision >= CN_KOSYNC_SYNC_DECISION_COUNT)
        return -1;
    if ((sync->status == CN_KOSYNC_SYNC_STATUS_TRUSTED_TIME_FAILED &&
         (sync->time_result < CN_TIME_OK ||
          sync->time_result >= CN_TIME_RESULT_COUNT)) ||
        (sync->status == CN_KOSYNC_SYNC_STATUS_DNS_FAILED &&
         (sync->dns_result < CN_DNS_OK ||
          sync->dns_result >= CN_DNS_RESULT_COUNT)) ||
        (sync->status == CN_KOSYNC_SYNC_STATUS_HTTPS_FAILED &&
         !transport_valid(sync->kosync_outcome.transport_result)) ||
        (sync->status == CN_KOSYNC_SYNC_STATUS_PROTOCOL_FAILED &&
         (sync->kosync_result < CN_KOSYNC_OK ||
          sync->kosync_result >= CN_KOSYNC_RESULT_COUNT)))
        return -1;

    result->local_mutation = sync->local_saved
                                 ? CN_KOSYNC_MUTATION_CONFIRMED
                                 : sync->local_save_attempted
                                       ? CN_KOSYNC_MUTATION_POSSIBLE
                                       : CN_KOSYNC_MUTATION_NONE;
    result->remote_mutation = remote_mutation(sync);

    if ((sync->local_saved && !sync->local_save_attempted) ||
        (sync->remote_uploaded && !sync->remote_put_attempted) ||
        (sync->status != CN_KOSYNC_SYNC_STATUS_OK &&
         (sync->local_saved || sync->remote_uploaded))) {
        fail_closed(result);
        return 0;
    }

    switch (sync->status) {
    case CN_KOSYNC_SYNC_STATUS_OK:
        classify_success(sync, result);
        break;
    case CN_KOSYNC_SYNC_STATUS_INVALID:
        result->outcome = CN_KOSYNC_PRODUCT_CONFIGURATION_FAILURE;
        result->retry = CN_KOSYNC_RETRY_EXPLICIT_ACTION;
        break;
    case CN_KOSYNC_SYNC_STATUS_IDENTITY_FAILED:
    case CN_KOSYNC_SYNC_STATUS_LOCAL_FAILED:
        result->outcome = CN_KOSYNC_PRODUCT_LOCAL_FAILURE;
        result->retry = CN_KOSYNC_RETRY_EXPLICIT_ACTION;
        break;
    case CN_KOSYNC_SYNC_STATUS_LOCAL_UNSUPPORTED:
        result->outcome = CN_KOSYNC_PRODUCT_LOCAL_UNSUPPORTED;
        result->retry = CN_KOSYNC_RETRY_EXPLICIT_ACTION;
        break;
    case CN_KOSYNC_SYNC_STATUS_TRUSTED_TIME_FAILED:
        classify_time(sync, result);
        break;
    case CN_KOSYNC_SYNC_STATUS_DNS_FAILED:
        classify_dns(sync, result);
        break;
    case CN_KOSYNC_SYNC_STATUS_AUTH_FAILED:
        result->outcome = CN_KOSYNC_PRODUCT_AUTH_REQUIRED;
        result->retry = CN_KOSYNC_RETRY_EXPLICIT_ACTION;
        break;
    case CN_KOSYNC_SYNC_STATUS_HTTPS_FAILED:
        classify_https(sync, result);
        break;
    case CN_KOSYNC_SYNC_STATUS_PROTOCOL_FAILED:
        classify_protocol(sync, result);
        break;
    case CN_KOSYNC_SYNC_STATUS_NO_MEMORY:
        result->outcome = CN_KOSYNC_PRODUCT_INTERNAL_FAILURE;
        result->retry = CN_KOSYNC_RETRY_EXPLICIT_ACTION;
        break;
    default:
        fail_closed(result);
        return -1;
    }
    return 0;
}

const char *cn_kosync_product_outcome_name(
    cn_kosync_product_outcome outcome)
{
    static const char *const names[CN_KOSYNC_PRODUCT_OUTCOME_COUNT] = {
        "disabled", "unchanged", "uploaded", "imported", "no-state",
        "conflict", "auth-required", "trusted-time-unavailable",
        "connectivity-failure", "security-failure", "service-failure",
        "local-unsupported", "local-failure", "configuration-failure",
        "internal-failure"
    };
    if (outcome < 0 || outcome >= CN_KOSYNC_PRODUCT_OUTCOME_COUNT)
        return "unknown";
    return names[outcome];
}

const char *cn_kosync_retry_policy_name(cn_kosync_retry_policy retry)
{
    static const char *const names[CN_KOSYNC_RETRY_POLICY_COUNT] = {
        "none", "automatic-later", "explicit-action"
    };
    if (retry < 0 || retry >= CN_KOSYNC_RETRY_POLICY_COUNT)
        return "unknown";
    return names[retry];
}

const char *cn_kosync_mutation_state_name(
    cn_kosync_mutation_state mutation)
{
    static const char *const names[CN_KOSYNC_MUTATION_STATE_COUNT] = {
        "none", "confirmed", "possible"
    };
    if (mutation < 0 || mutation >= CN_KOSYNC_MUTATION_STATE_COUNT)
        return "unknown";
    return names[mutation];
}

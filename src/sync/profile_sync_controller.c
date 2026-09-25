#include <string.h>

#include "sync/profile_sync_controller.h"
#include "sync/sync_controller_internal.h"

static size_t bounded_length(const char *text, size_t capacity)
{
    size_t i;
    if (!text) return capacity;
    for (i = 0; i < capacity; ++i)
        if (text[i] == '\0') return i;
    return capacity;
}

static int nonempty_bounded(const char *text, size_t capacity)
{
    size_t length = bounded_length(text, capacity);
    return length > 0 && length < capacity;
}

static int valid_device_name(const char *text)
{
    size_t i;
    size_t length = bounded_length(text, CN_SETTINGS_DEVICE_NAME_CAPACITY);
    if (length == 0 || length >= CN_SETTINGS_DEVICE_NAME_CAPACITY)
        return 0;
    for (i = 0; i < length;) {
        unsigned codepoint;
        unsigned char first = (unsigned char)text[i++];
        int remaining;
        unsigned minimum;
        if (first < 0x80) {
            codepoint = first;
            remaining = 0;
            minimum = 0;
        } else if ((first & 0xe0) == 0xc0) {
            codepoint = first & 0x1f;
            remaining = 1;
            minimum = 0x80;
        } else if ((first & 0xf0) == 0xe0) {
            codepoint = first & 0x0f;
            remaining = 2;
            minimum = 0x800;
        } else if ((first & 0xf8) == 0xf0) {
            codepoint = first & 0x07;
            remaining = 3;
            minimum = 0x10000;
        } else return 0;
        if (i + (size_t)remaining > length) return 0;
        while (remaining-- > 0) {
            unsigned char continuation = (unsigned char)text[i++];
            if ((continuation & 0xc0) != 0x80) return 0;
            codepoint = (codepoint << 6) | (continuation & 0x3f);
        }
        if (codepoint < minimum || codepoint > 0x10ffff ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff) ||
            codepoint < 0x20 || (codepoint >= 0x7f && codepoint <= 0x9f))
            return 0;
    }
    return 1;
}

static int valid_profile(const cn_persisted_sync_profile *profile)
{
    return profile &&
           nonempty_bounded(profile->base_url, sizeof profile->base_url) &&
           nonempty_bounded(profile->username, sizeof profile->username) &&
           nonempty_bounded(profile->userkey, sizeof profile->userkey) &&
           nonempty_bounded(profile->device_id, sizeof profile->device_id) &&
           valid_device_name(profile->device_name);
}

static int valid_runtime(const cn_sync_runtime_inputs *runtime)
{
    size_t i;
    if (!runtime || !runtime->dns || !runtime->tls ||
        !runtime->tls->ca_path || !runtime->tls->ca_path[0] ||
        runtime->dns->server_count == 0 ||
        runtime->dns->server_count > CN_DNS_MAX_SERVERS)
        return 0;
    for (i = 0; i < runtime->dns->server_count; ++i)
        if (!runtime->dns->servers[i] || !runtime->dns->servers[i][0])
            return 0;
    if (runtime->time_policy != CN_KOSYNC_SYNC_TIME_ESTABLISH &&
        runtime->time_policy != CN_KOSYNC_SYNC_TIME_CALLER_ESTABLISHED)
        return 0;
    if (runtime->time_policy == CN_KOSYNC_SYNC_TIME_ESTABLISH &&
        (!runtime->time || runtime->tls->get_time))
        return 0;
    return 1;
}

static int valid_operation_inputs(const cn_sync_runtime_inputs *runtime,
                                  cn_progress_store *progress_store,
                                  const char *document_path)
{
    return valid_runtime(runtime) && progress_store && document_path &&
           document_path[0];
}

static void resolved_from_profile(cn_sync_resolved_config *resolved,
                                  const cn_persisted_sync_profile *profile,
                                  const cn_sync_runtime_inputs *runtime,
                                  cn_progress_store *progress_store,
                                  const char *document_path)
{
    memset(resolved, 0, sizeof *resolved);
    resolved->base_url = profile->base_url;
    resolved->device_name = profile->device_name;
    resolved->username = profile->username;
    resolved->userkey = profile->userkey;
    resolved->device_id = profile->device_id;
    resolved->progress_store = progress_store;
    resolved->document_path = document_path;
    resolved->dns = runtime->dns;
    resolved->tls = runtime->tls;
    resolved->time_policy = runtime->time_policy;
    resolved->time = runtime->time;
}

cn_sync_controller_result cn_sync_current_book_with_profile(
    const cn_persisted_sync_profile *profile,
    const cn_sync_runtime_inputs *runtime,
    cn_progress_store *progress_store,
    const char *document_path)
{
    cn_sync_controller_result result;
    cn_sync_resolved_config resolved;

    cn_sync_controller_result_init_internal(&result);
    if (!valid_profile(profile) || !valid_device_name(profile->device_name) ||
        !cn_sync_valid_device_id_internal(profile->device_id) ||
        !valid_operation_inputs(runtime, progress_store, document_path)) {
        result.stage = CN_SYNC_CONTROLLER_CONFIG_FAILED;
        result.product.outcome = CN_KOSYNC_PRODUCT_CONFIGURATION_FAILURE;
        result.product.retry = CN_KOSYNC_RETRY_EXPLICIT_ACTION;
        return result;
    }
    resolved_from_profile(&resolved, profile, runtime, progress_store,
                          document_path);
    cn_sync_current_book_resolved(&resolved, &result);
    return result;
}

cn_sync_push_result cn_sync_push_local_current_book_with_profile(
    const cn_persisted_sync_profile *profile,
    const cn_sync_runtime_inputs *runtime,
    cn_progress_store *progress_store,
    const char *document_path)
{
    cn_sync_push_result result;
    cn_sync_resolved_config resolved;

    cn_sync_push_result_init_internal(&result);
    if (!valid_profile(profile) ||
        !cn_sync_valid_device_id_internal(profile->device_id) ||
        !valid_operation_inputs(runtime, progress_store, document_path)) {
        result.stage = CN_SYNC_PUSH_CONFIG_FAILED;
        result.outcome = CN_SYNC_PUSH_CONFIGURATION_FAILURE;
        return result;
    }
    resolved_from_profile(&resolved, profile, runtime, progress_store,
                          document_path);
    cn_sync_push_local_current_book_resolved(&resolved, &result);
    return result;
}

cn_sync_pull_result cn_sync_pull_remote_current_book_with_profile(
    const cn_persisted_sync_profile *profile,
    const cn_sync_runtime_inputs *runtime,
    cn_progress_store *progress_store,
    const char *document_path)
{
    cn_sync_pull_result result;
    cn_sync_resolved_config resolved;

    cn_sync_pull_result_init_internal(&result);
    if (!valid_profile(profile) ||
        !valid_operation_inputs(runtime, progress_store, document_path)) {
        result.stage = CN_SYNC_PULL_CONFIG_FAILED;
        result.outcome = CN_SYNC_PULL_CONFIGURATION_FAILURE;
        return result;
    }
    resolved_from_profile(&resolved, profile, runtime, progress_store,
                          document_path);
    cn_sync_pull_remote_current_book_resolved(&resolved, &result);
    return result;
}

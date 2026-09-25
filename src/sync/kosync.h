/* Bounded KOReader progress-sync protocol core over HTTP or verified HTTPS. */
#ifndef CN_SYNC_KOSYNC_H
#define CN_SYNC_KOSYNC_H

#include <stddef.h>

#include "net/netsimple.h"

struct cn_tls_config;

#define CN_KOSYNC_DOCUMENT_ID_BYTES 32
#define CN_KOSYNC_USERNAME_MAX 255
#define CN_KOSYNC_USERKEY_MAX 255
#define CN_KOSYNC_DEVICE_MAX 255
#define CN_KOSYNC_POSITION_MAX 65536
#define CN_KOSYNC_BASE_PATH_MAX 1024
#define CN_KOSYNC_JSON_MAX (CN_KOSYNC_POSITION_MAX * 6 + 4096)

typedef enum cn_kosync_result {
    CN_KOSYNC_OK = 0,
    CN_KOSYNC_NOT_FOUND,
    CN_KOSYNC_INVALID,
    CN_KOSYNC_AUTH_FAILED,
    CN_KOSYNC_HTTP_ERROR,
    CN_KOSYNC_TRANSPORT_ERROR,
    CN_KOSYNC_BAD_JSON,
    CN_KOSYNC_BAD_PROTOCOL,
    CN_KOSYNC_NO_MEMORY,
    CN_KOSYNC_RESULT_COUNT
} cn_kosync_result;

typedef struct cn_kosync_client {
    char host[CN_NETSIMPLE_HOST_MAX + 1];
    char port[CN_NETSIMPLE_PORT_MAX_TEXT + 1];
    char base_path[CN_KOSYNC_BASE_PATH_MAX + 1];
    char username[CN_KOSYNC_USERNAME_MAX + 1];
    char userkey[CN_KOSYNC_USERKEY_MAX + 1];
    char connect_host[CN_NETSIMPLE_HOST_MAX + 1];
    const struct cn_tls_config *tls;
    int use_tls;
    unsigned connect_ms;
    unsigned recv_ms;
} cn_kosync_client;

typedef struct cn_kosync_progress {
    char document_id[CN_KOSYNC_DOCUMENT_ID_BYTES + 1];
    char *logical_position;
    int progress_10000;
    char device[CN_KOSYNC_DEVICE_MAX + 1];
    char device_id[CN_KOSYNC_DEVICE_MAX + 1];
    long long timestamp;
    int has_timestamp;
} cn_kosync_progress;

typedef struct cn_kosync_outcome {
    int http_status;
    cn_netsimple_result transport_result;
} cn_kosync_outcome;

typedef enum cn_kosync_remote_state {
    CN_KOSYNC_REMOTE_SAME_DEVICE = 0,
    CN_KOSYNC_REMOTE_ALREADY_SYNCED,
    CN_KOSYNC_REMOTE_NEWER,
    CN_KOSYNC_REMOTE_OLDER_OR_EQUAL
} cn_kosync_remote_state;

void cn_kosync_progress_init(cn_kosync_progress *progress);
void cn_kosync_progress_clear(cn_kosync_progress *progress);
/* progress must be initialized before set/parse and cleared when finished.
 * Both operations replace its owned logical_position on success. */
cn_kosync_result cn_kosync_progress_set(cn_kosync_progress *progress,
                                        const char *document_id,
                                        const char *logical_position,
                                        int progress_10000,
                                        const char *device,
                                        const char *device_id);

cn_kosync_result cn_kosync_client_init(cn_kosync_client *client,
                                       const char *base_url,
                                       const char *username,
                                       const char *userkey);
/* tls and every path it references must outlive client and its requests. */
cn_kosync_result cn_kosync_client_set_tls(
    cn_kosync_client *client, const struct cn_tls_config *tls,
    const char *connect_host);

/* Read-only account authorization; does not access document progress. */
cn_kosync_result cn_kosync_authorize(const cn_kosync_client *client,
                                     cn_kosync_outcome *outcome);

cn_kosync_result cn_kosync_serialize_progress(
    const cn_kosync_progress *progress,
    char *json, size_t json_cap, size_t *json_len);
cn_kosync_result cn_kosync_parse_progress(const char *json, size_t json_len,
                                          cn_kosync_progress *progress);

cn_kosync_result cn_kosync_put_progress(const cn_kosync_client *client,
                                        const cn_kosync_progress *progress,
                                        long long *server_timestamp,
                                        cn_kosync_outcome *outcome);
cn_kosync_result cn_kosync_get_progress(const cn_kosync_client *client,
                                        const char *document_id,
                                        cn_kosync_progress *progress,
                                        cn_kosync_outcome *outcome);

cn_kosync_remote_state cn_kosync_classify_remote(
    const cn_kosync_progress *remote,
    const char *local_position, int local_progress_10000,
    long long local_timestamp,
    const char *local_device_model, const char *local_device_id);

const char *cn_kosync_result_name(cn_kosync_result result);

#endif /* CN_SYNC_KOSYNC_H */

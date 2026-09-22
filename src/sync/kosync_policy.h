/* Stable product policy derived from a completed one-shot KOSync result. */
#ifndef CN_SYNC_KOSYNC_POLICY_H
#define CN_SYNC_KOSYNC_POLICY_H

#include "sync/kosync_sync.h"

typedef enum cn_kosync_product_outcome {
    CN_KOSYNC_PRODUCT_DISABLED = 0,
    CN_KOSYNC_PRODUCT_UNCHANGED,
    CN_KOSYNC_PRODUCT_UPLOADED,
    CN_KOSYNC_PRODUCT_IMPORTED,
    CN_KOSYNC_PRODUCT_NO_STATE,
    CN_KOSYNC_PRODUCT_CONFLICT,
    CN_KOSYNC_PRODUCT_AUTH_REQUIRED,
    CN_KOSYNC_PRODUCT_TRUSTED_TIME_UNAVAILABLE,
    CN_KOSYNC_PRODUCT_CONNECTIVITY_FAILURE,
    CN_KOSYNC_PRODUCT_SECURITY_FAILURE,
    CN_KOSYNC_PRODUCT_SERVICE_FAILURE,
    CN_KOSYNC_PRODUCT_LOCAL_UNSUPPORTED,
    CN_KOSYNC_PRODUCT_LOCAL_FAILURE,
    CN_KOSYNC_PRODUCT_CONFIGURATION_FAILURE,
    CN_KOSYNC_PRODUCT_INTERNAL_FAILURE,
    CN_KOSYNC_PRODUCT_OUTCOME_COUNT
} cn_kosync_product_outcome;

typedef enum cn_kosync_retry_policy {
    CN_KOSYNC_RETRY_NONE = 0,
    CN_KOSYNC_RETRY_AUTOMATIC_LATER,
    CN_KOSYNC_RETRY_EXPLICIT_ACTION,
    CN_KOSYNC_RETRY_POLICY_COUNT
} cn_kosync_retry_policy;

typedef enum cn_kosync_mutation_state {
    CN_KOSYNC_MUTATION_NONE = 0,
    CN_KOSYNC_MUTATION_CONFIRMED,
    CN_KOSYNC_MUTATION_POSSIBLE,
    CN_KOSYNC_MUTATION_STATE_COUNT
} cn_kosync_mutation_state;

typedef struct cn_kosync_product_result {
    cn_kosync_product_outcome outcome;
    cn_kosync_retry_policy retry;
    cn_kosync_mutation_state local_mutation;
    cn_kosync_mutation_state remote_mutation;
} cn_kosync_product_result;

int cn_kosync_policy_classify(int enabled,
                              const cn_kosync_sync_result *sync,
                              cn_kosync_product_result *result);

const char *cn_kosync_product_outcome_name(
    cn_kosync_product_outcome outcome);
const char *cn_kosync_retry_policy_name(cn_kosync_retry_policy retry);
const char *cn_kosync_mutation_state_name(
    cn_kosync_mutation_state mutation);

#endif /* CN_SYNC_KOSYNC_POLICY_H */

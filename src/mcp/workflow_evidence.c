/* workflow_evidence.c — read-only generation, freshness, and path-coverage gate. */
#include "mcp/workflow_evidence.h"

#include "foundation/compat_fs.h"
#include "foundation/constants.h"
#include "foundation/sha256.h"
#include "mcp/compact_out.h"

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Security-hardened real-path containment is owned by mcp.c today. Keeping one
 * implementation avoids weakening symlink and Windows case-folding checks. */
bool cbm_path_within_root(const char *root_path, const char *abs_path);

static int64_t workflow_stat_mtime_ns(const struct stat *st) {
#ifdef __APPLE__
    return ((int64_t)st->st_mtimespec.tv_sec * (int64_t)CBM_NSEC_PER_SEC) +
           (int64_t)st->st_mtimespec.tv_nsec;
#elif defined(_WIN32)
    return (int64_t)st->st_mtime * (int64_t)CBM_NSEC_PER_SEC;
#else
    return ((int64_t)st->st_mtim.tv_sec * (int64_t)CBM_NSEC_PER_SEC) +
           (int64_t)st->st_mtim.tv_nsec;
#endif
}

static bool workflow_rel_path_is_normalized(const char *path) {
    if (!path || !path[0] || path[0] == '/' || path[0] == '\\' ||
        (isalpha((unsigned char)path[0]) && path[1] == ':')) {
        return false;
    }
    const char *component = path;
    for (const char *p = path;; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '\\' || (c != 0 && (c < 0x20U || c == 0x7fU))) {
            return false;
        }
        if (c != '/' && c != 0) {
            continue;
        }
        size_t length = (size_t)(p - component);
        if (length == 0U || (length == 1U && component[0] == '.') ||
            (length == 2U && component[0] == '.' && component[1] == '.')) {
            return false;
        }
        if (c == 0) {
            return true;
        }
        component = p + 1;
    }
}

static bool workflow_sha256_is_valid(const char *digest) {
    if (!digest || strlen(digest) != CBM_SHA256_HEX_LEN) {
        return false;
    }
    for (size_t i = 0; i < CBM_SHA256_HEX_LEN; i++) {
        if (!isxdigit((unsigned char)digest[i])) {
            return false;
        }
    }
    return true;
}

/* Hash one stable source generation. A concurrent edit is retried once and
 * then fails closed; no mixed before/after content can prove full coverage. */
static bool workflow_hash_file_stable(const char *path,
                                      char digest[CBM_SHA256_HEX_LEN + 1U]) {
    for (int attempt = 0; attempt < 2; attempt++) {
        cbm_path_info_t before;
        if (cbm_path_info_utf8(path, &before) != 0 || !before.is_regular ||
            before.is_symlink) {
            return false;
        }
        FILE *file = cbm_fopen(path, "rb");
        if (!file) {
            return false;
        }
        cbm_sha256_ctx sha;
        cbm_sha256_init(&sha);
        unsigned char buffer[CBM_SZ_64K];
        size_t count;
        while ((count = fread(buffer, 1, sizeof(buffer), file)) > 0U) {
            cbm_sha256_update(&sha, buffer, count);
        }
        bool read_ok = !ferror(file);
        if (fclose(file) != 0) {
            read_ok = false;
        }
        cbm_path_info_t after;
        if (!read_ok || cbm_path_info_utf8(path, &after) != 0 || !after.is_regular ||
            after.is_symlink) {
            return false;
        }
        if (before.size != after.size || before.mtime_ns != after.mtime_ns) {
            continue;
        }
        uint8_t raw[CBM_SHA256_DIGEST_LEN];
        cbm_sha256_final(&sha, raw);
        static const char hex[] = "0123456789abcdef";
        for (size_t i = 0; i < CBM_SHA256_DIGEST_LEN; i++) {
            digest[i * 2U] = hex[raw[i] >> 4U];
            digest[i * 2U + 1U] = hex[raw[i] & 0x0fU];
        }
        digest[CBM_SHA256_HEX_LEN] = 0;
        return true;
    }
    return false;
}

static bool workflow_sha256_equal(const char *left, const char *right) {
    for (size_t i = 0; i < CBM_SHA256_HEX_LEN; i++) {
        if (tolower((unsigned char)left[i]) != tolower((unsigned char)right[i])) {
            return false;
        }
    }
    return true;
}

static const char *workflow_path_freshness(cbm_workflow_evidence_gate_t *gate,
                                           const char *rel_path, bool *outside,
                                           bool *lookup_ok, bool *metadata_matches,
                                           bool *content_hash_available,
                                           bool *content_hash_matches) {
    *outside = false;
    *lookup_ok = true;
    *metadata_matches = false;
    *content_hash_available = false;
    *content_hash_matches = false;
    const char *root_path =
        gate->have_project ? gate->project_record.root_path : NULL;
    if (!root_path || !root_path[0]) {
        *lookup_ok = false;
        return "unavailable";
    }

    char abs_path[CBM_SZ_4K];
    int n = snprintf(abs_path, sizeof(abs_path), "%s%s%s", root_path,
                     root_path[strlen(root_path) - 1U] == '/' ? "" : "/", rel_path);
    if (n < 0 || (size_t)n >= sizeof(abs_path)) {
        *lookup_ok = false;
        return "unavailable";
    }

    if (!cbm_path_within_root(root_path, abs_path)) {
        errno = 0;
        struct stat containment_probe;
        if (stat(abs_path, &containment_probe) != 0) {
            if (errno == ENOENT || errno == ENOTDIR) {
                return "missing";
            }
            *lookup_ok = false;
            return "unavailable";
        }
        *outside = true;
        return "outside_project";
    }

    errno = 0;
    struct stat st;
    if (stat(abs_path, &st) != 0) {
        if (errno == ENOENT || errno == ENOTDIR) {
            return "missing";
        }
        *lookup_ok = false;
        return "unavailable";
    }

    cbm_file_hash_t hash = {0};
    int rc = cbm_store_get_file_hash(gate->store, gate->project, rel_path, &hash);
    if (rc == CBM_STORE_NOT_FOUND) {
        return "not_tracked";
    }
    if (rc != CBM_STORE_OK) {
        *lookup_ok = false;
        return "unavailable";
    }
    bool matches = hash.mtime_ns == workflow_stat_mtime_ns(&st) && hash.size == st.st_size;
    *metadata_matches = matches;
    if (matches && gate->have_coverage_meta && gate->coverage_meta.hash_records_complete) {
        if (!workflow_sha256_is_valid(hash.sha256)) {
            cbm_store_clear_file_hash(&hash);
            *lookup_ok = false;
            return "unavailable";
        }
        *content_hash_available = true;
        char digest[CBM_SHA256_HEX_LEN + 1U];
        if (!workflow_hash_file_stable(abs_path, digest)) {
            cbm_store_clear_file_hash(&hash);
            *lookup_ok = false;
            return "unavailable";
        }
        *content_hash_matches = workflow_sha256_equal(hash.sha256, digest);
        if (!*content_hash_matches) {
            matches = false;
        }
    }
    cbm_store_clear_file_hash(&hash);
    return matches ? "metadata_match" : "metadata_changed";
}

static const char *workflow_path_status(const cbm_coverage_row_t *rows, int count,
                                        const char *requested_path,
                                        const char *recording_status,
                                        bool generation_authoritative, bool lookup_ok,
                                        bool content_verified, const char *freshness) {
    if (!lookup_ok) {
        return "coverage_unavailable";
    }
    bool exact = false;
    for (int i = 0; i < count; i++) {
        if (rows[i].rel_path && strcmp(rows[i].rel_path, requested_path) == 0) {
            exact = true;
            break;
        }
    }
    /* Explicit gaps remain useful even on an older generation, but can never
     * authorize an empty answer. The aggregate gate freshness carries stale. */
    for (int pass = 0; pass < 3; pass++) {
        for (int i = 0; i < count; i++) {
            if (exact && (!rows[i].rel_path || strcmp(rows[i].rel_path, requested_path) != 0)) {
                continue;
            }
            const char *kind = rows[i].kind ? rows[i].kind : "";
            if (pass == 0 && strcmp(kind, "parse_partial") == 0) {
                return "partial";
            }
            if (pass == 1 && strncmp(kind, "not_indexed", 11) == 0) {
                return "excluded";
            }
            if (pass == 2 && kind[0]) {
                return "skipped";
            }
        }
    }

    bool source_fresh = freshness && strcmp(freshness, "metadata_match") == 0;
    bool recording_complete = recording_status && strcmp(recording_status, "complete") == 0;
    bool truncated_exact_path_verified =
        content_verified && recording_status && strcmp(recording_status, "truncated") == 0;
    if (!generation_authoritative || !source_fresh || !content_verified ||
        (!recording_complete && !truncated_exact_path_verified)) {
        return "coverage_unavailable";
    }
    return "no_recorded_issue";
}

static cbm_workflow_freshness_t workflow_source_freshness(const char *detail) {
    if (detail && strcmp(detail, "metadata_match") == 0) {
        return CBM_WORKFLOW_FRESHNESS_FRESH;
    }
    if (detail && strcmp(detail, "metadata_changed") == 0) {
        return CBM_WORKFLOW_FRESHNESS_DIRTY;
    }
    return CBM_WORKFLOW_FRESHNESS_UNKNOWN;
}

static int workflow_freshness_rank(cbm_workflow_freshness_t freshness) {
    switch (freshness) {
    case CBM_WORKFLOW_FRESHNESS_STALE:
        return 3;
    case CBM_WORKFLOW_FRESHNESS_DIRTY:
        return 2;
    case CBM_WORKFLOW_FRESHNESS_UNKNOWN:
        return 1;
    case CBM_WORKFLOW_FRESHNESS_FRESH:
    default:
        return 0;
    }
}

static void workflow_merge_freshness(cbm_workflow_evidence_gate_t *gate,
                                     cbm_workflow_freshness_t freshness) {
    if (gate->requested == 0U) {
        gate->freshness = freshness;
        return;
    }
    if (workflow_freshness_rank(freshness) > workflow_freshness_rank(gate->freshness)) {
        gate->freshness = freshness;
    }
}

static cbm_workflow_path_state_t
workflow_path_state(const cbm_workflow_path_evidence_t *evidence) {
    if (evidence->outside) {
        return CBM_WORKFLOW_PATH_OUTSIDE_PROJECT;
    }
    if (!evidence->coverage_lookup_ok || !evidence->source_lookup_ok) {
        return CBM_WORKFLOW_PATH_LOOKUP_FAILED;
    }
    if (strcmp(evidence->status, "partial") == 0) {
        return CBM_WORKFLOW_PATH_PARTIAL;
    }
    if (strcmp(evidence->status, "excluded") == 0) {
        return CBM_WORKFLOW_PATH_IGNORED;
    }
    if (strcmp(evidence->status, "skipped") == 0) {
        return CBM_WORKFLOW_PATH_UNSUPPORTED;
    }
    if (strcmp(evidence->freshness_detail, "missing") == 0 ||
        strcmp(evidence->freshness_detail, "not_tracked") == 0) {
        return CBM_WORKFLOW_PATH_ABSENT;
    }
    return CBM_WORKFLOW_PATH_INDEXED;
}

int cbm_workflow_evidence_gate_begin(cbm_store_t *store, const char *project,
                                     cbm_workflow_evidence_gate_t *gate) {
    if (!store || !project || !gate) {
        return CBM_STORE_ERR;
    }
    memset(gate, 0, sizeof(*gate));
    gate->store = store;
    gate->project = project;
    gate->freshness = CBM_WORKFLOW_FRESHNESS_UNKNOWN;
    if (cbm_store_exec(store, "BEGIN;") != CBM_STORE_OK) {
        return CBM_STORE_ERR;
    }
    gate->snapshot_active = true;
    if (cbm_store_generation(store, gate->store_generation,
                             sizeof(gate->store_generation)) != CBM_STORE_OK) {
        (void)cbm_workflow_evidence_gate_end(gate);
        return CBM_STORE_ERR;
    }

    int project_rc = cbm_store_get_project(store, project, &gate->project_record);
    if (project_rc != CBM_STORE_OK && project_rc != CBM_STORE_NOT_FOUND) {
        (void)cbm_workflow_evidence_gate_end(gate);
        return CBM_STORE_ERR;
    }
    gate->have_project = project_rc == CBM_STORE_OK;

    int meta_rc = cbm_store_coverage_meta_get(store, project, &gate->coverage_meta);
    if (meta_rc != CBM_STORE_OK && meta_rc != CBM_STORE_NOT_FOUND) {
        (void)cbm_workflow_evidence_gate_end(gate);
        return CBM_STORE_ERR;
    }
    gate->have_coverage_meta = meta_rc == CBM_STORE_OK;
    bool generation_known =
        gate->have_project && gate->have_coverage_meta && gate->project_record.indexed_at &&
        gate->project_record.indexed_at[0] && gate->coverage_meta.generation &&
        gate->coverage_meta.generation[0];
    gate->generation_matches =
        generation_known &&
        strcmp(gate->project_record.indexed_at, gate->coverage_meta.generation) == 0;
    gate->generation_state =
        !generation_known                ? CBM_WORKFLOW_GENERATION_UNKNOWN
        : gate->generation_matches       ? CBM_WORKFLOW_GENERATION_MATCH
                                         : CBM_WORKFLOW_GENERATION_MISMATCH;
    if (gate->generation_state == CBM_WORKFLOW_GENERATION_MISMATCH) {
        gate->freshness = CBM_WORKFLOW_FRESHNESS_STALE;
    }
    return CBM_STORE_OK;
}

const char *cbm_workflow_evidence_recording_status(
    const cbm_workflow_evidence_gate_t *gate) {
    return gate && gate->have_coverage_meta && gate->coverage_meta.recording_status
               ? gate->coverage_meta.recording_status
               : "unknown";
}

int cbm_workflow_evidence_gate_check_path(cbm_workflow_evidence_gate_t *gate,
                                          const char *rel_path,
                                          cbm_workflow_path_evidence_t *evidence) {
    if (!gate || !gate->snapshot_active || !workflow_rel_path_is_normalized(rel_path) ||
        !evidence) {
        return CBM_STORE_ERR;
    }
    memset(evidence, 0, sizeof(*evidence));
    int coverage_rc = cbm_store_coverage_get_path(gate->store, gate->project, rel_path,
                                                  &evidence->rows, &evidence->row_count);
    evidence->coverage_lookup_ok =
        coverage_rc == CBM_STORE_OK || coverage_rc == CBM_STORE_NOT_FOUND;
    if (!evidence->coverage_lookup_ok) {
        evidence->row_count = 0;
    }
    evidence->freshness_detail = workflow_path_freshness(
        gate, rel_path, &evidence->outside, &evidence->source_lookup_ok,
        &evidence->metadata_matches, &evidence->content_hash_available,
        &evidence->content_hash_matches);

    bool store_generation_available = strcmp(gate->store_generation, "legacy") != 0;
    bool generation_authoritative =
        gate->generation_state == CBM_WORKFLOW_GENERATION_MATCH &&
        store_generation_available;
    bool content_verified =
        gate->have_coverage_meta && gate->coverage_meta.hash_records_complete &&
        evidence->content_hash_matches;
    bool lookup_ok = evidence->coverage_lookup_ok && evidence->source_lookup_ok;
    evidence->status =
        evidence->outside
            ? "outside_project"
            : workflow_path_status(evidence->rows, evidence->row_count, rel_path,
                                   cbm_workflow_evidence_recording_status(gate),
                                   generation_authoritative, lookup_ok, content_verified,
                                   evidence->freshness_detail);
    evidence->freshness =
        gate->generation_state == CBM_WORKFLOW_GENERATION_MATCH
            ? workflow_source_freshness(evidence->freshness_detail)
        : gate->generation_state == CBM_WORKFLOW_GENERATION_MISMATCH
            ? CBM_WORKFLOW_FRESHNESS_STALE
            : CBM_WORKFLOW_FRESHNESS_UNKNOWN;
    evidence->state = workflow_path_state(evidence);
    evidence->full_coverage =
        content_verified && strcmp(evidence->status, "no_recorded_issue") == 0;

    workflow_merge_freshness(gate, evidence->freshness);
    gate->requested++;
    if (evidence->full_coverage) {
        gate->covered++;
    }
    switch (evidence->state) {
    case CBM_WORKFLOW_PATH_PARTIAL:
        gate->partial++;
        break;
    case CBM_WORKFLOW_PATH_IGNORED:
        gate->ignored++;
        break;
    case CBM_WORKFLOW_PATH_UNSUPPORTED:
        gate->unsupported++;
        break;
    case CBM_WORKFLOW_PATH_ABSENT:
    case CBM_WORKFLOW_PATH_OUTSIDE_PROJECT:
        gate->absent++;
        break;
    case CBM_WORKFLOW_PATH_LOOKUP_FAILED:
        gate->lookup_failed++;
        break;
    case CBM_WORKFLOW_PATH_INDEXED:
    default:
        break;
    }
    return CBM_STORE_OK;
}

void cbm_workflow_evidence_path_clear(cbm_workflow_path_evidence_t *evidence) {
    if (!evidence) {
        return;
    }
    cbm_store_free_coverage(evidence->rows, evidence->row_count);
    memset(evidence, 0, sizeof(*evidence));
}

int cbm_workflow_evidence_gate_end(cbm_workflow_evidence_gate_t *gate) {
    if (!gate) {
        return CBM_STORE_ERR;
    }
    int rc = CBM_STORE_OK;
    if (gate->snapshot_active &&
        cbm_store_exec(gate->store, "ROLLBACK;") != CBM_STORE_OK) {
        rc = CBM_STORE_ERR;
    }
    cbm_store_coverage_meta_clear(&gate->coverage_meta);
    cbm_project_free_fields(&gate->project_record);
    memset(gate, 0, sizeof(*gate));
    return rc;
}

enum {
    WORKFLOW_RENDER_OMISSION_RESERVE = 1024,
    WORKFLOW_RENDER_TEXT_BYTES = 512,
    WORKFLOW_RENDER_SNIPPET_BYTES = 1024,
    WORKFLOW_RENDER_CURSOR_BYTES = 96,
};

static const char *workflow_outcome_name(cbm_workflow_outcome_t outcome) {
    switch (outcome) {
    case CBM_WORKFLOW_OUTCOME_COMPLETE:
        return "complete";
    case CBM_WORKFLOW_OUTCOME_PARTIAL:
        return "partial";
    case CBM_WORKFLOW_OUTCOME_EMPTY_VERIFIED:
        return "empty_verified";
    case CBM_WORKFLOW_OUTCOME_AMBIGUOUS:
        return "ambiguous";
    case CBM_WORKFLOW_OUTCOME_STALE:
        return "stale";
    case CBM_WORKFLOW_OUTCOME_UNSUPPORTED:
        return "unsupported";
    case CBM_WORKFLOW_OUTCOME_ERROR:
    default:
        return "error";
    }
}

static const char *workflow_freshness_name(cbm_workflow_freshness_t freshness) {
    switch (freshness) {
    case CBM_WORKFLOW_FRESHNESS_FRESH:
        return "fresh";
    case CBM_WORKFLOW_FRESHNESS_DIRTY:
        return "dirty";
    case CBM_WORKFLOW_FRESHNESS_STALE:
        return "stale";
    case CBM_WORKFLOW_FRESHNESS_UNKNOWN:
    default:
        return "unknown";
    }
}

static const char *workflow_risk_name(cbm_workflow_risk_level_t risk) {
    switch (risk) {
    case CBM_WORKFLOW_RISK_LOW:
        return "low";
    case CBM_WORKFLOW_RISK_MEDIUM:
        return "medium";
    case CBM_WORKFLOW_RISK_HIGH:
        return "high";
    case CBM_WORKFLOW_RISK_CRITICAL:
        return "critical";
    case CBM_WORKFLOW_RISK_UNKNOWN:
    default:
        return "unknown";
    }
}

static const char *workflow_confidence_name(cbm_workflow_confidence_level_t confidence) {
    switch (confidence) {
    case CBM_WORKFLOW_CONFIDENCE_LOW:
        return "low";
    case CBM_WORKFLOW_CONFIDENCE_MEDIUM:
        return "medium";
    case CBM_WORKFLOW_CONFIDENCE_HIGH:
        return "high";
    case CBM_WORKFLOW_CONFIDENCE_UNKNOWN:
    default:
        return "unknown";
    }
}

static void workflow_hash_field(cbm_sha256_ctx *sha, const char *value) {
    size_t length = value ? strlen(value) : 0U;
    uint8_t encoded[8];
    uint64_t size = (uint64_t)length;
    for (size_t i = 0; i < sizeof(encoded); i++) {
        encoded[sizeof(encoded) - i - 1U] = (uint8_t)(size & 0xffU);
        size >>= 8U;
    }
    cbm_sha256_update(sha, encoded, sizeof(encoded));
    if (length > 0U) {
        cbm_sha256_update(sha, value, length);
    }
}

static void workflow_digest_handle(const char *prefix, const uint8_t *digest,
                                   size_t digest_bytes, char *out) {
    static const char hex[] = "0123456789abcdef";
    size_t prefix_length = strlen(prefix);
    memcpy(out, prefix, prefix_length);
    for (size_t i = 0; i < digest_bytes; i++) {
        out[prefix_length + i * 2U] = hex[digest[i] >> 4U];
        out[prefix_length + i * 2U + 1U] = hex[digest[i] & 0x0fU];
    }
    out[prefix_length + digest_bytes * 2U] = 0;
}

bool cbm_workflow_context_handle(char *out, size_t out_size, const char *project_id,
                                 const char *generation, const char *tool,
                                 const char *normalized_query) {
    if (!out || out_size < CBM_WORKFLOW_CONTEXT_HANDLE_SIZE || !project_id ||
        !project_id[0] || !generation || !generation[0] || !tool || !tool[0] ||
        !normalized_query || !normalized_query[0]) {
        if (out && out_size > 0U) {
            out[0] = 0;
        }
        return false;
    }
    cbm_sha256_ctx sha;
    cbm_sha256_init(&sha);
    workflow_hash_field(&sha, "cbm.workflow.context.v1");
    workflow_hash_field(&sha, project_id);
    workflow_hash_field(&sha, generation);
    workflow_hash_field(&sha, tool);
    workflow_hash_field(&sha, normalized_query);
    uint8_t digest[CBM_SHA256_DIGEST_LEN];
    cbm_sha256_final(&sha, digest);
    workflow_digest_handle("wf1:", digest, 16U, out);
    return true;
}

void cbm_workflow_envelope_init(cbm_workflow_envelope_t *envelope) {
    if (!envelope) {
        return;
    }
    memset(envelope, 0, sizeof(*envelope));
    envelope->outcome = CBM_WORKFLOW_OUTCOME_ERROR;
    envelope->index.freshness = CBM_WORKFLOW_FRESHNESS_UNKNOWN;
    envelope->risk.level = CBM_WORKFLOW_RISK_UNKNOWN;
    envelope->confidence.level = CBM_WORKFLOW_CONFIDENCE_UNKNOWN;
}

static size_t workflow_saturated_add(size_t left, size_t right) {
    return right > SIZE_MAX - left ? SIZE_MAX : left + right;
}

bool cbm_workflow_text_list_add(cbm_workflow_text_list_t *list, const char *text) {
    if (!list || !text) {
        return false;
    }
    if (list->count >= CBM_WORKFLOW_TEXT_MAX_ITEMS) {
        list->remaining = workflow_saturated_add(list->remaining, 1U);
        return false;
    }
    list->items[list->count++] = text;
    return true;
}

static void workflow_evidence_id(const cbm_workflow_envelope_t *envelope,
                                 cbm_workflow_evidence_item_t *item) {
    cbm_sha256_ctx sha;
    cbm_sha256_init(&sha);
    workflow_hash_field(&sha, "cbm.workflow.evidence.v1");
    workflow_hash_field(&sha, envelope->project.id);
    workflow_hash_field(&sha, envelope->index.generation);
    workflow_hash_field(&sha, item->path);
    workflow_hash_field(&sha, item->qualified_name);
    workflow_hash_field(&sha, item->target_path);
    workflow_hash_field(&sha, item->target_qualified_name);
    workflow_hash_field(&sha, item->relationship);
    workflow_hash_field(&sha, item->source_generation);
    char numbers[96];
    (void)snprintf(numbers, sizeof(numbers), "%d:%d:%d", item->distance, item->line_start,
                   item->line_end);
    workflow_hash_field(&sha, numbers);
    uint8_t digest[CBM_SHA256_DIGEST_LEN];
    cbm_sha256_final(&sha, digest);
    workflow_digest_handle("ev1:", digest, 16U, item->id);
}

bool cbm_workflow_envelope_add_evidence(
    cbm_workflow_envelope_t *envelope, const cbm_workflow_evidence_item_t *item) {
    if (!envelope || !item) {
        return false;
    }
    if (envelope->evidence_count >= CBM_WORKFLOW_EVIDENCE_MAX_ITEMS) {
        envelope->evidence_remaining =
            workflow_saturated_add(envelope->evidence_remaining, 1U);
        return false;
    }
    cbm_workflow_evidence_item_t *stored =
        &envelope->evidence[envelope->evidence_count++];
    *stored = *item;
    stored->id[sizeof(stored->id) - 1U] = 0;
    if (!stored->id[0]) {
        workflow_evidence_id(envelope, stored);
    }
    if (!(stored->rank == stored->rank)) {
        stored->rank = 0.0;
    }
    return true;
}

static int workflow_normalized_path_compare(const char *left, const char *right) {
    const unsigned char *a = (const unsigned char *)(left ? left : "");
    const unsigned char *b = (const unsigned char *)(right ? right : "");
    while (*a || *b) {
        unsigned char ac = *a == '\\' ? '/' : *a;
        unsigned char bc = *b == '\\' ? '/' : *b;
        if (ac != bc) {
            return ac < bc ? -1 : 1;
        }
        if (*a) {
            a++;
        }
        if (*b) {
            b++;
        }
    }
    return 0;
}

static int workflow_string_compare(const char *left, const char *right) {
    return strcmp(left ? left : "", right ? right : "");
}

static int workflow_evidence_compare(const void *left_ptr, const void *right_ptr) {
    const cbm_workflow_evidence_item_t *left = left_ptr;
    const cbm_workflow_evidence_item_t *right = right_ptr;
    if (left->rank != right->rank) {
        return left->rank > right->rank ? -1 : 1;
    }
    int compared = workflow_normalized_path_compare(left->path, right->path);
    if (compared != 0) {
        return compared;
    }
    compared = workflow_string_compare(left->qualified_name, right->qualified_name);
    if (compared != 0) {
        return compared;
    }
    compared = workflow_string_compare(left->relationship, right->relationship);
    return compared != 0 ? compared : workflow_string_compare(left->id, right->id);
}

static const char *workflow_bounded_text(const char *text, char *buffer, size_t buffer_size,
                                         size_t max_bytes) {
    const char *source = text ? text : "";
    size_t length = strlen(source);
    size_t limit = max_bytes;
    if (limit + 1U > buffer_size) {
        limit = buffer_size - 1U;
    }
    if (length <= limit) {
        memcpy(buffer, source, length + 1U);
        return buffer;
    }
    size_t prefix = limit > 3U ? limit - 3U : 0U;
    memcpy(buffer, source, prefix);
    memcpy(buffer + prefix, "...", 3U);
    buffer[prefix + 3U] = 0;
    return buffer;
}

static void workflow_tree_scalar_text(cbm_sb_t *output, const char *key, const char *value) {
    char bounded[WORKFLOW_RENDER_TEXT_BYTES + 1U];
    cbm_tree_scalar_str(
        output, key,
        workflow_bounded_text(value, bounded, sizeof(bounded), WORKFLOW_RENDER_TEXT_BYTES));
}

static void workflow_tree_scalar_size(cbm_sb_t *output, const char *key, size_t value) {
    long long bounded = value > (size_t)INT64_MAX ? INT64_MAX : (long long)value;
    cbm_tree_scalar_int(output, key, bounded);
}

static char *workflow_render_base(const cbm_workflow_envelope_t *envelope,
                                  bool include_snippets) {
    cbm_sb_t output;
    cbm_sb_init(&output);
    cbm_tree_scalar_int(&output, "schema_version", 1);
    cbm_tree_scalar_str(&output, "outcome", workflow_outcome_name(envelope->outcome));
    workflow_tree_scalar_text(&output, "project_id", envelope->project.id);
    workflow_tree_scalar_text(&output, "project_name", envelope->project.display_name);
    workflow_tree_scalar_text(&output, "canonical_root", envelope->project.canonical_root);
    workflow_tree_scalar_text(&output, "project_resolution", envelope->project.resolution);
    workflow_tree_scalar_text(&output, "index_generation", envelope->index.generation);
    workflow_tree_scalar_text(&output, "store_generation", envelope->index.store_generation);
    workflow_tree_scalar_text(&output, "source_fingerprint",
                              envelope->index.source_fingerprint);
    cbm_tree_scalar_str(&output, "freshness",
                        workflow_freshness_name(envelope->index.freshness));
    workflow_tree_scalar_size(&output, "coverage_requested", envelope->coverage.requested);
    workflow_tree_scalar_size(&output, "coverage_covered", envelope->coverage.covered);
    cbm_tree_scalar_str(&output, "risk_level", workflow_risk_name(envelope->risk.level));
    cbm_tree_scalar_str(&output, "confidence_level",
                        workflow_confidence_name(envelope->confidence.level));
    workflow_tree_scalar_text(&output, "context_handle", envelope->context_handle);
    cbm_tree_scalar_bool(&output, "snippets_included", include_snippets);
    return cbm_sb_finish(&output);
}

static char *workflow_render_text_section(const char *key,
                                          const cbm_workflow_text_list_t *list,
                                          size_t emit_count) {
    static const char *columns[] = {"value"};
    cbm_sb_t output;
    cbm_sb_init(&output);
    cbm_tree_table_header(&output, key, (int)emit_count, columns, 1);
    for (size_t i = 0; i < emit_count; i++) {
        char bounded[WORKFLOW_RENDER_TEXT_BYTES + 1U];
        cbm_tree_row_begin(&output);
        cbm_tree_cell_str(
            &output,
            workflow_bounded_text(list->items[i], bounded, sizeof(bounded),
                                  WORKFLOW_RENDER_TEXT_BYTES),
            true);
        cbm_tree_row_end(&output);
    }
    return cbm_sb_finish(&output);
}

static bool workflow_append_text_section(cbm_sb_t *output, size_t body_limit,
                                         const char *key,
                                         const cbm_workflow_text_list_t *list,
                                         size_t *remaining_out) {
    size_t count = list->count;
    size_t hidden = list->remaining;
    if (count > CBM_WORKFLOW_TEXT_MAX_ITEMS) {
        hidden = workflow_saturated_add(hidden, count - CBM_WORKFLOW_TEXT_MAX_ITEMS);
        count = CBM_WORKFLOW_TEXT_MAX_ITEMS;
    }
    *remaining_out = hidden;
    if (count == 0U) {
        return true;
    }
    for (size_t emit_count = count;; emit_count--) {
        char *section = workflow_render_text_section(key, list, emit_count);
        if (!section) {
            return false;
        }
        size_t length = strlen(section);
        bool appended =
            cbm_sb_append_n_bounded(output, section, length, body_limit);
        free(section);
        if (appended) {
            *remaining_out += count - emit_count;
            return true;
        }
        if (output->oom) {
            return false;
        }
        if (emit_count == 0U) {
            *remaining_out += count;
            return true;
        }
    }
}

static void workflow_render_evidence_row(cbm_sb_t *output,
                                         const cbm_workflow_evidence_item_t *item,
                                         bool include_snippets) {
    char path[WORKFLOW_RENDER_TEXT_BYTES + 1U];
    char qualified_name[WORKFLOW_RENDER_TEXT_BYTES + 1U];
    char target_path[WORKFLOW_RENDER_TEXT_BYTES + 1U];
    char target_name[WORKFLOW_RENDER_TEXT_BYTES + 1U];
    char relationship[WORKFLOW_RENDER_TEXT_BYTES + 1U];
    char reason[WORKFLOW_RENDER_TEXT_BYTES + 1U];
    char generation[WORKFLOW_RENDER_TEXT_BYTES + 1U];
    cbm_tree_row_begin(output);
    cbm_tree_cell_str(output, item->id, true);
    cbm_tree_cell_str(
        output,
        workflow_bounded_text(item->path, path, sizeof(path), WORKFLOW_RENDER_TEXT_BYTES),
        false);
    cbm_tree_cell_str(output,
                      workflow_bounded_text(item->qualified_name, qualified_name,
                                            sizeof(qualified_name), WORKFLOW_RENDER_TEXT_BYTES),
                      false);
    cbm_tree_cell_str(output,
                      workflow_bounded_text(item->target_path, target_path,
                                            sizeof(target_path), WORKFLOW_RENDER_TEXT_BYTES),
                      false);
    cbm_tree_cell_str(output,
                      workflow_bounded_text(item->target_qualified_name, target_name,
                                            sizeof(target_name), WORKFLOW_RENDER_TEXT_BYTES),
                      false);
    cbm_tree_cell_str(output,
                      workflow_bounded_text(item->relationship, relationship,
                                            sizeof(relationship), WORKFLOW_RENDER_TEXT_BYTES),
                      false);
    cbm_tree_cell_int(output, item->distance, false);
    cbm_tree_cell_real(output, item->rank, false);
    cbm_tree_cell_str(
        output,
        workflow_bounded_text(item->reason, reason, sizeof(reason), WORKFLOW_RENDER_TEXT_BYTES),
        false);
    cbm_tree_cell_str(output,
                      workflow_bounded_text(item->source_generation, generation,
                                            sizeof(generation), WORKFLOW_RENDER_TEXT_BYTES),
                      false);
    cbm_tree_cell_int(output, item->line_start, false);
    cbm_tree_cell_int(output, item->line_end, false);
    if (include_snippets) {
        char snippet[WORKFLOW_RENDER_SNIPPET_BYTES + 1U];
        cbm_tree_cell_str(output,
                          workflow_bounded_text(item->snippet, snippet, sizeof(snippet),
                                                WORKFLOW_RENDER_SNIPPET_BYTES),
                          false);
    }
    cbm_tree_row_end(output);
}

static char *workflow_render_evidence_section(
    const cbm_workflow_evidence_item_t *items, size_t emit_count, bool include_snippets) {
    static const char *compact_columns[] = {
        "id",          "path",       "qualified_name", "target_path",
        "target_name", "relation",   "distance",       "rank",
        "reason",      "generation", "line_start",     "line_end",
    };
    static const char *detailed_columns[] = {
        "id",          "path",       "qualified_name", "target_path", "target_name",
        "relation",    "distance",   "rank",           "reason",      "generation",
        "line_start",  "line_end",   "snippet",
    };
    cbm_sb_t output;
    cbm_sb_init(&output);
    const char *const *columns = include_snippets ? detailed_columns : compact_columns;
    int column_count = include_snippets ? 13 : 12;
    cbm_tree_table_header(&output, "evidence", (int)emit_count, columns, column_count);
    for (size_t i = 0; i < emit_count; i++) {
        workflow_render_evidence_row(&output, &items[i], include_snippets);
    }
    return cbm_sb_finish(&output);
}

static bool workflow_append_evidence(cbm_sb_t *output, size_t body_limit,
                                     const cbm_workflow_envelope_t *envelope,
                                     bool include_snippets, size_t *remaining_out) {
    size_t count = envelope->evidence_count;
    size_t hidden = envelope->evidence_remaining;
    if (count > CBM_WORKFLOW_EVIDENCE_MAX_ITEMS) {
        hidden = workflow_saturated_add(hidden, count - CBM_WORKFLOW_EVIDENCE_MAX_ITEMS);
        count = CBM_WORKFLOW_EVIDENCE_MAX_ITEMS;
    }
    cbm_workflow_evidence_item_t sorted[CBM_WORKFLOW_EVIDENCE_MAX_ITEMS];
    if (count > 0U) {
        memcpy(sorted, envelope->evidence, count * sizeof(*sorted));
        qsort(sorted, count, sizeof(*sorted), workflow_evidence_compare);
    }
    *remaining_out = hidden;
    for (size_t emit_count = count;; emit_count--) {
        char *section =
            workflow_render_evidence_section(sorted, emit_count, include_snippets);
        if (!section) {
            return false;
        }
        size_t length = strlen(section);
        bool appended =
            cbm_sb_append_n_bounded(output, section, length, body_limit);
        free(section);
        if (appended) {
            *remaining_out += count - emit_count;
            return true;
        }
        if (output->oom) {
            return false;
        }
        if (emit_count == 0U) {
            *remaining_out += count;
            return true;
        }
    }
}

static void workflow_add_remaining(cbm_sb_t *output, const char *key, size_t remaining) {
    if (remaining > 0U) {
        workflow_tree_scalar_size(output, key, remaining);
    }
}

char *cbm_workflow_envelope_render_compact(const cbm_workflow_envelope_t *envelope,
                                           size_t max_bytes, bool include_snippets) {
    if (!envelope || max_bytes < CBM_WORKFLOW_RENDER_MIN_BYTES ||
        max_bytes <= WORKFLOW_RENDER_OMISSION_RESERVE) {
        return NULL;
    }
    size_t body_limit = max_bytes - WORKFLOW_RENDER_OMISSION_RESERVE;
    cbm_sb_t output;
    cbm_sb_init(&output);
    char *base = workflow_render_base(envelope, include_snippets);
    if (!base) {
        return NULL;
    }
    size_t base_length = strlen(base);
    bool base_appended =
        cbm_sb_append_n_bounded(&output, base, base_length, body_limit);
    free(base);
    if (!base_appended) {
        cbm_sb_free(&output);
        return NULL;
    }

    size_t missing_remaining = 0U;
    size_t ignored_remaining = 0U;
    size_t unsupported_remaining = 0U;
    size_t warnings_remaining = 0U;
    size_t risk_remaining = 0U;
    size_t confidence_remaining = 0U;
    size_t evidence_remaining = 0U;
    if (!workflow_append_text_section(&output, body_limit, "coverage_missing",
                                      &envelope->coverage.missing, &missing_remaining) ||
        !workflow_append_text_section(&output, body_limit, "coverage_ignored",
                                      &envelope->coverage.ignored, &ignored_remaining) ||
        !workflow_append_text_section(&output, body_limit, "coverage_unsupported",
                                      &envelope->coverage.unsupported,
                                      &unsupported_remaining) ||
        !workflow_append_text_section(&output, body_limit, "warnings",
                                      &envelope->warnings, &warnings_remaining) ||
        !workflow_append_text_section(&output, body_limit, "risk_factors",
                                      &envelope->risk.factors, &risk_remaining) ||
        !workflow_append_text_section(&output, body_limit, "confidence_basis",
                                      &envelope->confidence.basis,
                                      &confidence_remaining) ||
        !workflow_append_evidence(&output, body_limit, envelope, include_snippets,
                                  &evidence_remaining)) {
        cbm_sb_free(&output);
        return NULL;
    }

    cbm_sb_t omissions;
    cbm_sb_init(&omissions);
    workflow_add_remaining(&omissions, "coverage_missing_remaining", missing_remaining);
    workflow_add_remaining(&omissions, "coverage_ignored_remaining", ignored_remaining);
    workflow_add_remaining(&omissions, "coverage_unsupported_remaining",
                           unsupported_remaining);
    workflow_add_remaining(&omissions, "warnings_remaining", warnings_remaining);
    workflow_add_remaining(&omissions, "risk_factors_remaining", risk_remaining);
    workflow_add_remaining(&omissions, "confidence_basis_remaining",
                           confidence_remaining);
    workflow_add_remaining(&omissions, "evidence_remaining", evidence_remaining);
    size_t total_remaining = envelope->omissions.remaining;
    total_remaining = workflow_saturated_add(total_remaining, missing_remaining);
    total_remaining = workflow_saturated_add(total_remaining, ignored_remaining);
    total_remaining = workflow_saturated_add(total_remaining, unsupported_remaining);
    total_remaining = workflow_saturated_add(total_remaining, warnings_remaining);
    total_remaining = workflow_saturated_add(total_remaining, risk_remaining);
    total_remaining = workflow_saturated_add(total_remaining, confidence_remaining);
    total_remaining = workflow_saturated_add(total_remaining, evidence_remaining);
    bool truncated = envelope->omissions.truncated || total_remaining > 0U;
    cbm_tree_scalar_bool(&omissions, "omissions_truncated", truncated);
    workflow_tree_scalar_size(&omissions, "omissions_remaining", total_remaining);
    if (truncated && envelope->omissions.cursor) {
        char cursor[WORKFLOW_RENDER_CURSOR_BYTES + 1U];
        cbm_tree_scalar_str(
            &omissions, "omissions_cursor",
            workflow_bounded_text(envelope->omissions.cursor, cursor, sizeof(cursor),
                                  WORKFLOW_RENDER_CURSOR_BYTES));
    }
    char *omission_text = cbm_sb_finish(&omissions);
    if (!omission_text) {
        cbm_sb_free(&output);
        return NULL;
    }
    size_t omission_length = strlen(omission_text);
    bool omissions_appended =
        cbm_sb_append_n_bounded(&output, omission_text, omission_length, max_bytes);
    free(omission_text);
    if (!omissions_appended) {
        cbm_sb_free(&output);
        return NULL;
    }
    return cbm_sb_finish(&output);
}

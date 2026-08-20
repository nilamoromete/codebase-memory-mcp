/* workflow_evidence.c — read-only generation, freshness, and path-coverage gate. */
#include "mcp/workflow_evidence.h"

#include "foundation/constants.h"

#include <errno.h>
#include <stdio.h>
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

static const char *workflow_path_freshness(cbm_workflow_evidence_gate_t *gate,
                                           const char *rel_path, bool *outside,
                                           bool *lookup_ok) {
    *outside = false;
    *lookup_ok = true;
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

    errno = 0;
    struct stat st;
    if (stat(abs_path, &st) != 0) {
        if (errno == ENOENT || errno == ENOTDIR) {
            return "missing";
        }
        *lookup_ok = false;
        return "unavailable";
    }
    if (!cbm_path_within_root(root_path, abs_path)) {
        *outside = true;
        return "outside_project";
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
    cbm_store_clear_file_hash(&hash);
    return matches ? "metadata_match" : "metadata_changed";
}

static const char *workflow_path_status(const cbm_coverage_row_t *rows, int count,
                                        const char *requested_path,
                                        const char *recording_status,
                                        bool generation_authoritative, bool lookup_ok,
                                        bool exact_path_verified, const char *freshness) {
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
        exact_path_verified && recording_status && strcmp(recording_status, "truncated") == 0;
    if (!generation_authoritative || !source_fresh ||
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
    if (cbm_store_generation(store, gate->source_fingerprint,
                             sizeof(gate->source_fingerprint)) != CBM_STORE_OK) {
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
    gate->generation_matches =
        gate->have_project && gate->have_coverage_meta && gate->project_record.indexed_at &&
        gate->coverage_meta.generation &&
        strcmp(gate->project_record.indexed_at, gate->coverage_meta.generation) == 0;
    if (gate->have_project && gate->have_coverage_meta && !gate->generation_matches) {
        gate->freshness = CBM_WORKFLOW_FRESHNESS_STALE;
    } else if (gate->generation_matches &&
               strcmp(gate->source_fingerprint, "legacy") != 0) {
        gate->freshness = CBM_WORKFLOW_FRESHNESS_FRESH;
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
    if (!gate || !gate->snapshot_active || !rel_path || !rel_path[0] || !evidence) {
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
        gate, rel_path, &evidence->outside, &evidence->source_lookup_ok);

    bool fingerprint_available = strcmp(gate->source_fingerprint, "legacy") != 0;
    bool generation_authoritative = gate->generation_matches && fingerprint_available;
    bool exact_path_verified =
        gate->have_coverage_meta && gate->coverage_meta.hash_records_complete &&
        strcmp(evidence->freshness_detail, "metadata_match") == 0;
    bool lookup_ok = evidence->coverage_lookup_ok && evidence->source_lookup_ok;
    evidence->status =
        evidence->outside
            ? "outside_project"
            : workflow_path_status(evidence->rows, evidence->row_count, rel_path,
                                   cbm_workflow_evidence_recording_status(gate),
                                   generation_authoritative, lookup_ok, exact_path_verified,
                                   evidence->freshness_detail);
    evidence->freshness =
        gate->generation_matches ? workflow_source_freshness(evidence->freshness_detail)
                                 : CBM_WORKFLOW_FRESHNESS_STALE;
    evidence->state = workflow_path_state(evidence);
    evidence->full_coverage = strcmp(evidence->status, "no_recorded_issue") == 0;

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
    workflow_merge_freshness(gate, evidence->freshness);
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

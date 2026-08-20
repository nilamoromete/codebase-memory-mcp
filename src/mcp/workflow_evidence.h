/* workflow_evidence.h — generation-bound freshness and coverage evidence. */
#ifndef CBM_MCP_WORKFLOW_EVIDENCE_H
#define CBM_MCP_WORKFLOW_EVIDENCE_H

#include "store/store.h"

#include <stdbool.h>
#include <stddef.h>

enum {
    CBM_WORKFLOW_STORE_GENERATION_SIZE = 96,
    CBM_WORKFLOW_CONTEXT_HANDLE_SIZE = 37,
    CBM_WORKFLOW_EVIDENCE_ID_SIZE = 37,
    CBM_WORKFLOW_TEXT_MAX_ITEMS = 16,
    CBM_WORKFLOW_EVIDENCE_MAX_ITEMS = 32,
    CBM_WORKFLOW_RENDER_MIN_BYTES = 2048,
};

typedef enum {
    CBM_WORKFLOW_FRESHNESS_UNKNOWN = 0,
    CBM_WORKFLOW_FRESHNESS_FRESH,
    CBM_WORKFLOW_FRESHNESS_DIRTY,
    CBM_WORKFLOW_FRESHNESS_STALE,
} cbm_workflow_freshness_t;

typedef enum {
    CBM_WORKFLOW_PATH_INDEXED = 0,
    CBM_WORKFLOW_PATH_PARTIAL,
    CBM_WORKFLOW_PATH_IGNORED,
    CBM_WORKFLOW_PATH_UNSUPPORTED,
    CBM_WORKFLOW_PATH_ABSENT,
    CBM_WORKFLOW_PATH_LOOKUP_FAILED,
    CBM_WORKFLOW_PATH_OUTSIDE_PROJECT,
} cbm_workflow_path_state_t;

typedef enum {
    CBM_WORKFLOW_GENERATION_UNKNOWN = 0,
    CBM_WORKFLOW_GENERATION_MATCH,
    CBM_WORKFLOW_GENERATION_MISMATCH,
} cbm_workflow_generation_state_t;

/* One read-only SQLite snapshot shared by every subquery in a workflow.
 * project is borrowed until gate_end; project_record and coverage_meta own
 * their strings. */
typedef struct {
    cbm_store_t *store;
    const char *project;
    cbm_project_t project_record;
    cbm_coverage_meta_t coverage_meta;
    char store_generation[CBM_WORKFLOW_STORE_GENERATION_SIZE];
    size_t requested;
    size_t covered;
    size_t partial;
    size_t ignored;
    size_t unsupported;
    size_t absent;
    size_t lookup_failed;
    cbm_workflow_freshness_t freshness;
    cbm_workflow_generation_state_t generation_state;
    bool have_project;
    bool have_coverage_meta;
    bool generation_matches;
    bool snapshot_active;
} cbm_workflow_evidence_gate_t;

/* Per-path evidence owns rows until path_clear. The string labels preserve the
 * existing check_index_coverage response contract while typed state is shared
 * by future workflow renderers. */
typedef struct {
    cbm_coverage_row_t *rows;
    int row_count;
    cbm_workflow_path_state_t state;
    cbm_workflow_freshness_t freshness;
    const char *status;
    const char *freshness_detail;
    bool coverage_lookup_ok;
    bool source_lookup_ok;
    bool outside;
    bool metadata_matches;
    bool content_hash_available;
    bool content_hash_matches;
    bool full_coverage;
} cbm_workflow_path_evidence_t;

typedef enum {
    CBM_WORKFLOW_OUTCOME_COMPLETE = 0,
    CBM_WORKFLOW_OUTCOME_PARTIAL,
    CBM_WORKFLOW_OUTCOME_EMPTY_VERIFIED,
    CBM_WORKFLOW_OUTCOME_AMBIGUOUS,
    CBM_WORKFLOW_OUTCOME_STALE,
    CBM_WORKFLOW_OUTCOME_UNSUPPORTED,
    CBM_WORKFLOW_OUTCOME_ERROR,
} cbm_workflow_outcome_t;

typedef enum {
    CBM_WORKFLOW_RISK_UNKNOWN = 0,
    CBM_WORKFLOW_RISK_LOW,
    CBM_WORKFLOW_RISK_MEDIUM,
    CBM_WORKFLOW_RISK_HIGH,
    CBM_WORKFLOW_RISK_CRITICAL,
} cbm_workflow_risk_level_t;

typedef enum {
    CBM_WORKFLOW_CONFIDENCE_UNKNOWN = 0,
    CBM_WORKFLOW_CONFIDENCE_LOW,
    CBM_WORKFLOW_CONFIDENCE_MEDIUM,
    CBM_WORKFLOW_CONFIDENCE_HIGH,
} cbm_workflow_confidence_level_t;

/* Lists and evidence arrays borrow their strings until rendering. Their fixed
 * capacities make truncation explicit instead of growing an unbounded DOM. */
typedef struct {
    const char *items[CBM_WORKFLOW_TEXT_MAX_ITEMS];
    size_t count;
    size_t remaining;
} cbm_workflow_text_list_t;

typedef struct {
    char id[CBM_WORKFLOW_EVIDENCE_ID_SIZE];
    const char *path;
    const char *qualified_name;
    const char *target_path;
    const char *target_qualified_name;
    const char *relationship;
    const char *reason;
    const char *source_generation;
    const char *snippet;
    double rank;
    int distance;
    int line_start;
    int line_end;
} cbm_workflow_evidence_item_t;

typedef struct {
    const char *id;
    const char *display_name;
    const char *canonical_root;
    const char *resolution;
} cbm_workflow_project_evidence_t;

typedef struct {
    const char *generation;
    const char *store_generation;
    const char *source_fingerprint;
    cbm_workflow_freshness_t freshness;
} cbm_workflow_index_evidence_t;

typedef struct {
    size_t requested;
    size_t covered;
    cbm_workflow_text_list_t missing;
    cbm_workflow_text_list_t ignored;
    cbm_workflow_text_list_t unsupported;
} cbm_workflow_coverage_evidence_t;

typedef struct {
    cbm_workflow_risk_level_t level;
    cbm_workflow_text_list_t factors;
} cbm_workflow_risk_evidence_t;

typedef struct {
    cbm_workflow_confidence_level_t level;
    cbm_workflow_text_list_t basis;
} cbm_workflow_confidence_evidence_t;

typedef struct {
    bool truncated;
    size_t remaining;
    const char *cursor;
} cbm_workflow_omissions_t;

typedef struct {
    cbm_workflow_outcome_t outcome;
    cbm_workflow_project_evidence_t project;
    cbm_workflow_index_evidence_t index;
    cbm_workflow_coverage_evidence_t coverage;
    cbm_workflow_evidence_item_t evidence[CBM_WORKFLOW_EVIDENCE_MAX_ITEMS];
    size_t evidence_count;
    size_t evidence_remaining;
    cbm_workflow_risk_evidence_t risk;
    cbm_workflow_confidence_evidence_t confidence;
    cbm_workflow_text_list_t warnings;
    cbm_workflow_omissions_t omissions;
    char context_handle[CBM_WORKFLOW_CONTEXT_HANDLE_SIZE];
} cbm_workflow_envelope_t;

int cbm_workflow_evidence_gate_begin(cbm_store_t *store, const char *project,
                                     cbm_workflow_evidence_gate_t *gate);
/* Rejects absolute, parent-relative, dot-component, backslash, empty-component,
 * and control-character paths before any filesystem lookup. */
int cbm_workflow_evidence_gate_check_path(cbm_workflow_evidence_gate_t *gate,
                                          const char *rel_path,
                                          cbm_workflow_path_evidence_t *evidence);
void cbm_workflow_evidence_path_clear(cbm_workflow_path_evidence_t *evidence);
int cbm_workflow_evidence_gate_end(cbm_workflow_evidence_gate_t *gate);
const char *cbm_workflow_evidence_recording_status(
    const cbm_workflow_evidence_gate_t *gate);

void cbm_workflow_envelope_init(cbm_workflow_envelope_t *envelope);
bool cbm_workflow_text_list_add(cbm_workflow_text_list_t *list, const char *text);
bool cbm_workflow_envelope_add_evidence(
    cbm_workflow_envelope_t *envelope, const cbm_workflow_evidence_item_t *item);
bool cbm_workflow_context_handle(char *out, size_t out_size, const char *project_id,
                                 const char *generation, const char *tool,
                                 const char *normalized_query);
/* Returns owned compact text no larger than max_bytes, or NULL for an invalid
 * envelope/budget or allocation failure. Snippets are opt-in. */
char *cbm_workflow_envelope_render_compact(const cbm_workflow_envelope_t *envelope,
                                           size_t max_bytes, bool include_snippets);

#endif /* CBM_MCP_WORKFLOW_EVIDENCE_H */

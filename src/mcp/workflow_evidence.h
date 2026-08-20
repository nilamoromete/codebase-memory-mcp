/* workflow_evidence.h — generation-bound freshness and coverage evidence. */
#ifndef CBM_MCP_WORKFLOW_EVIDENCE_H
#define CBM_MCP_WORKFLOW_EVIDENCE_H

#include "store/store.h"

#include <stdbool.h>
#include <stddef.h>

enum { CBM_WORKFLOW_SOURCE_FINGERPRINT_SIZE = 96 };

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

/* One read-only SQLite snapshot shared by every subquery in a workflow.
 * project is borrowed until gate_end; project_record and coverage_meta own
 * their strings. */
typedef struct {
    cbm_store_t *store;
    const char *project;
    cbm_project_t project_record;
    cbm_coverage_meta_t coverage_meta;
    char source_fingerprint[CBM_WORKFLOW_SOURCE_FINGERPRINT_SIZE];
    size_t requested;
    size_t covered;
    size_t partial;
    size_t ignored;
    size_t unsupported;
    size_t absent;
    size_t lookup_failed;
    cbm_workflow_freshness_t freshness;
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
    bool full_coverage;
} cbm_workflow_path_evidence_t;

int cbm_workflow_evidence_gate_begin(cbm_store_t *store, const char *project,
                                     cbm_workflow_evidence_gate_t *gate);
int cbm_workflow_evidence_gate_check_path(cbm_workflow_evidence_gate_t *gate,
                                          const char *rel_path,
                                          cbm_workflow_path_evidence_t *evidence);
void cbm_workflow_evidence_path_clear(cbm_workflow_path_evidence_t *evidence);
int cbm_workflow_evidence_gate_end(cbm_workflow_evidence_gate_t *gate);
const char *cbm_workflow_evidence_recording_status(
    const cbm_workflow_evidence_gate_t *gate);

#endif /* CBM_MCP_WORKFLOW_EVIDENCE_H */

/* workflow_query.h — deterministic internal evidence views for coding workflows. */
#ifndef CBM_MCP_WORKFLOW_QUERY_H
#define CBM_MCP_WORKFLOW_QUERY_H

#include "mcp/workflow_evidence.h"

#include <stdbool.h>
#include <stddef.h>

enum {
    CBM_WORKFLOW_QUERY_MAX_PATHS = 16,
    CBM_WORKFLOW_QUERY_MAX_CANDIDATES = 128,
    CBM_WORKFLOW_QUERY_TEXT_BUDGET = 64 * 1024,
};

typedef struct {
    const char *path;
    const char *const *paths;
    size_t path_count;
    const char *symbol;
    const char *task_type;
    size_t max_related_files;
    size_t max_tests;
    int caller_depth;
    bool include_file_aggregation;
    bool include_routes;
} cbm_workflow_query_request_t;

/* The envelope owns no strings. _private_storage retains bounded copies until
 * cbm_workflow_query_result_clear() is called. Callers should zero-initialize
 * the result before the first query. */
typedef struct {
    cbm_workflow_envelope_t envelope;
    void *_private_storage;
} cbm_workflow_query_result_t;

void cbm_workflow_query_result_clear(cbm_workflow_query_result_t *result);

int cbm_workflow_query_file_context(cbm_workflow_evidence_gate_t *gate,
                                    const cbm_workflow_query_request_t *request,
                                    cbm_workflow_query_result_t *result);
int cbm_workflow_query_related_files(cbm_workflow_evidence_gate_t *gate,
                                     const cbm_workflow_query_request_t *request,
                                     cbm_workflow_query_result_t *result);
int cbm_workflow_query_tests(cbm_workflow_evidence_gate_t *gate,
                             const cbm_workflow_query_request_t *request,
                             cbm_workflow_query_result_t *result);
int cbm_workflow_query_callers(cbm_workflow_evidence_gate_t *gate,
                               const cbm_workflow_query_request_t *request,
                               cbm_workflow_query_result_t *result);
int cbm_workflow_query_change_risks(cbm_workflow_evidence_gate_t *gate,
                                    const cbm_workflow_query_request_t *request,
                                    cbm_workflow_query_result_t *result);
int cbm_workflow_query_edit_plan(cbm_workflow_evidence_gate_t *gate,
                                 const cbm_workflow_query_request_t *request,
                                 cbm_workflow_query_result_t *result);

#endif /* CBM_MCP_WORKFLOW_QUERY_H */

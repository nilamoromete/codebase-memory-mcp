/* workflow_query.c — deterministic internal evidence views for coding workflows. */
#include "mcp/workflow_query.h"

#include <string.h>

void cbm_workflow_query_result_clear(cbm_workflow_query_result_t *result) {
    if (result) {
        memset(result, 0, sizeof(*result));
    }
}

static int workflow_query_not_implemented(cbm_workflow_evidence_gate_t *gate,
                                          const cbm_workflow_query_request_t *request,
                                          cbm_workflow_query_result_t *result) {
    (void)gate;
    (void)request;
    cbm_workflow_query_result_clear(result);
    return CBM_STORE_ERR;
}

int cbm_workflow_query_file_context(cbm_workflow_evidence_gate_t *gate,
                                    const cbm_workflow_query_request_t *request,
                                    cbm_workflow_query_result_t *result) {
    return workflow_query_not_implemented(gate, request, result);
}

int cbm_workflow_query_related_files(cbm_workflow_evidence_gate_t *gate,
                                     const cbm_workflow_query_request_t *request,
                                     cbm_workflow_query_result_t *result) {
    return workflow_query_not_implemented(gate, request, result);
}

int cbm_workflow_query_tests(cbm_workflow_evidence_gate_t *gate,
                             const cbm_workflow_query_request_t *request,
                             cbm_workflow_query_result_t *result) {
    return workflow_query_not_implemented(gate, request, result);
}

int cbm_workflow_query_callers(cbm_workflow_evidence_gate_t *gate,
                               const cbm_workflow_query_request_t *request,
                               cbm_workflow_query_result_t *result) {
    return workflow_query_not_implemented(gate, request, result);
}

int cbm_workflow_query_change_risks(cbm_workflow_evidence_gate_t *gate,
                                    const cbm_workflow_query_request_t *request,
                                    cbm_workflow_query_result_t *result) {
    return workflow_query_not_implemented(gate, request, result);
}

int cbm_workflow_query_edit_plan(cbm_workflow_evidence_gate_t *gate,
                                 const cbm_workflow_query_request_t *request,
                                 cbm_workflow_query_result_t *result) {
    return workflow_query_not_implemented(gate, request, result);
}

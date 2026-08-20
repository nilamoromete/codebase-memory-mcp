/* workflow_query.c — deterministic internal evidence views for coding workflows. */
#include "mcp/workflow_query.h"

#include "foundation/arena.h"
#include "foundation/constants.h"
#include "foundation/sha256.h"
#include "pipeline/pipeline_internal.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    WORKFLOW_QUERY_FIELD_MAX = 4096,
    WORKFLOW_QUERY_SYMBOL_MAX = CBM_WORKFLOW_QUERY_MAX_CANDIDATES,
    WORKFLOW_QUERY_OUTPUT_MAX = CBM_WORKFLOW_QUERY_MAX_CANDIDATES * 6,
};

typedef enum {
    WORKFLOW_VIEW_FILE_CONTEXT = 0,
    WORKFLOW_VIEW_RELATED_FILES,
    WORKFLOW_VIEW_TESTS,
    WORKFLOW_VIEW_CALLERS,
    WORKFLOW_VIEW_CHANGE_RISKS,
    WORKFLOW_VIEW_EDIT_PLAN,
} workflow_view_t;

enum {
    WORKFLOW_WANT_SYMBOLS = 1U << 0,
    WORKFLOW_WANT_RELATED = 1U << 1,
    WORKFLOW_WANT_TESTS = 1U << 2,
    WORKFLOW_WANT_CALLERS = 1U << 3,
    WORKFLOW_WANT_ROUTES = 1U << 4,
};

typedef struct {
    CBMArena arena;
    size_t text_bytes;
    size_t dropped;
    bool oom;
    bool truncated;
} workflow_query_storage_t;

typedef struct {
    bool lookup_ok;
    bool calls;
    bool imports;
    bool tests;
    bool tests_file;
    bool cochanges;
    bool handles;
    bool http_calls;
    bool async_calls;
} workflow_query_capabilities_t;

typedef struct {
    const char *path;
    const char *target_path;
    int callers;
    int callees;
    int imports;
    int tests;
    int cochanges;
    int dependents;
} workflow_related_candidate_t;

typedef struct {
    const char *path;
    const char *qualified_name;
    const char *target_path;
    const char *target_qualified_name;
    int direct_symbol;
    int direct_file;
    int heuristic;
    double heuristic_rank;
} workflow_test_candidate_t;

typedef struct {
    const char *path;
    const char *qualified_name;
    const char *target_path;
    const char *target_qualified_name;
    int distance;
    int occurrences;
    int line_start;
    int line_end;
} workflow_caller_candidate_t;

typedef struct {
    const char *path;
    int caller_count;
    double strongest_rank;
} workflow_caller_file_candidate_t;

typedef struct {
    const char *path;
    const char *qualified_name;
    const char *target_path;
    const char *target_qualified_name;
    int line_start;
    int line_end;
} workflow_route_candidate_t;

typedef enum {
    WORKFLOW_OUTPUT_SYMBOL = 0,
    WORKFLOW_OUTPUT_RELATED,
    WORKFLOW_OUTPUT_TEST,
    WORKFLOW_OUTPUT_CALLER,
    WORKFLOW_OUTPUT_CALLER_AGGREGATE,
    WORKFLOW_OUTPUT_ROUTE,
} workflow_output_group_t;

typedef struct {
    const char *path;
    const char *qualified_name;
    const char *target_path;
    const char *target_qualified_name;
    const char *relationship;
    const char *reason;
    workflow_output_group_t group;
    double rank;
    int distance;
    int line_start;
    int line_end;
} workflow_output_candidate_t;

typedef struct {
    cbm_workflow_query_result_t *result;
    workflow_query_storage_t *storage;
    const cbm_workflow_query_request_t *request;
    cbm_store_t *store;
    const char *project;
    workflow_query_capabilities_t capabilities;
    unsigned int wants;
    size_t max_related_files;
    size_t max_tests;
    int caller_depth;
    const char *paths[CBM_WORKFLOW_QUERY_MAX_PATHS];
    size_t path_count;
    workflow_related_candidate_t related[CBM_WORKFLOW_QUERY_MAX_CANDIDATES];
    size_t related_count;
    workflow_test_candidate_t tests[CBM_WORKFLOW_QUERY_MAX_CANDIDATES];
    size_t test_count;
    workflow_caller_candidate_t callers[CBM_WORKFLOW_QUERY_MAX_CANDIDATES];
    size_t caller_count;
    workflow_caller_file_candidate_t caller_files[CBM_WORKFLOW_QUERY_MAX_CANDIDATES];
    size_t caller_file_count;
    workflow_route_candidate_t routes[CBM_WORKFLOW_QUERY_MAX_CANDIDATES];
    size_t route_count;
    workflow_output_candidate_t output[WORKFLOW_QUERY_OUTPUT_MAX];
    size_t output_count;
    size_t direct_test_count;
    size_t symbol_count;
    bool coverage_incomplete;
    bool coverage_error;
    bool unsupported;
    bool graph_error;
    bool target_missing;
    bool file_node_missing;
    bool missing_relationships;
    bool heuristic_only_tests;
} workflow_query_bundle_t;

static size_t workflow_saturated_add(size_t left, size_t right) {
    return right > SIZE_MAX - left ? SIZE_MAX : left + right;
}

static workflow_query_storage_t *workflow_query_storage(
    cbm_workflow_query_result_t *result) {
    return result ? (workflow_query_storage_t *)result->_private_storage : NULL;
}

static const char *workflow_query_copy(cbm_workflow_query_result_t *result,
                                       const char *text) {
    workflow_query_storage_t *storage = workflow_query_storage(result);
    if (!storage || !text) {
        return NULL;
    }
    if (!text[0]) {
        return "";
    }
    size_t length = strlen(text);
    if (length > WORKFLOW_QUERY_FIELD_MAX ||
        storage->text_bytes >= CBM_WORKFLOW_QUERY_TEXT_BUDGET ||
        length + 1U > CBM_WORKFLOW_QUERY_TEXT_BUDGET - storage->text_bytes) {
        storage->dropped = workflow_saturated_add(storage->dropped, 1U);
        storage->truncated = true;
        return NULL;
    }
    char *copy = cbm_arena_strndup(&storage->arena, text, length);
    if (!copy) {
        storage->oom = true;
        return NULL;
    }
    storage->text_bytes += length + 1U;
    return copy;
}

static const char *workflow_query_copy_path(cbm_workflow_query_result_t *result,
                                            const char *path) {
    workflow_query_storage_t *storage = workflow_query_storage(result);
    if (!storage || !path || !path[0]) {
        return NULL;
    }
    size_t length = strlen(path);
    if (length > WORKFLOW_QUERY_FIELD_MAX ||
        storage->text_bytes >= CBM_WORKFLOW_QUERY_TEXT_BUDGET ||
        length + 1U > CBM_WORKFLOW_QUERY_TEXT_BUDGET - storage->text_bytes) {
        storage->dropped = workflow_saturated_add(storage->dropped, 1U);
        storage->truncated = true;
        return NULL;
    }
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)path[i];
        if (c < 0x20U || c == 0x7fU) {
            storage->dropped = workflow_saturated_add(storage->dropped, 1U);
            storage->truncated = true;
            return NULL;
        }
    }
    char *copy = cbm_arena_alloc(&storage->arena, length + 1U);
    if (!copy) {
        storage->oom = true;
        return NULL;
    }
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)path[i];
        copy[i] = c == '\\' ? '/' : (char)c;
    }
    copy[length] = 0;
    storage->text_bytes += length + 1U;
    return copy;
}

static bool workflow_query_result_begin(cbm_workflow_evidence_gate_t *gate,
                                        cbm_workflow_query_result_t *result) {
    if (!gate || !gate->snapshot_active || !result || result->_private_storage) {
        return false;
    }
    memset(result, 0, sizeof(*result));
    workflow_query_storage_t *storage = calloc(1, sizeof(*storage));
    if (!storage) {
        return false;
    }
    cbm_arena_init_sized(&storage->arena, 4096U);
    result->_private_storage = storage;
    cbm_workflow_envelope_init(&result->envelope);

    result->envelope.project.id = workflow_query_copy(result, gate->project);
    result->envelope.project.display_name = workflow_query_copy(
        result, gate->have_project && gate->project_record.name ? gate->project_record.name
                                                                : gate->project);
    result->envelope.project.canonical_root = workflow_query_copy(
        result, gate->have_project ? gate->project_record.root_path : NULL);
    result->envelope.project.resolution = "explicit";
    const char *generation =
        gate->have_coverage_meta && gate->coverage_meta.generation
            ? gate->coverage_meta.generation
        : gate->have_project && gate->project_record.indexed_at ? gate->project_record.indexed_at
                                                                : gate->store_generation;
    result->envelope.index.generation = workflow_query_copy(result, generation);
    result->envelope.index.store_generation =
        workflow_query_copy(result, gate->store_generation);
    if (!result->envelope.project.id || !result->envelope.index.generation ||
        !result->envelope.index.store_generation || storage->oom) {
        cbm_workflow_query_result_clear(result);
        return false;
    }
    return true;
}

void cbm_workflow_query_result_clear(cbm_workflow_query_result_t *result) {
    if (!result) {
        return;
    }
    workflow_query_storage_t *storage = workflow_query_storage(result);
    if (storage) {
        cbm_arena_destroy(&storage->arena);
        free(storage);
    }
    memset(result, 0, sizeof(*result));
}

static const char *workflow_nonnull(const char *value) {
    return value ? value : "";
}

static int workflow_path_compare(const char *left, const char *right) {
    left = workflow_nonnull(left);
    right = workflow_nonnull(right);
    while (*left && *right) {
        unsigned char lc = (unsigned char)(*left == '\\' ? '/' : *left);
        unsigned char rc = (unsigned char)(*right == '\\' ? '/' : *right);
        if (lc != rc) {
            return lc < rc ? -1 : 1;
        }
        left++;
        right++;
    }
    if (*left == *right) {
        return 0;
    }
    return *left ? 1 : -1;
}

static int workflow_string_pointer_compare(const void *a, const void *b) {
    const char *const *left = a;
    const char *const *right = b;
    return workflow_path_compare(*left, *right);
}

static int workflow_node_compare(const void *a, const void *b) {
    const cbm_node_t *left = a;
    const cbm_node_t *right = b;
    int cmp = workflow_path_compare(left->file_path, right->file_path);
    if (cmp != 0) {
        return cmp;
    }
    cmp = strcmp(workflow_nonnull(left->qualified_name),
                 workflow_nonnull(right->qualified_name));
    if (cmp != 0) {
        return cmp;
    }
    cmp = strcmp(workflow_nonnull(left->label), workflow_nonnull(right->label));
    if (cmp != 0) {
        return cmp;
    }
    if (left->start_line != right->start_line) {
        return left->start_line < right->start_line ? -1 : 1;
    }
    if (left->id == right->id) {
        return 0;
    }
    return left->id < right->id ? -1 : 1;
}

static int workflow_output_compare(const void *a, const void *b) {
    const workflow_output_candidate_t *left = a;
    const workflow_output_candidate_t *right = b;
    if (left->rank != right->rank) {
        return left->rank > right->rank ? -1 : 1;
    }
    int cmp = workflow_path_compare(left->path, right->path);
    if (cmp != 0) {
        return cmp;
    }
    cmp = strcmp(workflow_nonnull(left->qualified_name),
                 workflow_nonnull(right->qualified_name));
    if (cmp != 0) {
        return cmp;
    }
    cmp = strcmp(workflow_nonnull(left->relationship),
                 workflow_nonnull(right->relationship));
    if (cmp != 0) {
        return cmp;
    }
    cmp = workflow_path_compare(left->target_path, right->target_path);
    if (cmp != 0) {
        return cmp;
    }
    cmp = strcmp(workflow_nonnull(left->target_qualified_name),
                 workflow_nonnull(right->target_qualified_name));
    if (cmp != 0) {
        return cmp;
    }
    if (left->distance != right->distance) {
        return left->distance < right->distance ? -1 : 1;
    }
    if (left->line_start != right->line_start) {
        return left->line_start < right->line_start ? -1 : 1;
    }
    if (left->line_end == right->line_end) {
        return 0;
    }
    return left->line_end < right->line_end ? -1 : 1;
}

static bool workflow_output_add(workflow_query_bundle_t *bundle, const char *path,
                                const char *qualified_name, const char *target_path,
                                const char *target_qualified_name, const char *relationship,
                                const char *reason, workflow_output_group_t group,
                                double rank, int distance, int line_start, int line_end) {
    if (!bundle || !relationship || !reason) {
        return false;
    }
    if (bundle->output_count >= WORKFLOW_QUERY_OUTPUT_MAX) {
        bundle->storage->dropped =
            workflow_saturated_add(bundle->storage->dropped, 1U);
        bundle->storage->truncated = true;
        return false;
    }
    workflow_output_candidate_t *candidate =
        &bundle->output[bundle->output_count++];
    *candidate = (workflow_output_candidate_t){
        .path = path,
        .qualified_name = qualified_name,
        .target_path = target_path,
        .target_qualified_name = target_qualified_name,
        .relationship = relationship,
        .reason = reason,
        .group = group,
        .rank = rank,
        .distance = distance,
        .line_start = line_start,
        .line_end = line_end,
    };
    return true;
}

typedef enum {
    WORKFLOW_RELATED_CALLER = 0,
    WORKFLOW_RELATED_CALLEE,
    WORKFLOW_RELATED_IMPORT,
    WORKFLOW_RELATED_TEST,
    WORKFLOW_RELATED_COCHANGE,
    WORKFLOW_RELATED_DEPENDENT,
} workflow_related_signal_t;

static void workflow_related_add(workflow_query_bundle_t *bundle, const char *path,
                                 const char *target_path, workflow_related_signal_t signal) {
    if (!bundle || !path || !path[0] ||
        (target_path && workflow_path_compare(path, target_path) == 0)) {
        return;
    }
    for (size_t i = 0; i < bundle->related_count; i++) {
        workflow_related_candidate_t *candidate = &bundle->related[i];
        if (workflow_path_compare(candidate->path, path) == 0) {
            switch (signal) {
            case WORKFLOW_RELATED_CALLER:
                candidate->callers++;
                break;
            case WORKFLOW_RELATED_CALLEE:
                candidate->callees++;
                break;
            case WORKFLOW_RELATED_IMPORT:
                candidate->imports++;
                break;
            case WORKFLOW_RELATED_TEST:
                candidate->tests++;
                break;
            case WORKFLOW_RELATED_COCHANGE:
                candidate->cochanges++;
                break;
            case WORKFLOW_RELATED_DEPENDENT:
                candidate->dependents++;
                break;
            }
            return;
        }
    }
    if (bundle->related_count >= CBM_WORKFLOW_QUERY_MAX_CANDIDATES) {
        bundle->storage->dropped =
            workflow_saturated_add(bundle->storage->dropped, 1U);
        bundle->storage->truncated = true;
        return;
    }
    const char *path_copy = workflow_query_copy_path(bundle->result, path);
    const char *target_copy = workflow_query_copy_path(bundle->result, target_path);
    if (!path_copy || (target_path && !target_copy)) {
        return;
    }
    workflow_related_candidate_t *candidate =
        &bundle->related[bundle->related_count++];
    memset(candidate, 0, sizeof(*candidate));
    candidate->path = path_copy;
    candidate->target_path = target_copy;
    switch (signal) {
    case WORKFLOW_RELATED_CALLER:
        candidate->callers = 1;
        break;
    case WORKFLOW_RELATED_CALLEE:
        candidate->callees = 1;
        break;
    case WORKFLOW_RELATED_IMPORT:
        candidate->imports = 1;
        break;
    case WORKFLOW_RELATED_TEST:
        candidate->tests = 1;
        break;
    case WORKFLOW_RELATED_COCHANGE:
        candidate->cochanges = 1;
        break;
    case WORKFLOW_RELATED_DEPENDENT:
        candidate->dependents = 1;
        break;
    }
}

static void workflow_test_add(workflow_query_bundle_t *bundle, const cbm_node_t *node,
                              const char *path, const char *target_path,
                              const char *target_qualified_name, bool direct_symbol,
                              bool direct_file, double heuristic_rank) {
    const char *candidate_path = path ? path : node ? node->file_path : NULL;
    if (!bundle || !candidate_path || !candidate_path[0]) {
        return;
    }
    for (size_t i = 0; i < bundle->test_count; i++) {
        workflow_test_candidate_t *candidate = &bundle->tests[i];
        if (workflow_path_compare(candidate->path, candidate_path) != 0) {
            continue;
        }
        candidate->direct_symbol += direct_symbol ? 1 : 0;
        candidate->direct_file += direct_file ? 1 : 0;
        candidate->heuristic += heuristic_rank > 0.0 ? 1 : 0;
        if (heuristic_rank > candidate->heuristic_rank) {
            candidate->heuristic_rank = heuristic_rank;
        }
        if (direct_symbol && node && node->qualified_name && !candidate->qualified_name) {
            candidate->qualified_name =
                workflow_query_copy(bundle->result, node->qualified_name);
        }
        if (direct_symbol && target_qualified_name && !candidate->target_qualified_name) {
            candidate->target_qualified_name =
                workflow_query_copy(bundle->result, target_qualified_name);
        }
        return;
    }
    if (bundle->test_count >= CBM_WORKFLOW_QUERY_MAX_CANDIDATES) {
        bundle->storage->dropped =
            workflow_saturated_add(bundle->storage->dropped, 1U);
        bundle->storage->truncated = true;
        return;
    }
    workflow_test_candidate_t *candidate = &bundle->tests[bundle->test_count++];
    memset(candidate, 0, sizeof(*candidate));
    candidate->path = workflow_query_copy_path(bundle->result, candidate_path);
    candidate->target_path = workflow_query_copy_path(bundle->result, target_path);
    candidate->qualified_name =
        direct_symbol && node ? workflow_query_copy(bundle->result, node->qualified_name) : NULL;
    candidate->target_qualified_name = direct_symbol
                                                   ? workflow_query_copy(
                                                         bundle->result,
                                                         target_qualified_name)
                                                   : NULL;
    if (!candidate->path || (target_path && !candidate->target_path)) {
        bundle->test_count--;
        return;
    }
    candidate->direct_symbol = direct_symbol ? 1 : 0;
    candidate->direct_file = direct_file ? 1 : 0;
    candidate->heuristic = heuristic_rank > 0.0 ? 1 : 0;
    candidate->heuristic_rank = heuristic_rank;
}

static double workflow_caller_rank(int distance) {
    if (distance <= 1) {
        return 1.0;
    }
    if (distance == 2) {
        return 0.76;
    }
    return 0.58;
}

static void workflow_caller_file_add(workflow_query_bundle_t *bundle, const char *path,
                                     double rank) {
    if (!path || !path[0]) {
        return;
    }
    for (size_t i = 0; i < bundle->caller_file_count; i++) {
        workflow_caller_file_candidate_t *candidate = &bundle->caller_files[i];
        if (workflow_path_compare(candidate->path, path) == 0) {
            candidate->caller_count++;
            if (rank > candidate->strongest_rank) {
                candidate->strongest_rank = rank;
            }
            return;
        }
    }
    if (bundle->caller_file_count >= CBM_WORKFLOW_QUERY_MAX_CANDIDATES) {
        bundle->storage->dropped =
            workflow_saturated_add(bundle->storage->dropped, 1U);
        bundle->storage->truncated = true;
        return;
    }
    workflow_caller_file_candidate_t *candidate =
        &bundle->caller_files[bundle->caller_file_count++];
    candidate->path = path;
    candidate->caller_count = 1;
    candidate->strongest_rank = rank;
}

static void workflow_caller_add(workflow_query_bundle_t *bundle, const cbm_node_t *node,
                                const char *target_path,
                                const char *target_qualified_name, int distance) {
    if (!bundle || !node || !node->qualified_name || !node->qualified_name[0] ||
        !node->file_path || !node->file_path[0] || cbm_is_test_path(node->file_path)) {
        return;
    }
    for (size_t i = 0; i < bundle->caller_count; i++) {
        workflow_caller_candidate_t *candidate = &bundle->callers[i];
        if (strcmp(candidate->qualified_name, node->qualified_name) != 0) {
            continue;
        }
        candidate->occurrences++;
        if (distance < candidate->distance) {
            candidate->distance = distance;
            if (target_qualified_name) {
                candidate->target_qualified_name =
                    workflow_query_copy(bundle->result, target_qualified_name);
            }
        }
        return;
    }
    if (bundle->caller_count >= CBM_WORKFLOW_QUERY_MAX_CANDIDATES) {
        bundle->storage->dropped =
            workflow_saturated_add(bundle->storage->dropped, 1U);
        bundle->storage->truncated = true;
        return;
    }
    workflow_caller_candidate_t *candidate = &bundle->callers[bundle->caller_count++];
    memset(candidate, 0, sizeof(*candidate));
    candidate->path = workflow_query_copy_path(bundle->result, node->file_path);
    candidate->qualified_name = workflow_query_copy(bundle->result, node->qualified_name);
    candidate->target_path = workflow_query_copy_path(bundle->result, target_path);
    candidate->target_qualified_name =
        workflow_query_copy(bundle->result, target_qualified_name);
    if (!candidate->path || !candidate->qualified_name ||
        (target_path && !candidate->target_path)) {
        bundle->caller_count--;
        return;
    }
    candidate->distance = distance;
    candidate->occurrences = 1;
    candidate->line_start = node->start_line;
    candidate->line_end = node->end_line;
    workflow_caller_file_add(bundle, candidate->path, workflow_caller_rank(distance));
}

static void workflow_route_add(workflow_query_bundle_t *bundle, const cbm_node_t *node,
                               const char *target_path,
                               const char *target_qualified_name) {
    if (!bundle || !node || !node->qualified_name || !node->qualified_name[0]) {
        return;
    }
    for (size_t i = 0; i < bundle->route_count; i++) {
        if (strcmp(bundle->routes[i].qualified_name, node->qualified_name) == 0) {
            return;
        }
    }
    if (bundle->route_count >= CBM_WORKFLOW_QUERY_MAX_CANDIDATES) {
        bundle->storage->dropped =
            workflow_saturated_add(bundle->storage->dropped, 1U);
        bundle->storage->truncated = true;
        return;
    }
    workflow_route_candidate_t *candidate = &bundle->routes[bundle->route_count++];
    memset(candidate, 0, sizeof(*candidate));
    candidate->path = workflow_query_copy_path(
        bundle->result, node->file_path && node->file_path[0] ? node->file_path : target_path);
    candidate->qualified_name = workflow_query_copy(bundle->result, node->qualified_name);
    candidate->target_path = workflow_query_copy_path(bundle->result, target_path);
    candidate->target_qualified_name =
        workflow_query_copy(bundle->result, target_qualified_name);
    if (!candidate->qualified_name || (target_path && !candidate->target_path)) {
        bundle->route_count--;
        return;
    }
    candidate->line_start = node->start_line;
    candidate->line_end = node->end_line;
}

static bool workflow_is_meta_label(const char *label) {
    return label &&
           (strcmp(label, "File") == 0 || strcmp(label, "Folder") == 0 ||
            strcmp(label, "Module") == 0 || strcmp(label, "Package") == 0);
}

static bool workflow_is_route_node(const cbm_node_t *node) {
    return node && node->label && strcmp(node->label, "Route") == 0;
}

static bool workflow_schema_has_edge(const cbm_schema_info_t *schema, const char *type) {
    for (int i = 0; schema && i < schema->edge_type_count; i++) {
        if (schema->edge_types[i].type && strcmp(schema->edge_types[i].type, type) == 0) {
            return true;
        }
    }
    return false;
}

static bool workflow_load_capabilities(cbm_workflow_evidence_gate_t *gate,
                                       workflow_query_capabilities_t *capabilities) {
    cbm_schema_info_t schema = {0};
    if (cbm_store_get_schema_counts(gate->store, gate->project, &schema) != CBM_STORE_OK) {
        return false;
    }
    capabilities->lookup_ok = true;
    capabilities->calls = workflow_schema_has_edge(&schema, "CALLS");
    capabilities->imports = workflow_schema_has_edge(&schema, "IMPORTS");
    capabilities->tests = workflow_schema_has_edge(&schema, "TESTS");
    capabilities->tests_file = workflow_schema_has_edge(&schema, "TESTS_FILE");
    capabilities->cochanges = workflow_schema_has_edge(&schema, "FILE_CHANGES_WITH");
    capabilities->handles = workflow_schema_has_edge(&schema, "HANDLES");
    capabilities->http_calls = workflow_schema_has_edge(&schema, "HTTP_CALLS");
    capabilities->async_calls = workflow_schema_has_edge(&schema, "ASYNC_CALLS");
    cbm_store_schema_free(&schema);
    return true;
}

static bool workflow_text_list_add_copy(cbm_workflow_query_result_t *result,
                                        cbm_workflow_text_list_t *list,
                                        const char *text) {
    const char *copy = workflow_query_copy(result, text);
    return copy && cbm_workflow_text_list_add(list, copy);
}

static bool workflow_collect_request_paths(workflow_query_bundle_t *bundle) {
    const cbm_workflow_query_request_t *request = bundle->request;
    size_t count = 0;
    if (request->path && request->path[0]) {
        bundle->paths[count++] = request->path;
    }
    if (request->path_count > CBM_WORKFLOW_QUERY_MAX_PATHS ||
        (request->path_count > 0U && !request->paths)) {
        return false;
    }
    for (size_t i = 0; i < request->path_count; i++) {
        const char *path = request->paths[i];
        if (!path || !path[0] || strlen(path) >= CBM_PATH_MAX ||
            count >= CBM_WORKFLOW_QUERY_MAX_PATHS) {
            return false;
        }
        bundle->paths[count++] = path;
    }
    if (count == 0U) {
        bundle->path_count = 0U;
        return request->allow_empty;
    }
    qsort(bundle->paths, count, sizeof(bundle->paths[0]), workflow_string_pointer_compare);
    size_t unique = 0;
    for (size_t i = 0; i < count; i++) {
        if (strlen(bundle->paths[i]) >= CBM_PATH_MAX) {
            return false;
        }
        if (unique == 0U || workflow_path_compare(bundle->paths[i], bundle->paths[unique - 1U]) != 0) {
            bundle->paths[unique++] = bundle->paths[i];
        }
    }
    bundle->path_count = unique;
    return true;
}

static const char *workflow_view_name(workflow_view_t view) {
    switch (view) {
    case WORKFLOW_VIEW_FILE_CONTEXT:
        return "file_context";
    case WORKFLOW_VIEW_RELATED_FILES:
        return "related_files";
    case WORKFLOW_VIEW_TESTS:
        return "tests";
    case WORKFLOW_VIEW_CALLERS:
        return "callers";
    case WORKFLOW_VIEW_CHANGE_RISKS:
        return "change_risks";
    case WORKFLOW_VIEW_EDIT_PLAN:
        return "edit_plan";
    default:
        return "workflow";
    }
}

static unsigned int workflow_view_wants(workflow_view_t view, bool include_routes) {
    unsigned int routes = include_routes ? WORKFLOW_WANT_ROUTES : 0U;
    switch (view) {
    case WORKFLOW_VIEW_FILE_CONTEXT:
        return WORKFLOW_WANT_SYMBOLS | WORKFLOW_WANT_RELATED | WORKFLOW_WANT_TESTS |
               WORKFLOW_WANT_CALLERS | routes;
    case WORKFLOW_VIEW_RELATED_FILES:
        return WORKFLOW_WANT_RELATED;
    case WORKFLOW_VIEW_TESTS:
        return WORKFLOW_WANT_TESTS;
    case WORKFLOW_VIEW_CALLERS:
        return WORKFLOW_WANT_CALLERS;
    case WORKFLOW_VIEW_CHANGE_RISKS:
        return WORKFLOW_WANT_RELATED | WORKFLOW_WANT_TESTS | WORKFLOW_WANT_CALLERS |
               routes;
    case WORKFLOW_VIEW_EDIT_PLAN:
        return WORKFLOW_WANT_SYMBOLS | WORKFLOW_WANT_RELATED | WORKFLOW_WANT_TESTS |
               WORKFLOW_WANT_CALLERS | routes;
    default:
        return 0U;
    }
}

static void workflow_add_text(workflow_query_bundle_t *bundle,
                              cbm_workflow_text_list_t *list, const char *text) {
    if (!workflow_text_list_add_copy(bundle->result, list, text)) {
        bundle->storage->dropped =
            workflow_saturated_add(bundle->storage->dropped, 1U);
        bundle->storage->truncated = true;
    }
}

static bool workflow_node_matches_symbol(const cbm_node_t *node,
                                         const char *symbol) {
    if (!symbol || !symbol[0]) {
        return true;
    }
    if ((node->qualified_name && strcmp(node->qualified_name, symbol) == 0) ||
        (node->name && strcmp(node->name, symbol) == 0)) {
        return true;
    }
    if (!node->qualified_name) {
        return false;
    }
    size_t qn_length = strlen(node->qualified_name);
    size_t symbol_length = strlen(symbol);
    return qn_length > symbol_length &&
           strcmp(node->qualified_name + qn_length - symbol_length, symbol) == 0 &&
           node->qualified_name[qn_length - symbol_length - 1U] == '.';
}

static void workflow_hash_value(cbm_sha256_ctx *sha, const char *value) {
    size_t length = value ? strlen(value) : 0U;
    uint64_t encoded_length = (uint64_t)length;
    unsigned char encoded[8];
    for (size_t i = 0; i < sizeof(encoded); i++) {
        encoded[sizeof(encoded) - i - 1U] =
            (unsigned char)(encoded_length & 0xffU);
        encoded_length >>= 8U;
    }
    cbm_sha256_update(sha, encoded, sizeof(encoded));
    if (length > 0U) {
        cbm_sha256_update(sha, value, length);
    }
}

static bool workflow_set_context_handle(workflow_query_bundle_t *bundle,
                                        workflow_view_t view) {
    cbm_sha256_ctx sha;
    cbm_sha256_init(&sha);
    workflow_hash_value(&sha, "cbm.workflow.query.v1");
    workflow_hash_value(&sha, workflow_view_name(view));
    workflow_hash_value(&sha, bundle->request->symbol);
    workflow_hash_value(&sha, bundle->request->task_type);
    workflow_hash_value(&sha, bundle->request->query_mode);
    char options[128];
    (void)snprintf(options, sizeof(options), "%zu:%zu:%d:%d:%d",
                   bundle->max_related_files, bundle->max_tests,
                   bundle->caller_depth,
                   bundle->request->include_file_aggregation ? 1 : 0,
                   bundle->request->include_routes ? 1 : 0);
    workflow_hash_value(&sha, options);
    for (size_t i = 0; i < bundle->path_count; i++) {
        workflow_hash_value(&sha, bundle->paths[i]);
    }
    uint8_t digest[CBM_SHA256_DIGEST_LEN];
    cbm_sha256_final(&sha, digest);
    static const char hex[] = "0123456789abcdef";
    char normalized_query[CBM_SHA256_HEX_LEN + 1U];
    for (size_t i = 0; i < CBM_SHA256_DIGEST_LEN; i++) {
        normalized_query[i * 2U] = hex[digest[i] >> 4U];
        normalized_query[i * 2U + 1U] = hex[digest[i] & 0x0fU];
    }
    normalized_query[CBM_SHA256_HEX_LEN] = 0;
    return cbm_workflow_context_handle(
        bundle->result->envelope.context_handle,
        sizeof(bundle->result->envelope.context_handle),
        bundle->result->envelope.project.id,
        bundle->result->envelope.index.generation, workflow_view_name(view),
        normalized_query);
}

static void workflow_record_coverage_path(
    workflow_query_bundle_t *bundle, const char *path,
    const cbm_workflow_path_evidence_t *evidence) {
    cbm_workflow_coverage_evidence_t *coverage =
        &bundle->result->envelope.coverage;
    coverage->requested++;
    if (evidence->full_coverage) {
        coverage->covered++;
        return;
    }
    bundle->coverage_incomplete = true;
    switch (evidence->state) {
    case CBM_WORKFLOW_PATH_IGNORED:
        workflow_add_text(bundle, &coverage->ignored, path);
        break;
    case CBM_WORKFLOW_PATH_UNSUPPORTED:
        bundle->unsupported = true;
        workflow_add_text(bundle, &coverage->unsupported, path);
        break;
    case CBM_WORKFLOW_PATH_LOOKUP_FAILED:
        bundle->coverage_error = true;
        workflow_add_text(bundle, &coverage->missing, path);
        break;
    case CBM_WORKFLOW_PATH_PARTIAL:
    case CBM_WORKFLOW_PATH_ABSENT:
    case CBM_WORKFLOW_PATH_OUTSIDE_PROJECT:
    case CBM_WORKFLOW_PATH_INDEXED:
    default:
        workflow_add_text(bundle, &coverage->missing, path);
        break;
    }
}

static bool workflow_collect_coverage(workflow_query_bundle_t *bundle,
                                      cbm_workflow_evidence_gate_t *gate) {
    for (size_t i = 0; i < bundle->path_count; i++) {
        cbm_workflow_path_evidence_t evidence = {0};
        if (cbm_workflow_evidence_gate_check_path(gate, bundle->paths[i],
                                                  &evidence) != CBM_STORE_OK) {
            bundle->coverage_error = true;
            return false;
        }
        workflow_record_coverage_path(bundle, bundle->paths[i], &evidence);
        cbm_workflow_evidence_path_clear(&evidence);
    }
    bundle->result->envelope.index.freshness = gate->freshness;
    return true;
}

static void workflow_require_capabilities(workflow_query_bundle_t *bundle) {
    if ((bundle->wants & (WORKFLOW_WANT_RELATED | WORKFLOW_WANT_CALLERS)) != 0U &&
        !bundle->capabilities.calls) {
        bundle->missing_relationships = true;
        workflow_add_text(bundle, &bundle->result->envelope.warnings,
                          "CALLS relationships are unavailable");
    }
    if ((bundle->wants & WORKFLOW_WANT_RELATED) != 0U &&
        !bundle->capabilities.imports) {
        bundle->missing_relationships = true;
        workflow_add_text(bundle, &bundle->result->envelope.warnings,
                          "IMPORTS relationships are unavailable");
    }
    if ((bundle->wants & WORKFLOW_WANT_TESTS) != 0U &&
        !bundle->capabilities.tests && !bundle->capabilities.tests_file) {
        bundle->missing_relationships = true;
        workflow_add_text(bundle, &bundle->result->envelope.warnings,
                          "TESTS and TESTS_FILE relationships are unavailable");
    }
    if ((bundle->wants & WORKFLOW_WANT_ROUTES) != 0U &&
        !bundle->capabilities.handles && !bundle->capabilities.http_calls &&
        !bundle->capabilities.async_calls) {
        bundle->missing_relationships = true;
        workflow_add_text(bundle, &bundle->result->envelope.warnings,
                          "route relationship classes are unavailable");
    }
}

static int workflow_bfs(workflow_query_bundle_t *bundle,
                        const int64_t *seed_ids, int seed_count,
                        const char *direction, const char **edge_types,
                        int edge_type_count, int max_depth,
                        cbm_traverse_result_t *traverse) {
    memset(traverse, 0, sizeof(*traverse));
    if (seed_count <= 0) {
        return CBM_STORE_OK;
    }
    bool truncated = false;
    int rc = cbm_store_bfs_multi(
        bundle->store,
        seed_ids, seed_count, direction, edge_types, edge_type_count, max_depth,
        CBM_WORKFLOW_QUERY_MAX_CANDIDATES, traverse, &truncated);
    if (truncated) {
        bundle->storage->truncated = true;
        bundle->storage->dropped =
            workflow_saturated_add(bundle->storage->dropped, 1U);
    }
    if (rc != CBM_STORE_OK) {
        bundle->graph_error = true;
    }
    return rc;
}

static void workflow_add_symbol_output(workflow_query_bundle_t *bundle,
                                       const cbm_node_t *node) {
    const char *path = workflow_query_copy_path(bundle->result, node->file_path);
    const char *qualified_name =
        workflow_query_copy(bundle->result, node->qualified_name);
    const char *reason = workflow_query_copy(
        bundle->result, "direct definition node in the requested file");
    if (path && qualified_name && reason) {
        (void)workflow_output_add(
            bundle, path, qualified_name, NULL, NULL, "DEFINES", reason,
            WORKFLOW_OUTPUT_SYMBOL, 0.96, 0, node->start_line, node->end_line);
    }
}

static void workflow_collect_call_edges(workflow_query_bundle_t *bundle,
                                        const int64_t *symbol_ids,
                                        int symbol_count,
                                        const cbm_node_t *target_symbol,
                                        const char *target_path) {
    const char *edge_types[] = {"CALLS"};
    cbm_traverse_result_t inbound = {0};
    if (workflow_bfs(bundle, symbol_ids, symbol_count, "inbound", edge_types, 1,
                     bundle->caller_depth, &inbound) == CBM_STORE_OK) {
        for (int i = 0; i < inbound.visited_count; i++) {
            const cbm_node_hop_t *hop = &inbound.visited[i];
            if ((bundle->wants & WORKFLOW_WANT_CALLERS) != 0U) {
                workflow_caller_add(bundle, &hop->node, target_path,
                                    target_symbol ? target_symbol->qualified_name : NULL,
                                    hop->hop);
            }
            if ((bundle->wants & WORKFLOW_WANT_RELATED) != 0U) {
                workflow_related_add(bundle, hop->node.file_path, target_path,
                                     WORKFLOW_RELATED_CALLER);
            }
        }
    }
    cbm_store_traverse_free(&inbound);

    if ((bundle->wants & WORKFLOW_WANT_RELATED) == 0U) {
        return;
    }
    cbm_traverse_result_t outbound = {0};
    if (workflow_bfs(bundle, symbol_ids, symbol_count, "outbound", edge_types, 1,
                     1, &outbound) == CBM_STORE_OK) {
        for (int i = 0; i < outbound.visited_count; i++) {
            workflow_related_add(bundle, outbound.visited[i].node.file_path,
                                 target_path, WORKFLOW_RELATED_CALLEE);
        }
    }
    cbm_store_traverse_free(&outbound);
}

static void workflow_collect_symbol_tests(workflow_query_bundle_t *bundle,
                                          const int64_t *symbol_ids,
                                          int symbol_count,
                                          const cbm_node_t *target_symbol,
                                          const char *target_path) {
    const char *edge_types[] = {"TESTS"};
    cbm_traverse_result_t traverse = {0};
    if (workflow_bfs(bundle, symbol_ids, symbol_count, "inbound", edge_types, 1,
                     1, &traverse) == CBM_STORE_OK) {
        for (int i = 0; i < traverse.visited_count; i++) {
            const cbm_node_t *node = &traverse.visited[i].node;
            workflow_test_add(bundle, node, NULL, target_path,
                              target_symbol ? target_symbol->qualified_name : NULL,
                              true, false, 0.0);
            if ((bundle->wants & WORKFLOW_WANT_RELATED) != 0U) {
                workflow_related_add(bundle, node->file_path, target_path,
                                     WORKFLOW_RELATED_TEST);
            }
        }
    }
    cbm_store_traverse_free(&traverse);
}

static void workflow_collect_file_edge(workflow_query_bundle_t *bundle,
                                       int64_t file_id, const char *target_path,
                                       const char *edge_type,
                                       const char *direction,
                                       workflow_related_signal_t signal) {
    const char *edge_types[] = {edge_type};
    cbm_traverse_result_t traverse = {0};
    if (workflow_bfs(bundle, &file_id, 1, direction, edge_types, 1, 1,
                     &traverse) == CBM_STORE_OK) {
        for (int i = 0; i < traverse.visited_count; i++) {
            workflow_related_add(bundle, traverse.visited[i].node.file_path,
                                 target_path, signal);
        }
    }
    cbm_store_traverse_free(&traverse);
}

static void workflow_collect_file_tests(workflow_query_bundle_t *bundle,
                                        int64_t file_id,
                                        const char *target_path) {
    const char *edge_types[] = {"TESTS_FILE"};
    cbm_traverse_result_t traverse = {0};
    if (workflow_bfs(bundle, &file_id, 1, "inbound", edge_types, 1, 1,
                     &traverse) == CBM_STORE_OK) {
        for (int i = 0; i < traverse.visited_count; i++) {
            const cbm_node_t *node = &traverse.visited[i].node;
            workflow_test_add(bundle, node, node->file_path, target_path, NULL,
                              false, true, 0.0);
            if ((bundle->wants & WORKFLOW_WANT_RELATED) != 0U) {
                workflow_related_add(bundle, node->file_path, target_path,
                                     WORKFLOW_RELATED_TEST);
            }
        }
    }
    cbm_store_traverse_free(&traverse);
}

static void workflow_collect_routes(workflow_query_bundle_t *bundle,
                                    const int64_t *symbol_ids,
                                    int symbol_count,
                                    const cbm_node_t *target_symbol,
                                    const char *target_path) {
    const char *edge_types[3];
    int edge_type_count = 0;
    if (bundle->capabilities.handles) {
        edge_types[edge_type_count++] = "HANDLES";
    }
    if (bundle->capabilities.http_calls) {
        edge_types[edge_type_count++] = "HTTP_CALLS";
    }
    if (bundle->capabilities.async_calls) {
        edge_types[edge_type_count++] = "ASYNC_CALLS";
    }
    if (edge_type_count == 0) {
        return;
    }
    const char *directions[] = {"outbound", "inbound"};
    for (size_t direction = 0;
         direction < sizeof(directions) / sizeof(directions[0]); direction++) {
        cbm_traverse_result_t traverse = {0};
        if (workflow_bfs(bundle, symbol_ids, symbol_count, directions[direction],
                         edge_types, edge_type_count, 1, &traverse) ==
            CBM_STORE_OK) {
            for (int i = 0; i < traverse.visited_count; i++) {
                if (workflow_is_route_node(&traverse.visited[i].node)) {
                    workflow_route_add(
                        bundle, &traverse.visited[i].node, target_path,
                        target_symbol ? target_symbol->qualified_name : NULL);
                }
            }
        }
        cbm_store_traverse_free(&traverse);
    }
}

static int workflow_collect_path_graph(workflow_query_bundle_t *bundle,
                                       const char *path) {
    cbm_node_t *nodes = NULL;
    int node_count = 0;
    int rc = cbm_store_find_nodes_by_file(bundle->store, bundle->project, path,
                                          &nodes, &node_count);
    if (rc != CBM_STORE_OK && rc != CBM_STORE_NOT_FOUND) {
        bundle->graph_error = true;
        return CBM_STORE_ERR;
    }
    if (node_count > 1) {
        qsort(nodes, (size_t)node_count, sizeof(nodes[0]), workflow_node_compare);
    }

    int64_t file_id = 0;
    int64_t symbol_ids[WORKFLOW_QUERY_SYMBOL_MAX];
    int symbol_count = 0;
    const cbm_node_t *target_symbol = NULL;
    for (int i = 0; i < node_count; i++) {
        cbm_node_t *node = &nodes[i];
        if (node->label && strcmp(node->label, "File") == 0 && file_id == 0) {
            file_id = node->id;
        }
        if (workflow_is_meta_label(node->label) || workflow_is_route_node(node) ||
            !node->qualified_name || !node->qualified_name[0] ||
            !workflow_node_matches_symbol(node, bundle->request->symbol)) {
            continue;
        }
        if (symbol_count < WORKFLOW_QUERY_SYMBOL_MAX) {
            symbol_ids[symbol_count++] = node->id;
            if (!target_symbol) {
                target_symbol = node;
            }
            bundle->symbol_count++;
            if ((bundle->wants & WORKFLOW_WANT_SYMBOLS) != 0U) {
                workflow_add_symbol_output(bundle, node);
            }
        } else {
            bundle->storage->truncated = true;
            bundle->storage->dropped =
                workflow_saturated_add(bundle->storage->dropped, 1U);
        }
    }

    if (node_count == 0 || (bundle->request->symbol && bundle->request->symbol[0] &&
                            symbol_count == 0)) {
        bundle->target_missing = true;
    }
    if (file_id == 0) {
        bundle->file_node_missing = true;
    }
    if (symbol_count > 0 && bundle->capabilities.calls &&
        (bundle->wants & (WORKFLOW_WANT_CALLERS | WORKFLOW_WANT_RELATED)) != 0U) {
        workflow_collect_call_edges(bundle, symbol_ids, symbol_count, target_symbol,
                                    path);
    }
    if (symbol_count > 0 && bundle->capabilities.tests &&
        (bundle->wants & WORKFLOW_WANT_TESTS) != 0U) {
        workflow_collect_symbol_tests(bundle, symbol_ids, symbol_count, target_symbol,
                                      path);
    }
    if (symbol_count > 0 && (bundle->wants & WORKFLOW_WANT_ROUTES) != 0U) {
        workflow_collect_routes(bundle, symbol_ids, symbol_count, target_symbol, path);
    }
    if (file_id != 0 && (bundle->wants & WORKFLOW_WANT_RELATED) != 0U) {
        if (bundle->capabilities.imports) {
            workflow_collect_file_edge(bundle, file_id, path, "IMPORTS", "outbound",
                                       WORKFLOW_RELATED_IMPORT);
        }
        if (bundle->capabilities.cochanges) {
            workflow_collect_file_edge(bundle, file_id, path, "FILE_CHANGES_WITH",
                                       "outbound", WORKFLOW_RELATED_COCHANGE);
            workflow_collect_file_edge(bundle, file_id, path, "FILE_CHANGES_WITH",
                                       "inbound", WORKFLOW_RELATED_COCHANGE);
        }
    }
    if (file_id != 0 && bundle->capabilities.tests_file &&
        (bundle->wants & WORKFLOW_WANT_TESTS) != 0U) {
        workflow_collect_file_tests(bundle, file_id, path);
    }
    cbm_store_free_nodes(nodes, node_count);
    return bundle->graph_error ? CBM_STORE_ERR : CBM_STORE_OK;
}

static void workflow_collect_dependents(workflow_query_bundle_t *bundle) {
    if ((bundle->wants & WORKFLOW_WANT_RELATED) == 0U) {
        return;
    }
    char **files = NULL;
    int file_count = 0;
    if (cbm_store_get_dependent_files(
            bundle->store, bundle->project, bundle->paths,
            (int)bundle->path_count, &files, &file_count) != CBM_STORE_OK) {
        bundle->graph_error = true;
        return;
    }
    for (int i = 0; i < file_count; i++) {
        workflow_related_add(bundle, files[i], bundle->paths[0],
                             WORKFLOW_RELATED_DEPENDENT);
    }
    cbm_store_free_dependent_files(files, file_count);
}

static void workflow_file_stem(const char *path, char *out, size_t out_size) {
    if (!out || out_size == 0U) {
        return;
    }
    out[0] = 0;
    if (!path) {
        return;
    }
    const char *base = strrchr(path, '/');
    const char *backslash = strrchr(path, '\\');
    if (backslash && (!base || backslash > base)) {
        base = backslash;
    }
    base = base ? base + 1 : path;
    char lowered[WORKFLOW_QUERY_FIELD_MAX + 1U];
    size_t length = 0;
    while (base[length] && base[length] != '.' &&
           length < sizeof(lowered) - 1U) {
        lowered[length] = (char)tolower((unsigned char)base[length]);
        length++;
    }
    lowered[length] = 0;
    size_t start = 0;
    if (length >= 5U &&
        (memcmp(lowered, "test_", 5U) == 0 ||
         memcmp(lowered, "spec_", 5U) == 0)) {
        start = 5U;
    }
    size_t end = length;
    if (end >= start + 5U &&
        (strcmp(lowered + end - 5U, "_test") == 0 ||
         strcmp(lowered + end - 5U, "_spec") == 0)) {
        end -= 5U;
    }
    size_t written = 0;
    for (size_t i = start; i < end && written + 1U < out_size; i++) {
        unsigned char c = (unsigned char)lowered[i];
        if (isalnum(c)) {
            out[written++] = (char)c;
        }
    }
    out[written] = 0;
}

static void workflow_collect_test_name_heuristics(
    workflow_query_bundle_t *bundle) {
    if ((bundle->wants & WORKFLOW_WANT_TESTS) == 0U) {
        return;
    }
    char **files = NULL;
    int file_count = 0;
    if (cbm_store_list_files(bundle->store, bundle->project, &files,
                             &file_count) != CBM_STORE_OK) {
        bundle->graph_error = true;
        return;
    }
    if (file_count > 1) {
        qsort(files, (size_t)file_count, sizeof(files[0]),
              workflow_string_pointer_compare);
    }
    for (size_t target = 0; target < bundle->path_count; target++) {
        char target_stem[256];
        workflow_file_stem(bundle->paths[target], target_stem,
                           sizeof(target_stem));
        if (!target_stem[0]) {
            continue;
        }
        for (int i = 0; i < file_count; i++) {
            if (!cbm_is_test_path(files[i]) ||
                workflow_path_compare(files[i], bundle->paths[target]) == 0) {
                continue;
            }
            char candidate_stem[256];
            workflow_file_stem(files[i], candidate_stem,
                               sizeof(candidate_stem));
            if (candidate_stem[0] &&
                strcmp(candidate_stem, target_stem) == 0) {
                workflow_test_add(bundle, NULL, files[i], bundle->paths[target],
                                  NULL, false, false, 0.35);
            }
        }
    }
    for (int i = 0; i < file_count; i++) {
        free(files[i]);
    }
    free(files);
}

static void workflow_emit_related(workflow_query_bundle_t *bundle) {
    for (size_t i = 0; i < bundle->related_count; i++) {
        workflow_related_candidate_t *candidate = &bundle->related[i];
        double rank = 0.35 + candidate->callers * 0.18 +
                      candidate->tests * 0.16 + candidate->callees * 0.12 +
                      candidate->imports * 0.10 +
                      candidate->cochanges * 0.08 +
                      candidate->dependents * 0.05;
        if (rank > 0.94) {
            rank = 0.94;
        }
        char reason_buffer[256];
        (void)snprintf(reason_buffer, sizeof(reason_buffer),
                       "caller=%d; callee=%d; import=%d; test=%d; cochange=%d; dependent=%d",
                       candidate->callers, candidate->callees,
                       candidate->imports, candidate->tests,
                       candidate->cochanges, candidate->dependents);
        const char *reason =
            workflow_query_copy(bundle->result, reason_buffer);
        if (reason) {
            (void)workflow_output_add(
                bundle, candidate->path, NULL, candidate->target_path, NULL,
                "RELATED_FILE", reason, WORKFLOW_OUTPUT_RELATED, rank, 1, 0, 0);
        }
    }
}

static void workflow_emit_tests(workflow_query_bundle_t *bundle) {
    for (size_t i = 0; i < bundle->test_count; i++) {
        workflow_test_candidate_t *candidate = &bundle->tests[i];
        const char *relationship;
        double rank;
        if (candidate->direct_symbol > 0) {
            relationship = "TESTS";
            rank = 1.0;
            bundle->direct_test_count++;
        } else if (candidate->direct_file > 0) {
            relationship = "TESTS_FILE";
            rank = 0.88;
            bundle->direct_test_count++;
        } else {
            relationship = "TEST_NAME_HEURISTIC";
            rank = candidate->heuristic_rank;
        }
        char reason_buffer[192];
        (void)snprintf(reason_buffer, sizeof(reason_buffer),
                       "TESTS=%d; TESTS_FILE=%d; filename_matches=%d",
                       candidate->direct_symbol, candidate->direct_file,
                       candidate->heuristic);
        const char *reason =
            workflow_query_copy(bundle->result, reason_buffer);
        if (reason) {
            (void)workflow_output_add(
                bundle, candidate->path, candidate->qualified_name,
                candidate->target_path, candidate->target_qualified_name,
                relationship, reason, WORKFLOW_OUTPUT_TEST, rank, 1, 0, 0);
        }
    }
    bundle->heuristic_only_tests =
        bundle->test_count > 0U && bundle->direct_test_count == 0U;
}

static void workflow_emit_callers(workflow_query_bundle_t *bundle) {
    for (size_t i = 0; i < bundle->caller_count; i++) {
        workflow_caller_candidate_t *candidate = &bundle->callers[i];
        const char *relationship =
            candidate->distance <= 1 ? "CALLS" : "CALLS_TRANSITIVE";
        char reason_buffer[160];
        (void)snprintf(reason_buffer, sizeof(reason_buffer),
                       "direct graph traversal; distance=%d; occurrences=%d",
                       candidate->distance, candidate->occurrences);
        const char *reason =
            workflow_query_copy(bundle->result, reason_buffer);
        if (reason) {
            (void)workflow_output_add(
                bundle, candidate->path, candidate->qualified_name,
                candidate->target_path, candidate->target_qualified_name,
                relationship, reason, WORKFLOW_OUTPUT_CALLER,
                workflow_caller_rank(candidate->distance), candidate->distance,
                candidate->line_start, candidate->line_end);
        }
    }
    if (!bundle->request->include_file_aggregation) {
        return;
    }
    for (size_t i = 0; i < bundle->caller_file_count; i++) {
        workflow_caller_file_candidate_t *candidate =
            &bundle->caller_files[i];
        char reason_buffer[128];
        (void)snprintf(reason_buffer, sizeof(reason_buffer),
                       "file rollup only; direct_callers=%d",
                       candidate->caller_count);
        const char *reason =
            workflow_query_copy(bundle->result, reason_buffer);
        const char *target_path =
            workflow_query_copy_path(bundle->result, bundle->paths[0]);
        if (reason && target_path) {
            double rank = candidate->strongest_rank * 0.60;
            (void)workflow_output_add(
                bundle, candidate->path, NULL, target_path, NULL,
                "CALLS_FILE_AGGREGATE", reason,
                WORKFLOW_OUTPUT_CALLER_AGGREGATE, rank, 1, 0, 0);
        }
    }
}

static void workflow_emit_routes(workflow_query_bundle_t *bundle) {
    for (size_t i = 0; i < bundle->route_count; i++) {
        workflow_route_candidate_t *candidate = &bundle->routes[i];
        const char *reason = workflow_query_copy(
            bundle->result, "route node connected by a direct route relationship");
        if (reason) {
            (void)workflow_output_add(
                bundle, candidate->path, candidate->qualified_name,
                candidate->target_path, candidate->target_qualified_name,
                "ROUTE_ENTRYPOINT", reason, WORKFLOW_OUTPUT_ROUTE, 0.82, 1,
                candidate->line_start, candidate->line_end);
        }
    }
}

static void workflow_emit_candidates(workflow_query_bundle_t *bundle) {
    if ((bundle->wants & WORKFLOW_WANT_RELATED) != 0U) {
        workflow_emit_related(bundle);
    }
    if ((bundle->wants & WORKFLOW_WANT_TESTS) != 0U) {
        workflow_emit_tests(bundle);
    }
    if ((bundle->wants & WORKFLOW_WANT_CALLERS) != 0U) {
        workflow_emit_callers(bundle);
    }
    if ((bundle->wants & WORKFLOW_WANT_ROUTES) != 0U) {
        workflow_emit_routes(bundle);
    }
    if (bundle->output_count > 1U) {
        qsort(bundle->output, bundle->output_count, sizeof(bundle->output[0]),
              workflow_output_compare);
    }
    size_t related_emitted = 0;
    size_t tests_emitted = 0;
    cbm_workflow_envelope_t *envelope = &bundle->result->envelope;
    for (size_t i = 0; i < bundle->output_count; i++) {
        workflow_output_candidate_t *candidate = &bundle->output[i];
        if (candidate->group == WORKFLOW_OUTPUT_RELATED &&
            related_emitted >= bundle->max_related_files) {
            envelope->evidence_remaining = workflow_saturated_add(
                envelope->evidence_remaining, 1U);
            continue;
        }
        if (candidate->group == WORKFLOW_OUTPUT_TEST &&
            tests_emitted >= bundle->max_tests) {
            envelope->evidence_remaining = workflow_saturated_add(
                envelope->evidence_remaining, 1U);
            continue;
        }
        cbm_workflow_evidence_item_t item = {
            .path = candidate->path,
            .qualified_name = candidate->qualified_name,
            .target_path = candidate->target_path,
            .target_qualified_name = candidate->target_qualified_name,
            .relationship = candidate->relationship,
            .reason = candidate->reason,
            .source_generation = envelope->index.generation,
            .rank = candidate->rank,
            .distance = candidate->distance,
            .line_start = candidate->line_start,
            .line_end = candidate->line_end,
        };
        if (cbm_workflow_envelope_add_evidence(envelope, &item)) {
            if (candidate->group == WORKFLOW_OUTPUT_RELATED) {
                related_emitted++;
            } else if (candidate->group == WORKFLOW_OUTPUT_TEST) {
                tests_emitted++;
            }
        }
    }
}

static void workflow_add_metric(workflow_query_bundle_t *bundle,
                                cbm_workflow_text_list_t *list,
                                const char *name, size_t value) {
    char buffer[160];
    (void)snprintf(buffer, sizeof(buffer), "%s: %zu", name, value);
    workflow_add_text(bundle, list, buffer);
}

static size_t workflow_text_remaining(const cbm_workflow_envelope_t *envelope) {
    size_t remaining = envelope->coverage.missing.remaining;
    remaining = workflow_saturated_add(
        remaining, envelope->coverage.ignored.remaining);
    remaining = workflow_saturated_add(
        remaining, envelope->coverage.unsupported.remaining);
    remaining = workflow_saturated_add(remaining, envelope->risk.factors.remaining);
    remaining = workflow_saturated_add(
        remaining, envelope->confidence.basis.remaining);
    return workflow_saturated_add(remaining, envelope->warnings.remaining);
}

static void workflow_finalize_metadata(workflow_query_bundle_t *bundle,
                                       workflow_view_t view) {
    cbm_workflow_envelope_t *envelope = &bundle->result->envelope;
    bool verified_empty_change_set =
        view == WORKFLOW_VIEW_CHANGE_RISKS && bundle->request->allow_empty &&
        bundle->path_count == 0U && !bundle->graph_error &&
        !bundle->coverage_error && !bundle->storage->oom;
    if (bundle->coverage_incomplete) {
        workflow_add_text(bundle, &envelope->warnings,
                          "source coverage is incomplete for one or more requested paths");
    }
    if (bundle->target_missing) {
        workflow_add_text(bundle, &envelope->warnings,
                          "requested path or symbol has no matching indexed definition");
    }
    if (bundle->file_node_missing &&
        (bundle->wants & (WORKFLOW_WANT_RELATED | WORKFLOW_WANT_TESTS)) != 0U) {
        workflow_add_text(bundle, &envelope->warnings,
                          "File node is unavailable; file-level evidence is incomplete");
    }
    if (bundle->heuristic_only_tests) {
        workflow_add_text(bundle, &envelope->warnings,
                          "test recommendations rely only on filename heuristics");
    }

    if (bundle->graph_error || bundle->coverage_error || bundle->storage->oom) {
        envelope->risk.level = CBM_WORKFLOW_RISK_UNKNOWN;
        envelope->confidence.level = CBM_WORKFLOW_CONFIDENCE_UNKNOWN;
        workflow_add_text(bundle, &envelope->confidence.basis,
                          "a required evidence lookup failed");
    } else if (verified_empty_change_set) {
        envelope->risk.level = CBM_WORKFLOW_RISK_LOW;
        workflow_add_metric(bundle, &envelope->risk.factors,
                            "changed paths", bundle->path_count);
        envelope->confidence.level = CBM_WORKFLOW_CONFIDENCE_HIGH;
        workflow_add_text(
            bundle, &envelope->confidence.basis,
            "read-only working-tree status reported no changed paths");
    } else {
        if (bundle->caller_count >= 8U || bundle->related_count >= 20U) {
            envelope->risk.level = CBM_WORKFLOW_RISK_HIGH;
        } else if (bundle->caller_count > 0U || bundle->route_count > 0U ||
                   bundle->related_count >= 4U) {
            envelope->risk.level = CBM_WORKFLOW_RISK_MEDIUM;
        } else {
            envelope->risk.level = CBM_WORKFLOW_RISK_LOW;
        }
        if (view == WORKFLOW_VIEW_CHANGE_RISKS) {
            workflow_add_metric(bundle, &envelope->risk.factors,
                                "changed paths", bundle->path_count);
        }
        workflow_add_metric(bundle, &envelope->risk.factors,
                            "direct or transitive callers", bundle->caller_count);
        workflow_add_metric(bundle, &envelope->risk.factors,
                            "related files", bundle->related_count);
        workflow_add_metric(bundle, &envelope->risk.factors,
                            "recommended tests", bundle->test_count);
        workflow_add_metric(bundle, &envelope->risk.factors,
                            "route entrypoints", bundle->route_count);

        bool high_confidence =
            envelope->index.freshness == CBM_WORKFLOW_FRESHNESS_FRESH &&
            !bundle->coverage_incomplete && !bundle->missing_relationships &&
            !bundle->target_missing && !bundle->storage->truncated &&
            envelope->evidence_remaining == 0U && !bundle->heuristic_only_tests;
        envelope->confidence.level =
            high_confidence
                ? CBM_WORKFLOW_CONFIDENCE_HIGH
            : bundle->heuristic_only_tests || bundle->missing_relationships
                ? CBM_WORKFLOW_CONFIDENCE_LOW
                : CBM_WORKFLOW_CONFIDENCE_MEDIUM;
        workflow_add_text(bundle, &envelope->confidence.basis,
                          high_confidence
                              ? "fresh source-checked graph evidence"
                          : bundle->missing_relationships
                              ? "one or more relationship classes are absent"
                          : bundle->heuristic_only_tests
                              ? "only naming evidence is available for tests"
                              : "evidence is useful but not fully authoritative");
    }

    bool bounded = bundle->storage->truncated ||
                   envelope->evidence_remaining > 0U;
    if (bounded) {
        workflow_add_text(bundle, &envelope->warnings,
                          "result was bounded after stable ranking");
    }
    if (bundle->graph_error || bundle->coverage_error || bundle->storage->oom) {
        envelope->outcome = CBM_WORKFLOW_OUTCOME_ERROR;
    } else if (verified_empty_change_set) {
        envelope->outcome = CBM_WORKFLOW_OUTCOME_EMPTY_VERIFIED;
    } else if (envelope->index.freshness == CBM_WORKFLOW_FRESHNESS_DIRTY ||
               envelope->index.freshness == CBM_WORKFLOW_FRESHNESS_STALE) {
        envelope->outcome = CBM_WORKFLOW_OUTCOME_STALE;
    } else if (bundle->unsupported && envelope->coverage.covered == 0U) {
        envelope->outcome = CBM_WORKFLOW_OUTCOME_UNSUPPORTED;
    } else if (bundle->coverage_incomplete || bundle->missing_relationships ||
               bundle->target_missing ||
               (bundle->file_node_missing &&
                (bundle->wants &
                 (WORKFLOW_WANT_RELATED | WORKFLOW_WANT_TESTS)) != 0U) ||
               bundle->heuristic_only_tests || bounded ||
               envelope->index.freshness == CBM_WORKFLOW_FRESHNESS_UNKNOWN) {
        envelope->outcome = CBM_WORKFLOW_OUTCOME_PARTIAL;
    } else if (envelope->evidence_count == 0U) {
        envelope->outcome = CBM_WORKFLOW_OUTCOME_EMPTY_VERIFIED;
    } else {
        envelope->outcome = CBM_WORKFLOW_OUTCOME_COMPLETE;
    }

    if (!workflow_set_context_handle(bundle, view)) {
        envelope->outcome = CBM_WORKFLOW_OUTCOME_ERROR;
        bundle->graph_error = true;
    }
    size_t remaining = workflow_saturated_add(
        bundle->storage->dropped, envelope->evidence_remaining);
    remaining = workflow_saturated_add(remaining,
                                       workflow_text_remaining(envelope));
    envelope->omissions.remaining = remaining;
    envelope->omissions.truncated = remaining > 0U;
    envelope->omissions.cursor =
        envelope->omissions.truncated ? envelope->context_handle : NULL;
}

static int workflow_query_execute(cbm_workflow_evidence_gate_t *gate,
                                  const cbm_workflow_query_request_t *request,
                                  cbm_workflow_query_result_t *result,
                                  workflow_view_t view) {
    if (!gate || !gate->snapshot_active || !request || !result ||
        result->_private_storage) {
        return CBM_STORE_ERR;
    }
    if (!workflow_query_result_begin(gate, result)) {
        return CBM_STORE_ERR;
    }
    workflow_query_bundle_t bundle = {
        .result = result,
        .storage = workflow_query_storage(result),
        .request = request,
        .store = gate->store,
        .project = gate->project,
        .wants = workflow_view_wants(view, request->include_routes),
        .max_related_files =
            request->max_related_files > 0U
                ? request->max_related_files
                : 12U,
        .max_tests = request->max_tests > 0U ? request->max_tests : 12U,
        .caller_depth = request->caller_depth > 0 ? request->caller_depth : 1,
    };
    if (bundle.max_related_files > CBM_WORKFLOW_QUERY_MAX_CANDIDATES) {
        bundle.max_related_files = CBM_WORKFLOW_QUERY_MAX_CANDIDATES;
    }
    if (bundle.max_tests > CBM_WORKFLOW_QUERY_MAX_CANDIDATES) {
        bundle.max_tests = CBM_WORKFLOW_QUERY_MAX_CANDIDATES;
    }
    if (bundle.caller_depth > 3) {
        bundle.caller_depth = 3;
    }
    if (!workflow_collect_request_paths(&bundle)) {
        cbm_workflow_query_result_clear(result);
        return CBM_STORE_ERR;
    }
    bool verified_empty_change_set =
        view == WORKFLOW_VIEW_CHANGE_RISKS && request->allow_empty &&
        bundle.path_count == 0U;
    if (bundle.path_count == 0U && !verified_empty_change_set) {
        cbm_workflow_query_result_clear(result);
        return CBM_STORE_ERR;
    }
    if (!verified_empty_change_set &&
        !workflow_load_capabilities(gate, &bundle.capabilities)) {
        bundle.graph_error = true;
    } else if (!verified_empty_change_set) {
        workflow_require_capabilities(&bundle);
    }
    if (!verified_empty_change_set &&
        !workflow_collect_coverage(&bundle, gate)) {
        bundle.coverage_error = true;
    }
    if (!verified_empty_change_set && !bundle.graph_error &&
        !bundle.coverage_error) {
        for (size_t i = 0; i < bundle.path_count; i++) {
            if (workflow_collect_path_graph(&bundle, bundle.paths[i]) !=
                CBM_STORE_OK) {
                break;
            }
        }
        if (!bundle.graph_error) {
            workflow_collect_dependents(&bundle);
        }
        if (!bundle.graph_error) {
            workflow_collect_test_name_heuristics(&bundle);
        }
    }
    workflow_emit_candidates(&bundle);
    workflow_finalize_metadata(&bundle, view);
    return bundle.graph_error || bundle.coverage_error || bundle.storage->oom
               ? CBM_STORE_ERR
               : CBM_STORE_OK;
}

int cbm_workflow_query_file_context(cbm_workflow_evidence_gate_t *gate,
                                    const cbm_workflow_query_request_t *request,
                                    cbm_workflow_query_result_t *result) {
    return workflow_query_execute(gate, request, result,
                                  WORKFLOW_VIEW_FILE_CONTEXT);
}

int cbm_workflow_query_related_files(cbm_workflow_evidence_gate_t *gate,
                                     const cbm_workflow_query_request_t *request,
                                     cbm_workflow_query_result_t *result) {
    return workflow_query_execute(gate, request, result,
                                  WORKFLOW_VIEW_RELATED_FILES);
}

int cbm_workflow_query_tests(cbm_workflow_evidence_gate_t *gate,
                             const cbm_workflow_query_request_t *request,
                             cbm_workflow_query_result_t *result) {
    return workflow_query_execute(gate, request, result, WORKFLOW_VIEW_TESTS);
}

int cbm_workflow_query_callers(cbm_workflow_evidence_gate_t *gate,
                               const cbm_workflow_query_request_t *request,
                               cbm_workflow_query_result_t *result) {
    return workflow_query_execute(gate, request, result, WORKFLOW_VIEW_CALLERS);
}

int cbm_workflow_query_change_risks(cbm_workflow_evidence_gate_t *gate,
                                    const cbm_workflow_query_request_t *request,
                                    cbm_workflow_query_result_t *result) {
    return workflow_query_execute(gate, request, result,
                                  WORKFLOW_VIEW_CHANGE_RISKS);
}

int cbm_workflow_query_edit_plan(cbm_workflow_evidence_gate_t *gate,
                                 const cbm_workflow_query_request_t *request,
                                 cbm_workflow_query_result_t *result) {
    return workflow_query_execute(gate, request, result,
                                  WORKFLOW_VIEW_EDIT_PLAN);
}

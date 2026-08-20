/* Contract tests for deterministic internal workflow query views. */
#include "test_framework.h"
#include "test_helpers.h"

#include "foundation/compat_fs.h"
#include "foundation/platform.h"
#include "foundation/sha256.h"
#include "mcp/workflow_query.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QUERY_PROJECT "workflow-query-fixture"

typedef struct {
    cbm_store_t *store;
    char root[512];
} workflow_query_fixture_t;

typedef int (*workflow_query_fn)(cbm_workflow_evidence_gate_t *,
                                 const cbm_workflow_query_request_t *,
                                 cbm_workflow_query_result_t *);

static bool workflow_query_hash_file(const char *path,
                                     char digest[CBM_SHA256_HEX_LEN + 1U]) {
    FILE *file = cbm_fopen(path, "rb");
    if (!file) {
        return false;
    }
    cbm_sha256_ctx sha;
    cbm_sha256_init(&sha);
    unsigned char buffer[4096];
    size_t count;
    while ((count = fread(buffer, 1, sizeof(buffer), file)) > 0U) {
        cbm_sha256_update(&sha, buffer, count);
    }
    bool ok = !ferror(file);
    if (fclose(file) != 0) {
        ok = false;
    }
    if (!ok) {
        return false;
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

static void workflow_query_fixture_clear(workflow_query_fixture_t *fixture) {
    if (!fixture) {
        return;
    }
    if (fixture->store) {
        cbm_store_close(fixture->store);
    }
    if (fixture->root[0]) {
        (void)th_rmtree(fixture->root);
    }
    memset(fixture, 0, sizeof(*fixture));
}

static int64_t workflow_query_add_node(cbm_store_t *store, const char *label,
                                       const char *name, const char *qualified_name,
                                       const char *file_path, int start_line) {
    cbm_node_t node = {
        .project = QUERY_PROJECT,
        .label = label,
        .name = name,
        .qualified_name = qualified_name,
        .file_path = file_path,
        .start_line = start_line,
        .end_line = start_line + 4,
        .properties_json = "{}",
    };
    return cbm_store_upsert_node(store, &node);
}

static bool workflow_query_add_edge(cbm_store_t *store, int64_t source_id,
                                    int64_t target_id, const char *type) {
    cbm_edge_t edge = {
        .project = QUERY_PROJECT,
        .source_id = source_id,
        .target_id = target_id,
        .type = type,
        .properties_json = "{}",
    };
    return cbm_store_insert_edge(store, &edge) > 0;
}

static bool workflow_query_fixture_init(workflow_query_fixture_t *fixture,
                                        bool include_relationships) {
    memset(fixture, 0, sizeof(*fixture));
    (void)snprintf(fixture->root, sizeof(fixture->root),
                   "%s/cbm-workflow-query-XXXXXX", cbm_tmpdir());
    if (!cbm_mkdtemp(fixture->root)) {
        return false;
    }

    static const struct {
        const char *path;
        const char *content;
    } files[] = {
        {"src/target.c", "int target(void) { return 1; }\n"},
        {"src/caller.c", "int caller(void) { return target(); }\n"},
        {"src/callee.c", "int callee(void) { return 0; }\n"},
        {"src/cochange.c", "int cochange(void) { return 0; }\n"},
        {"tests/test_target.c", "int test_target(void) { return target() != 1; }\n"},
        {"tests/target_spec.c", "int target_spec(void) { return 0; }\n"},
    };
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        if (th_write_file(TH_PATH(fixture->root, files[i].path), files[i].content) != 0) {
            workflow_query_fixture_clear(fixture);
            return false;
        }
    }

    fixture->store = cbm_store_open_memory();
    if (!fixture->store ||
        cbm_store_upsert_project(fixture->store, QUERY_PROJECT, fixture->root) != CBM_STORE_OK) {
        workflow_query_fixture_clear(fixture);
        return false;
    }

    int64_t target_file = workflow_query_add_node(
        fixture->store, "File", "target.c", "fixture.file.target", "src/target.c", 1);
    int64_t caller_file = workflow_query_add_node(
        fixture->store, "File", "caller.c", "fixture.file.caller", "src/caller.c", 1);
    int64_t callee_file = workflow_query_add_node(
        fixture->store, "File", "callee.c", "fixture.file.callee", "src/callee.c", 1);
    int64_t cochange_file = workflow_query_add_node(
        fixture->store, "File", "cochange.c", "fixture.file.cochange", "src/cochange.c", 1);
    int64_t test_file = workflow_query_add_node(
        fixture->store, "File", "test_target.c", "fixture.file.test_target",
        "tests/test_target.c", 1);
    int64_t heuristic_file = workflow_query_add_node(
        fixture->store, "File", "target_spec.c", "fixture.file.target_spec",
        "tests/target_spec.c", 1);
    int64_t target = workflow_query_add_node(
        fixture->store, "Function", "target", "fixture.target", "src/target.c", 3);
    int64_t caller = workflow_query_add_node(
        fixture->store, "Function", "caller", "fixture.caller", "src/caller.c", 3);
    int64_t callee = workflow_query_add_node(
        fixture->store, "Function", "callee", "fixture.callee", "src/callee.c", 3);
    int64_t test = workflow_query_add_node(
        fixture->store, "Function", "test_target", "fixture.test_target",
        "tests/test_target.c", 3);
    int64_t route = workflow_query_add_node(
        fixture->store, "Route", "GET /target", "__route__GET__/target",
        "src/target.c", 1);

    int64_t ids[] = {target_file, caller_file, callee_file, cochange_file, test_file,
                     heuristic_file, target, caller, callee, test, route};
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        if (ids[i] <= 0) {
            workflow_query_fixture_clear(fixture);
            return false;
        }
    }

    if (include_relationships &&
        (!workflow_query_add_edge(fixture->store, caller, target, "CALLS") ||
         !workflow_query_add_edge(fixture->store, target, callee, "CALLS") ||
         !workflow_query_add_edge(fixture->store, test, target, "TESTS") ||
         !workflow_query_add_edge(fixture->store, test_file, target_file, "TESTS_FILE") ||
         !workflow_query_add_edge(fixture->store, target_file, callee_file, "IMPORTS") ||
         !workflow_query_add_edge(fixture->store, target_file, cochange_file,
                                  "FILE_CHANGES_WITH") ||
         !workflow_query_add_edge(fixture->store, target, route, "HANDLES"))) {
        workflow_query_fixture_clear(fixture);
        return false;
    }

    cbm_project_t project = {0};
    if (cbm_store_get_project(fixture->store, QUERY_PROJECT, &project) != CBM_STORE_OK) {
        workflow_query_fixture_clear(fixture);
        return false;
    }
    cbm_coverage_meta_t meta = {
        .generation = project.indexed_at,
        .index_mode = "full",
        .recorded_at = "2026-08-20T00:00:00Z",
        .recording_status = "complete",
        .coverage_version = 1,
        .hash_records_complete = true,
    };
    bool coverage_ok = cbm_store_coverage_replace_ex(fixture->store, QUERY_PROJECT, NULL, 0,
                                                     &meta) == CBM_STORE_OK;
    cbm_project_free_fields(&project);
    if (!coverage_ok) {
        workflow_query_fixture_clear(fixture);
        return false;
    }

    const char *target_disk_path = TH_PATH(fixture->root, "src/target.c");
    cbm_path_info_t info;
    char digest[CBM_SHA256_HEX_LEN + 1U];
    if (cbm_path_info_utf8(target_disk_path, &info) != 0 ||
        !workflow_query_hash_file(target_disk_path, digest) ||
        cbm_store_upsert_file_hash(fixture->store, QUERY_PROJECT, "src/target.c", digest,
                                   info.mtime_ns, info.size) != CBM_STORE_OK) {
        workflow_query_fixture_clear(fixture);
        return false;
    }
    return true;
}

static const cbm_workflow_evidence_item_t *workflow_query_find_item(
    const cbm_workflow_query_result_t *result, const char *path, const char *relationship) {
    for (size_t i = 0; i < result->envelope.evidence_count; i++) {
        const cbm_workflow_evidence_item_t *item = &result->envelope.evidence[i];
        if ((!path || (item->path && strcmp(item->path, path) == 0)) &&
            (!relationship ||
             (item->relationship && strcmp(item->relationship, relationship) == 0))) {
            return item;
        }
    }
    return NULL;
}

static int workflow_query_run(workflow_query_fixture_t *fixture, workflow_query_fn query,
                              const cbm_workflow_query_request_t *request,
                              cbm_workflow_query_result_t *result) {
    cbm_workflow_evidence_gate_t gate;
    if (cbm_workflow_evidence_gate_begin(fixture->store, QUERY_PROJECT, &gate) != CBM_STORE_OK) {
        return CBM_STORE_ERR;
    }
    int rc = query(&gate, request, result);
    int end_rc = cbm_workflow_evidence_gate_end(&gate);
    return rc == CBM_STORE_OK && end_rc == CBM_STORE_OK ? CBM_STORE_OK : CBM_STORE_ERR;
}

TEST(workflow_query_exposes_all_six_internal_views) {
    workflow_query_fixture_t fixture;
    ASSERT_TRUE(workflow_query_fixture_init(&fixture, true));
    cbm_workflow_query_request_t request = {
        .path = "src/target.c",
        .task_type = "fix",
        .max_related_files = 8U,
        .max_tests = 8U,
        .caller_depth = 2,
        .include_file_aggregation = true,
        .include_routes = true,
    };
    workflow_query_fn queries[] = {
        cbm_workflow_query_file_context, cbm_workflow_query_related_files,
        cbm_workflow_query_tests,        cbm_workflow_query_callers,
        cbm_workflow_query_change_risks, cbm_workflow_query_edit_plan,
    };
    for (size_t i = 0; i < sizeof(queries) / sizeof(queries[0]); i++) {
        cbm_workflow_query_result_t result = {0};
        ASSERT_EQ(workflow_query_run(&fixture, queries[i], &request, &result), CBM_STORE_OK);
        ASSERT_GT(result.envelope.evidence_count, 0);
        ASSERT_TRUE(result.envelope.context_handle[0] != 0);
        cbm_workflow_query_result_clear(&result);
    }
    workflow_query_fixture_clear(&fixture);
    PASS();
}

TEST(workflow_query_direct_test_outranks_filename_heuristic) {
    workflow_query_fixture_t fixture;
    ASSERT_TRUE(workflow_query_fixture_init(&fixture, true));
    cbm_workflow_query_request_t request = {
        .path = "src/target.c",
        .max_tests = 8U,
    };
    cbm_workflow_query_result_t result = {0};
    ASSERT_EQ(workflow_query_run(&fixture, cbm_workflow_query_tests, &request, &result),
              CBM_STORE_OK);
    const cbm_workflow_evidence_item_t *direct =
        workflow_query_find_item(&result, "tests/test_target.c", "TESTS");
    const cbm_workflow_evidence_item_t *heuristic =
        workflow_query_find_item(&result, "tests/target_spec.c", "TEST_NAME_HEURISTIC");
    ASSERT_NOT_NULL(direct);
    ASSERT_NOT_NULL(heuristic);
    ASSERT(direct->rank > heuristic->rank);
    cbm_workflow_query_result_clear(&result);
    workflow_query_fixture_clear(&fixture);
    PASS();
}

TEST(workflow_query_file_aggregation_never_masquerades_as_direct_caller) {
    workflow_query_fixture_t fixture;
    ASSERT_TRUE(workflow_query_fixture_init(&fixture, true));
    cbm_workflow_query_request_t request = {
        .path = "src/target.c",
        .caller_depth = 2,
        .include_file_aggregation = true,
    };
    cbm_workflow_query_result_t result = {0};
    ASSERT_EQ(workflow_query_run(&fixture, cbm_workflow_query_callers, &request, &result),
              CBM_STORE_OK);
    const cbm_workflow_evidence_item_t *direct =
        workflow_query_find_item(&result, "src/caller.c", "CALLS");
    const cbm_workflow_evidence_item_t *aggregate =
        workflow_query_find_item(&result, "src/caller.c", "CALLS_FILE_AGGREGATE");
    ASSERT_NOT_NULL(direct);
    ASSERT_NOT_NULL(direct->qualified_name);
    ASSERT_NOT_NULL(aggregate);
    ASSERT_TRUE(!aggregate->qualified_name || aggregate->qualified_name[0] == 0);
    ASSERT(direct->rank > aggregate->rank);
    cbm_workflow_query_result_clear(&result);
    workflow_query_fixture_clear(&fixture);
    PASS();
}

TEST(workflow_query_file_context_and_tests_share_direct_evidence) {
    workflow_query_fixture_t fixture;
    ASSERT_TRUE(workflow_query_fixture_init(&fixture, true));
    cbm_workflow_query_request_t request = {
        .path = "src/target.c",
        .max_related_files = 8U,
        .max_tests = 8U,
    };
    cbm_workflow_query_result_t context = {0};
    cbm_workflow_query_result_t tests = {0};
    ASSERT_EQ(workflow_query_run(&fixture, cbm_workflow_query_file_context, &request, &context),
              CBM_STORE_OK);
    ASSERT_EQ(workflow_query_run(&fixture, cbm_workflow_query_tests, &request, &tests),
              CBM_STORE_OK);
    const cbm_workflow_evidence_item_t *context_test =
        workflow_query_find_item(&context, "tests/test_target.c", "TESTS");
    const cbm_workflow_evidence_item_t *tests_test =
        workflow_query_find_item(&tests, "tests/test_target.c", "TESTS");
    ASSERT_NOT_NULL(context_test);
    ASSERT_NOT_NULL(tests_test);
    ASSERT(context_test->rank == tests_test->rank);
    ASSERT_STR_EQ(context_test->reason, tests_test->reason);
    cbm_workflow_query_result_clear(&context);
    cbm_workflow_query_result_clear(&tests);
    workflow_query_fixture_clear(&fixture);
    PASS();
}

TEST(workflow_query_fixed_graph_renders_byte_stably) {
    workflow_query_fixture_t fixture;
    ASSERT_TRUE(workflow_query_fixture_init(&fixture, true));
    cbm_workflow_query_request_t request = {
        .path = "src/target.c",
        .task_type = "refactor",
        .max_related_files = 8U,
        .max_tests = 8U,
        .caller_depth = 2,
        .include_file_aggregation = true,
        .include_routes = true,
    };
    cbm_workflow_query_result_t first = {0};
    cbm_workflow_query_result_t second = {0};
    ASSERT_EQ(workflow_query_run(&fixture, cbm_workflow_query_edit_plan, &request, &first),
              CBM_STORE_OK);
    ASSERT_EQ(workflow_query_run(&fixture, cbm_workflow_query_edit_plan, &request, &second),
              CBM_STORE_OK);
    char *first_text = cbm_workflow_envelope_render_compact(&first.envelope, 65536U, false);
    char *second_text = cbm_workflow_envelope_render_compact(&second.envelope, 65536U, false);
    ASSERT_NOT_NULL(first_text);
    ASSERT_NOT_NULL(second_text);
    ASSERT_STR_EQ(first_text, second_text);
    free(first_text);
    free(second_text);
    cbm_workflow_query_result_clear(&first);
    cbm_workflow_query_result_clear(&second);
    workflow_query_fixture_clear(&fixture);
    PASS();
}

TEST(workflow_query_missing_relationship_classes_are_partial) {
    workflow_query_fixture_t fixture;
    ASSERT_TRUE(workflow_query_fixture_init(&fixture, false));
    cbm_workflow_query_request_t request = {
        .path = "src/target.c",
        .max_related_files = 8U,
        .max_tests = 8U,
    };
    cbm_workflow_query_result_t result = {0};
    ASSERT_EQ(workflow_query_run(&fixture, cbm_workflow_query_file_context, &request, &result),
              CBM_STORE_OK);
    ASSERT_EQ(result.envelope.outcome, CBM_WORKFLOW_OUTCOME_PARTIAL);
    ASSERT_NEQ(result.envelope.confidence.level, CBM_WORKFLOW_CONFIDENCE_HIGH);
    ASSERT_GT(result.envelope.warnings.count, 0);
    cbm_workflow_query_result_clear(&result);
    workflow_query_fixture_clear(&fixture);
    PASS();
}

TEST(workflow_query_every_evidence_item_explains_its_rank) {
    workflow_query_fixture_t fixture;
    ASSERT_TRUE(workflow_query_fixture_init(&fixture, true));
    cbm_workflow_query_request_t request = {
        .path = "src/target.c",
        .task_type = "investigate",
        .max_related_files = 8U,
        .max_tests = 8U,
        .caller_depth = 2,
        .include_file_aggregation = true,
        .include_routes = true,
    };
    cbm_workflow_query_result_t result = {0};
    ASSERT_EQ(workflow_query_run(&fixture, cbm_workflow_query_edit_plan, &request, &result),
              CBM_STORE_OK);
    ASSERT_GT(result.envelope.evidence_count, 0);
    for (size_t i = 0; i < result.envelope.evidence_count; i++) {
        const cbm_workflow_evidence_item_t *item = &result.envelope.evidence[i];
        ASSERT_NOT_NULL(item->relationship);
        ASSERT_TRUE(item->relationship[0] != 0);
        ASSERT_NOT_NULL(item->reason);
        ASSERT_TRUE(item->reason[0] != 0);
        ASSERT_NOT_NULL(item->source_generation);
        ASSERT_TRUE(item->source_generation[0] != 0);
    }
    cbm_workflow_query_result_clear(&result);
    workflow_query_fixture_clear(&fixture);
    PASS();
}

SUITE(workflow_query) {
    RUN_TEST(workflow_query_exposes_all_six_internal_views);
    RUN_TEST(workflow_query_direct_test_outranks_filename_heuristic);
    RUN_TEST(workflow_query_file_aggregation_never_masquerades_as_direct_caller);
    RUN_TEST(workflow_query_file_context_and_tests_share_direct_evidence);
    RUN_TEST(workflow_query_fixed_graph_renders_byte_stably);
    RUN_TEST(workflow_query_missing_relationship_classes_are_partial);
    RUN_TEST(workflow_query_every_evidence_item_explains_its_rank);
}

/* Contract tests for the transport-independent workflow evidence envelope. */
#include "test_framework.h"
#include "test_helpers.h"

#include "foundation/compat_fs.h"
#include "foundation/platform.h"
#include "foundation/sha256.h"
#include "mcp/workflow_evidence.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cbm_workflow_envelope_t workflow_test_envelope(void) {
    cbm_workflow_envelope_t envelope;
    cbm_workflow_envelope_init(&envelope);
    envelope.outcome = CBM_WORKFLOW_OUTCOME_COMPLETE;
    envelope.project.id = "project-id";
    envelope.project.display_name = "fixture";
    envelope.project.canonical_root = "/tmp/fixture";
    envelope.project.resolution = "explicit";
    envelope.index.generation = "generation-a";
    envelope.index.freshness = CBM_WORKFLOW_FRESHNESS_FRESH;
    envelope.index.source_fingerprint = "fingerprint-a";
    envelope.coverage.requested = 1U;
    envelope.coverage.covered = 1U;
    envelope.risk.level = CBM_WORKFLOW_RISK_MEDIUM;
    envelope.confidence.level = CBM_WORKFLOW_CONFIDENCE_HIGH;
    (void)snprintf(envelope.context_handle, sizeof(envelope.context_handle), "wf1:fixture");
    return envelope;
}

TEST(workflow_envelope_keeps_risk_and_confidence_independent) {
    cbm_workflow_envelope_t envelope = workflow_test_envelope();
    envelope.risk.level = CBM_WORKFLOW_RISK_HIGH;
    envelope.confidence.level = CBM_WORKFLOW_CONFIDENCE_LOW;

    char *rendered = cbm_workflow_envelope_render_compact(&envelope, 8192U, false);
    ASSERT_NOT_NULL(rendered);
    ASSERT_NOT_NULL(strstr(rendered, "risk_level: high"));
    ASSERT_NOT_NULL(strstr(rendered, "confidence_level: low"));
    free(rendered);
    PASS();
}

TEST(workflow_envelope_ties_use_normalized_path_then_qualified_name) {
    cbm_workflow_envelope_t envelope = workflow_test_envelope();
    cbm_workflow_evidence_item_t z_path = {
        .id = "item-z-path-marker",
        .path = "src/z.c",
        .qualified_name = "alpha",
        .relationship = "CALLS",
        .reason = "same rank",
        .source_generation = "generation-a",
        .rank = 1.0,
    };
    cbm_workflow_evidence_item_t a_name_b = {
        .id = "item-a-name-b-marker",
        .path = "src\\a.c",
        .qualified_name = "beta",
        .relationship = "CALLS",
        .reason = "same rank",
        .source_generation = "generation-a",
        .rank = 1.0,
    };
    cbm_workflow_evidence_item_t a_name_a = {
        .id = "item-a-name-a-marker",
        .path = "src/a.c",
        .qualified_name = "alpha",
        .relationship = "CALLS",
        .reason = "same rank",
        .source_generation = "generation-a",
        .rank = 1.0,
    };
    ASSERT_TRUE(cbm_workflow_envelope_add_evidence(&envelope, &z_path));
    ASSERT_TRUE(cbm_workflow_envelope_add_evidence(&envelope, &a_name_b));
    ASSERT_TRUE(cbm_workflow_envelope_add_evidence(&envelope, &a_name_a));

    char *rendered = cbm_workflow_envelope_render_compact(&envelope, 8192U, false);
    ASSERT_NOT_NULL(rendered);
    const char *a_name_a_pos = strstr(rendered, "item-a-name-a-marker");
    const char *a_name_b_pos = strstr(rendered, "item-a-name-b-marker");
    const char *z_path_pos = strstr(rendered, "item-z-path-marker");
    ASSERT_NOT_NULL(a_name_a_pos);
    ASSERT_NOT_NULL(a_name_b_pos);
    ASSERT_NOT_NULL(z_path_pos);
    ASSERT_TRUE(a_name_a_pos < a_name_b_pos);
    ASSERT_TRUE(a_name_b_pos < z_path_pos);
    free(rendered);
    PASS();
}

static void workflow_fill_text_list(cbm_workflow_text_list_t *list) {
    for (size_t i = 0; i < CBM_WORKFLOW_TEXT_MAX_ITEMS + 2U; i++) {
        (void)cbm_workflow_text_list_add(list, "bounded-item");
    }
}

TEST(workflow_envelope_reports_remaining_for_every_truncated_section) {
    cbm_workflow_envelope_t envelope = workflow_test_envelope();
    workflow_fill_text_list(&envelope.coverage.missing);
    workflow_fill_text_list(&envelope.coverage.ignored);
    workflow_fill_text_list(&envelope.coverage.unsupported);
    workflow_fill_text_list(&envelope.risk.factors);
    workflow_fill_text_list(&envelope.confidence.basis);
    workflow_fill_text_list(&envelope.warnings);
    for (size_t i = 0; i < CBM_WORKFLOW_EVIDENCE_MAX_ITEMS + 2U; i++) {
        cbm_workflow_evidence_item_t item = {
            .path = "src/bounded.c",
            .qualified_name = "bounded",
            .relationship = "TESTS",
            .reason = "bounded evidence",
            .source_generation = "generation-a",
            .rank = 1.0,
        };
        (void)snprintf(item.id, sizeof(item.id), "bounded-%02zu", i);
        (void)cbm_workflow_envelope_add_evidence(&envelope, &item);
    }

    char *rendered = cbm_workflow_envelope_render_compact(&envelope, 65536U, false);
    ASSERT_NOT_NULL(rendered);
    ASSERT_NOT_NULL(strstr(rendered, "coverage_missing_remaining: 2"));
    ASSERT_NOT_NULL(strstr(rendered, "coverage_ignored_remaining: 2"));
    ASSERT_NOT_NULL(strstr(rendered, "coverage_unsupported_remaining: 2"));
    ASSERT_NOT_NULL(strstr(rendered, "evidence_remaining: 2"));
    ASSERT_NOT_NULL(strstr(rendered, "risk_factors_remaining: 2"));
    ASSERT_NOT_NULL(strstr(rendered, "confidence_basis_remaining: 2"));
    ASSERT_NOT_NULL(strstr(rendered, "warnings_remaining: 2"));
    ASSERT_NOT_NULL(strstr(rendered, "omissions_truncated: true"));
    free(rendered);
    PASS();
}

TEST(workflow_context_handle_binds_every_normalized_identifier) {
    char base[CBM_WORKFLOW_CONTEXT_HANDLE_SIZE];
    char project[CBM_WORKFLOW_CONTEXT_HANDLE_SIZE];
    char generation[CBM_WORKFLOW_CONTEXT_HANDLE_SIZE];
    char tool[CBM_WORKFLOW_CONTEXT_HANDLE_SIZE];
    char query[CBM_WORKFLOW_CONTEXT_HANDLE_SIZE];
    ASSERT_TRUE(cbm_workflow_context_handle(base, sizeof(base), "project-a", "generation-a",
                                            "get_edit_plan", "src/a.c|fix"));
    ASSERT_TRUE(cbm_workflow_context_handle(project, sizeof(project), "project-b", "generation-a",
                                            "get_edit_plan", "src/a.c|fix"));
    ASSERT_TRUE(cbm_workflow_context_handle(generation, sizeof(generation), "project-a",
                                            "generation-b", "get_edit_plan", "src/a.c|fix"));
    ASSERT_TRUE(cbm_workflow_context_handle(tool, sizeof(tool), "project-a", "generation-a",
                                            "get_change_risks", "src/a.c|fix"));
    ASSERT_TRUE(cbm_workflow_context_handle(query, sizeof(query), "project-a", "generation-a",
                                            "get_edit_plan", "src/b.c|fix"));
    ASSERT_TRUE(strcmp(base, project) != 0);
    ASSERT_TRUE(strcmp(base, generation) != 0);
    ASSERT_TRUE(strcmp(base, tool) != 0);
    ASSERT_TRUE(strcmp(base, query) != 0);
    ASSERT_NULL(strstr(base, "project-a"));
    ASSERT_NULL(strstr(base, "generation-a"));
    PASS();
}

TEST(workflow_renderer_sanitizes_control_and_invalid_utf8) {
    cbm_workflow_envelope_t envelope = workflow_test_envelope();
    char dirty_path[] = {'s', 'r', 'c', '/', 'b', 'a', 'd', 1, (char)0xff, '.', 'c', 0};
    cbm_workflow_evidence_item_t item = {
        .id = "dirty-path-marker",
        .path = dirty_path,
        .qualified_name = "dirty",
        .relationship = "CALLS",
        .reason = "sanitize",
        .source_generation = "generation-a",
        .rank = 1.0,
    };
    ASSERT_TRUE(cbm_workflow_envelope_add_evidence(&envelope, &item));

    char *rendered = cbm_workflow_envelope_render_compact(&envelope, 8192U, false);
    ASSERT_NOT_NULL(rendered);
    ASSERT_NOT_NULL(strstr(rendered, "\\u0001"));
    ASSERT_NOT_NULL(strstr(rendered, "\xEF\xBF\xBD"));
    for (const unsigned char *p = (const unsigned char *)rendered; *p; p++) {
        ASSERT_TRUE(*p != 1U);
        ASSERT_TRUE(*p != 0xffU);
    }
    free(rendered);
    PASS();
}

TEST(workflow_compact_renderer_omits_snippets_unless_requested) {
    cbm_workflow_envelope_t envelope = workflow_test_envelope();
    cbm_workflow_evidence_item_t item = {
        .id = "snippet-marker",
        .path = "src/main.c",
        .qualified_name = "main",
        .relationship = "DEFINES",
        .reason = "target",
        .source_generation = "generation-a",
        .snippet = "SOURCE_SNIPPET_SENTINEL",
        .rank = 1.0,
    };
    ASSERT_TRUE(cbm_workflow_envelope_add_evidence(&envelope, &item));

    char *compact = cbm_workflow_envelope_render_compact(&envelope, 8192U, false);
    char *detailed = cbm_workflow_envelope_render_compact(&envelope, 8192U, true);
    ASSERT_NOT_NULL(compact);
    ASSERT_NOT_NULL(detailed);
    ASSERT_NULL(strstr(compact, "SOURCE_SNIPPET_SENTINEL"));
    ASSERT_NOT_NULL(strstr(detailed, "SOURCE_SNIPPET_SENTINEL"));
    free(compact);
    free(detailed);
    PASS();
}

TEST(workflow_renderer_never_exceeds_its_byte_budget) {
    cbm_workflow_envelope_t envelope = workflow_test_envelope();
    const char *long_reason =
        "01234567890123456789012345678901234567890123456789012345678901234567890123456789"
        "01234567890123456789012345678901234567890123456789012345678901234567890123456789";
    for (size_t i = 0; i < CBM_WORKFLOW_EVIDENCE_MAX_ITEMS; i++) {
        cbm_workflow_evidence_item_t item = {
            .path = "src/a-very-long-but-bounded-path.c",
            .qualified_name = "fixture.qualified.name",
            .relationship = "CALLS",
            .reason = long_reason,
            .source_generation = "generation-a",
            .rank = 1.0,
        };
        (void)snprintf(item.id, sizeof(item.id), "budget-%02zu", i);
        ASSERT_TRUE(cbm_workflow_envelope_add_evidence(&envelope, &item));
    }

    char *rendered = cbm_workflow_envelope_render_compact(
        &envelope, CBM_WORKFLOW_RENDER_MIN_BYTES, false);
    ASSERT_NOT_NULL(rendered);
    ASSERT_TRUE(strlen(rendered) <= CBM_WORKFLOW_RENDER_MIN_BYTES);
    ASSERT_NOT_NULL(strstr(rendered, "omissions_truncated: true"));
    ASSERT_NOT_NULL(strstr(rendered, "evidence_remaining:"));
    free(rendered);
    PASS();
}

TEST(workflow_gate_rejects_non_normalized_relative_paths) {
    cbm_store_t *store = cbm_store_open_memory();
    ASSERT_NOT_NULL(store);
    ASSERT_EQ(cbm_store_upsert_project(store, "fixture", cbm_tmpdir()), CBM_STORE_OK);
    cbm_workflow_evidence_gate_t gate;
    ASSERT_EQ(cbm_workflow_evidence_gate_begin(store, "fixture", &gate), CBM_STORE_OK);
    const char *unsafe[] = {
        "../escape.c",
        "/tmp/absolute.c",
        "C:/drive.c",
        "\\\\server\\share.c",
        "src\\windows-separator.c",
        "./src/dot.c",
        "src//empty-component.c",
        "src/\x01control.c",
    };
    for (size_t i = 0; i < sizeof(unsafe) / sizeof(unsafe[0]); i++) {
        cbm_workflow_path_evidence_t evidence = {0};
        ASSERT_EQ(cbm_workflow_evidence_gate_check_path(&gate, unsafe[i], &evidence),
                  CBM_STORE_ERR);
    }
    ASSERT_EQ((int)gate.requested, 0);
    ASSERT_EQ(cbm_workflow_evidence_gate_end(&gate), CBM_STORE_OK);
    cbm_store_close(store);
    PASS();
}

static bool workflow_test_hash_file(const char *path,
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
    bool ok = !ferror(file) && fclose(file) == 0;
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

TEST(workflow_gate_full_coverage_requires_content_hash_match) {
    char root[512];
    (void)snprintf(root, sizeof(root), "%s/cbm-workflow-hash-XXXXXX", cbm_tmpdir());
    ASSERT_NOT_NULL(cbm_mkdtemp(root));
    char path[640];
    (void)snprintf(path, sizeof(path), "%s/main.c", root);
    ASSERT_EQ(th_write_file(path, "int main(void) { return 0; }\n"), 0);
    cbm_path_info_t info;
    ASSERT_EQ(cbm_path_info_utf8(path, &info), 0);

    cbm_store_t *store = cbm_store_open_memory();
    ASSERT_NOT_NULL(store);
    ASSERT_EQ(cbm_store_upsert_project(store, "fixture", root), CBM_STORE_OK);
    cbm_project_t project = {0};
    ASSERT_EQ(cbm_store_get_project(store, "fixture", &project), CBM_STORE_OK);
    cbm_coverage_meta_t meta = {
        .generation = project.indexed_at,
        .index_mode = "full",
        .recorded_at = "2026-08-20T00:00:00Z",
        .recording_status = "complete",
        .coverage_version = 1,
        .hash_records_complete = true,
    };
    ASSERT_EQ(cbm_store_coverage_replace_ex(store, "fixture", NULL, 0, &meta), CBM_STORE_OK);
    cbm_project_free_fields(&project);

    const char *wrong_digest =
        "0000000000000000000000000000000000000000000000000000000000000000";
    ASSERT_EQ(cbm_store_upsert_file_hash(store, "fixture", "main.c", wrong_digest, info.mtime_ns,
                                         info.size),
              CBM_STORE_OK);
    cbm_workflow_evidence_gate_t gate;
    ASSERT_EQ(cbm_workflow_evidence_gate_begin(store, "fixture", &gate), CBM_STORE_OK);
    cbm_workflow_path_evidence_t evidence = {0};
    ASSERT_EQ(cbm_workflow_evidence_gate_check_path(&gate, "main.c", &evidence), CBM_STORE_OK);
    ASSERT_TRUE(evidence.metadata_matches);
    ASSERT_TRUE(evidence.content_hash_available);
    ASSERT_FALSE(evidence.content_hash_matches);
    ASSERT_FALSE(evidence.full_coverage);
    cbm_workflow_evidence_path_clear(&evidence);
    ASSERT_EQ(cbm_workflow_evidence_gate_end(&gate), CBM_STORE_OK);

    char digest[CBM_SHA256_HEX_LEN + 1U];
    ASSERT_TRUE(workflow_test_hash_file(path, digest));
    ASSERT_EQ(cbm_store_upsert_file_hash(store, "fixture", "main.c", digest, info.mtime_ns,
                                         info.size),
              CBM_STORE_OK);
    ASSERT_EQ(cbm_workflow_evidence_gate_begin(store, "fixture", &gate), CBM_STORE_OK);
    ASSERT_EQ(cbm_workflow_evidence_gate_check_path(&gate, "main.c", &evidence), CBM_STORE_OK);
    ASSERT_TRUE(evidence.content_hash_matches);
    ASSERT_TRUE(evidence.full_coverage);
    cbm_workflow_evidence_path_clear(&evidence);
    ASSERT_EQ(cbm_workflow_evidence_gate_end(&gate), CBM_STORE_OK);

    cbm_store_close(store);
    ASSERT_EQ(cbm_unlink(path), 0);
    ASSERT_EQ(cbm_rmdir(root), 0);
    PASS();
}

TEST(workflow_gate_keeps_unproven_generation_freshness_unknown) {
    cbm_store_t *store = cbm_store_open_memory();
    ASSERT_NOT_NULL(store);
    ASSERT_EQ(cbm_store_upsert_project(store, "fixture", cbm_tmpdir()), CBM_STORE_OK);
    cbm_workflow_evidence_gate_t gate;
    ASSERT_EQ(cbm_workflow_evidence_gate_begin(store, "fixture", &gate), CBM_STORE_OK);
    ASSERT_EQ(gate.generation_state, CBM_WORKFLOW_GENERATION_UNKNOWN);
    ASSERT_EQ(gate.freshness, CBM_WORKFLOW_FRESHNESS_UNKNOWN);

    cbm_workflow_path_evidence_t evidence = {0};
    ASSERT_EQ(cbm_workflow_evidence_gate_check_path(
                  &gate, "cbm-workflow-evidence-definitely-missing.c", &evidence),
              CBM_STORE_OK);
    ASSERT_EQ(evidence.freshness, CBM_WORKFLOW_FRESHNESS_UNKNOWN);
    ASSERT_EQ(gate.freshness, CBM_WORKFLOW_FRESHNESS_UNKNOWN);
    cbm_workflow_evidence_path_clear(&evidence);
    ASSERT_EQ(cbm_workflow_evidence_gate_end(&gate), CBM_STORE_OK);
    cbm_store_close(store);
    PASS();
}

SUITE(workflow_evidence) {
    RUN_TEST(workflow_envelope_keeps_risk_and_confidence_independent);
    RUN_TEST(workflow_envelope_ties_use_normalized_path_then_qualified_name);
    RUN_TEST(workflow_envelope_reports_remaining_for_every_truncated_section);
    RUN_TEST(workflow_context_handle_binds_every_normalized_identifier);
    RUN_TEST(workflow_renderer_sanitizes_control_and_invalid_utf8);
    RUN_TEST(workflow_compact_renderer_omits_snippets_unless_requested);
    RUN_TEST(workflow_renderer_never_exceeds_its_byte_budget);
    RUN_TEST(workflow_gate_rejects_non_normalized_relative_paths);
    RUN_TEST(workflow_gate_keeps_unproven_generation_freshness_unknown);
    RUN_TEST(workflow_gate_full_coverage_requires_content_hash_match);
}

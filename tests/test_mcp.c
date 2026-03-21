/*
 * test_mcp.c — Tests for the MCP server module.
 *
 * Covers: JSON-RPC parsing, MCP protocol, tool dispatch, tool handlers.
 */
#include "../src/foundation/compat.h"
#include "test_framework.h"
#include <mcp/mcp.h>
#include <store/store.h>
#include <yyjson/yyjson.h>
#include <string.h>
#include <stdlib.h>

/* ══════════════════════════════════════════════════════════════════
 *  JSON-RPC PARSING
 * ══════════════════════════════════════════════════════════════════ */

TEST(jsonrpc_parse_request) {
    const char *line = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
                       "\"params\":{\"capabilities\":{}}}";
    cbm_jsonrpc_request_t req = {0};
    int rc = cbm_jsonrpc_parse(line, &req);
    ASSERT_EQ(rc, 0);
    ASSERT_STR_EQ(req.jsonrpc, "2.0");
    ASSERT_STR_EQ(req.method, "initialize");
    ASSERT_EQ(req.id, 1);
    ASSERT_TRUE(req.has_id);
    ASSERT_NOT_NULL(req.params_raw);
    cbm_jsonrpc_request_free(&req);
    PASS();
}

TEST(jsonrpc_parse_notification) {
    const char *line = "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";
    cbm_jsonrpc_request_t req = {0};
    int rc = cbm_jsonrpc_parse(line, &req);
    ASSERT_EQ(rc, 0);
    ASSERT_STR_EQ(req.method, "notifications/initialized");
    ASSERT_FALSE(req.has_id);
    cbm_jsonrpc_request_free(&req);
    PASS();
}

TEST(jsonrpc_parse_invalid) {
    cbm_jsonrpc_request_t req = {0};
    int rc = cbm_jsonrpc_parse("not json", &req);
    ASSERT_EQ(rc, -1);
    cbm_jsonrpc_request_free(&req);
    PASS();
}

TEST(jsonrpc_parse_tools_call) {
    const char *line = "{\"jsonrpc\":\"2.0\",\"id\":42,\"method\":\"tools/call\","
                       "\"params\":{\"name\":\"search_graph\","
                       "\"arguments\":{\"label\":\"Function\",\"limit\":5}}}";
    cbm_jsonrpc_request_t req = {0};
    int rc = cbm_jsonrpc_parse(line, &req);
    ASSERT_EQ(rc, 0);
    ASSERT_STR_EQ(req.method, "tools/call");
    ASSERT_EQ(req.id, 42);
    ASSERT_NOT_NULL(req.params_raw);
    cbm_jsonrpc_request_free(&req);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  JSON-RPC FORMATTING
 * ══════════════════════════════════════════════════════════════════ */

TEST(jsonrpc_format_response) {
    cbm_jsonrpc_response_t resp = {
        .id = 1,
        .result_json = "{\"name\":\"codebase-memory-mcp\"}",
    };
    char *json = cbm_jsonrpc_format_response(&resp);
    ASSERT_NOT_NULL(json);
    /* Should contain jsonrpc, id, and result */
    ASSERT_NOT_NULL(strstr(json, "\"jsonrpc\":\"2.0\""));
    ASSERT_NOT_NULL(strstr(json, "\"id\":1"));
    ASSERT_NOT_NULL(strstr(json, "\"result\""));
    free(json);
    PASS();
}

TEST(jsonrpc_format_error) {
    char *json = cbm_jsonrpc_format_error(5, -32600, "Invalid Request");
    ASSERT_NOT_NULL(json);
    ASSERT_NOT_NULL(strstr(json, "\"id\":5"));
    ASSERT_NOT_NULL(strstr(json, "\"error\""));
    ASSERT_NOT_NULL(strstr(json, "-32600"));
    ASSERT_NOT_NULL(strstr(json, "Invalid Request"));
    free(json);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  MCP PROTOCOL HELPERS
 * ══════════════════════════════════════════════════════════════════ */

TEST(mcp_initialize_response) {
    /* Default (no params): returns latest supported version */
    char *json = cbm_mcp_initialize_response(NULL);
    ASSERT_NOT_NULL(json);
    ASSERT_NOT_NULL(strstr(json, "codebase-memory-mcp"));
    ASSERT_NOT_NULL(strstr(json, "capabilities"));
    ASSERT_NOT_NULL(strstr(json, "tools"));
    ASSERT_NOT_NULL(strstr(json, "2025-11-25"));
    free(json);

    /* Client requests a supported version: server echoes it */
    json = cbm_mcp_initialize_response("{\"protocolVersion\":\"2024-11-05\"}");
    ASSERT_NOT_NULL(json);
    ASSERT_NOT_NULL(strstr(json, "2024-11-05"));
    free(json);

    json = cbm_mcp_initialize_response("{\"protocolVersion\":\"2025-06-18\"}");
    ASSERT_NOT_NULL(json);
    ASSERT_NOT_NULL(strstr(json, "2025-06-18"));
    free(json);

    /* Client requests unknown version: server returns its latest */
    json = cbm_mcp_initialize_response("{\"protocolVersion\":\"9999-01-01\"}");
    ASSERT_NOT_NULL(json);
    ASSERT_NOT_NULL(strstr(json, "2025-11-25"));
    free(json);
    PASS();
}

TEST(mcp_tools_list) {
    char *json = cbm_mcp_tools_list();
    ASSERT_NOT_NULL(json);
    /* Should contain all advertised tools */
    ASSERT_NOT_NULL(strstr(json, "index_repository"));
    ASSERT_NOT_NULL(strstr(json, "get_file_context"));
    ASSERT_NOT_NULL(strstr(json, "get_related_files"));
    ASSERT_NOT_NULL(strstr(json, "get_tests"));
    ASSERT_NOT_NULL(strstr(json, "get_callers"));
    ASSERT_NOT_NULL(strstr(json, "get_change_risks"));
    ASSERT_NOT_NULL(strstr(json, "get_edit_plan"));
    ASSERT_NOT_NULL(strstr(json, "search_graph"));
    ASSERT_NOT_NULL(strstr(json, "query_graph"));
    ASSERT_NOT_NULL(strstr(json, "trace_call_path"));
    ASSERT_NOT_NULL(strstr(json, "get_code_snippet"));
    ASSERT_NOT_NULL(strstr(json, "get_graph_schema"));
    ASSERT_NOT_NULL(strstr(json, "get_architecture"));
    ASSERT_NOT_NULL(strstr(json, "search_code"));
    ASSERT_NOT_NULL(strstr(json, "list_projects"));
    ASSERT_NOT_NULL(strstr(json, "delete_project"));
    ASSERT_NOT_NULL(strstr(json, "index_status"));
    ASSERT_NOT_NULL(strstr(json, "detect_changes"));
    ASSERT_NOT_NULL(strstr(json, "manage_adr"));
    ASSERT_NOT_NULL(strstr(json, "ingest_traces"));
    free(json);
    PASS();
}

TEST(mcp_tools_array_schemas_have_items) {
    /* VS Code 1.112+ rejects array schemas without "items" (see
     * https://github.com/microsoft/vscode/issues/248810).
     * Walk every tool's inputSchema and verify that every "type":"array"
     * property also contains "items". */
    char *json = cbm_mcp_tools_list();
    ASSERT_NOT_NULL(json);

    /* Scan for all occurrences of "type":"array" — each must be followed
     * by "items" before the next closing brace of that property. */
    const char *p = json;
    while ((p = strstr(p, "\"type\":\"array\"")) != NULL) {
        /* Find the enclosing '}' for this property object */
        const char *end = strchr(p, '}');
        ASSERT_NOT_NULL(end);
        /* "items" must appear between p and end */
        size_t span = (size_t)(end - p);
        char *segment = malloc(span + 1);
        memcpy(segment, p, span);
        segment[span] = '\0';
        ASSERT_NOT_NULL(strstr(segment, "\"items\"")); /* array missing items */
        free(segment);
        p = end;
    }

    free(json);
    PASS();
}

TEST(mcp_text_result) {
    char *json = cbm_mcp_text_result("{\"total\":5}", false);
    ASSERT_NOT_NULL(json);
    ASSERT_NOT_NULL(strstr(json, "\"type\":\"text\""));
    /* The text value is JSON-escaped inside the "text" field */
    ASSERT_NOT_NULL(strstr(json, "total"));
    ASSERT_NULL(strstr(json, "\"isError\":true"));
    free(json);
    PASS();
}

TEST(mcp_text_result_error) {
    char *json = cbm_mcp_text_result("something failed", true);
    ASSERT_NOT_NULL(json);
    ASSERT_NOT_NULL(strstr(json, "\"isError\":true"));
    ASSERT_NOT_NULL(strstr(json, "something failed"));
    free(json);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  ARGUMENT EXTRACTION
 * ══════════════════════════════════════════════════════════════════ */

TEST(mcp_get_tool_name) {
    const char *params = "{\"name\":\"search_graph\",\"arguments\":{\"label\":\"Function\"}}";
    char *name = cbm_mcp_get_tool_name(params);
    ASSERT_NOT_NULL(name);
    ASSERT_STR_EQ(name, "search_graph");
    free(name);
    PASS();
}

TEST(mcp_get_arguments) {
    const char *params =
        "{\"name\":\"search_graph\",\"arguments\":{\"label\":\"Function\",\"limit\":5}}";
    char *args = cbm_mcp_get_arguments(params);
    ASSERT_NOT_NULL(args);
    ASSERT_NOT_NULL(strstr(args, "\"label\":\"Function\""));
    ASSERT_NOT_NULL(strstr(args, "\"limit\":5"));
    free(args);
    PASS();
}

TEST(mcp_get_string_arg) {
    const char *args = "{\"label\":\"Function\",\"name_pattern\":\".*Order.*\"}";
    char *val = cbm_mcp_get_string_arg(args, "label");
    ASSERT_NOT_NULL(val);
    ASSERT_STR_EQ(val, "Function");
    free(val);

    val = cbm_mcp_get_string_arg(args, "name_pattern");
    ASSERT_NOT_NULL(val);
    ASSERT_STR_EQ(val, ".*Order.*");
    free(val);

    val = cbm_mcp_get_string_arg(args, "nonexistent");
    ASSERT_NULL(val);
    PASS();
}

TEST(mcp_get_int_arg) {
    const char *args = "{\"limit\":10,\"offset\":5}";
    int val = cbm_mcp_get_int_arg(args, "limit", 0);
    ASSERT_EQ(val, 10);
    val = cbm_mcp_get_int_arg(args, "offset", 0);
    ASSERT_EQ(val, 5);
    val = cbm_mcp_get_int_arg(args, "missing", 42);
    ASSERT_EQ(val, 42);
    PASS();
}

TEST(mcp_get_bool_arg) {
    const char *args = "{\"include_connected\":true,\"regex\":false}";
    bool val = cbm_mcp_get_bool_arg(args, "include_connected");
    ASSERT_TRUE(val);
    val = cbm_mcp_get_bool_arg(args, "regex");
    ASSERT_FALSE(val);
    val = cbm_mcp_get_bool_arg(args, "missing");
    ASSERT_FALSE(val);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  SERVER HANDLE — PROTOCOL FLOW
 * ══════════════════════════════════════════════════════════════════ */

TEST(server_handle_initialize) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
                                   "\"params\":{\"capabilities\":{}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"id\":1"));
    ASSERT_NOT_NULL(strstr(resp, "codebase-memory-mcp"));
    ASSERT_NOT_NULL(strstr(resp, "capabilities"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(server_handle_initialized_notification) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    /* Notification has no id → no response */
    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");
    ASSERT_NULL(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(server_handle_tools_list) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"id\":2"));
    ASSERT_NOT_NULL(strstr(resp, "search_graph"));
    ASSERT_NOT_NULL(strstr(resp, "query_graph"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(server_handle_unknown_method) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"unknown/method\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"error\""));
    ASSERT_NOT_NULL(strstr(resp, "-32601")); /* Method not found */
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  TOOL HANDLERS (via server_handle)
 * ══════════════════════════════════════════════════════════════════ */

/* Helper: create a server with an in-memory store populated with test data */
static cbm_mcp_server_t *setup_mcp_with_data(void) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL); /* NULL = in-memory */
    return srv;
}

TEST(tool_list_projects_empty) {
    cbm_mcp_server_t *srv = setup_mcp_with_data();

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"list_projects\",\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"id\":10"));
    /* Should return a result (possibly empty list) */
    ASSERT_NOT_NULL(strstr(resp, "\"result\""));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_get_graph_schema_empty) {
    cbm_mcp_server_t *srv = setup_mcp_with_data();

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"get_graph_schema\",\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"result\""));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_unknown_tool) {
    cbm_mcp_server_t *srv = setup_mcp_with_data();

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":12,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"nonexistent_tool\",\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    /* Should return result with isError */
    ASSERT_NOT_NULL(strstr(resp, "isError"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_search_graph_basic) {
    cbm_mcp_server_t *srv = setup_mcp_with_data();

    /* search_graph with no project → should work on empty store */
    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":13,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"search_graph\","
                                   "\"arguments\":{\"label\":\"Function\",\"limit\":10}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"result\""));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_query_graph_basic) {
    cbm_mcp_server_t *srv = setup_mcp_with_data();

    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":14,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"query_graph\","
             "\"arguments\":{\"query\":\"MATCH (f:Function) RETURN f.name\"}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"result\""));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_index_status_no_project) {
    cbm_mcp_server_t *srv = setup_mcp_with_data();

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":15,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"index_status\",\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    /* Should return error or empty status */
    ASSERT_NOT_NULL(strstr(resp, "\"result\""));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  TOOL HANDLERS WITH DATA
 * ══════════════════════════════════════════════════════════════════ */

TEST(tool_trace_call_path_not_found) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":20,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"trace_call_path\","
                                   "\"arguments\":{\"function_name\":\"NonExistent\"}}}");
    ASSERT_NOT_NULL(resp);
    /* Should return error about function not found */
    ASSERT_NOT_NULL(strstr(resp, "not found"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_trace_missing_function_name) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":21,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"trace_call_path\","
                                   "\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "required"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_delete_project_not_found) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":22,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"delete_project\","
                                   "\"arguments\":{\"project_name\":\"nonexistent\"}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "not_found"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_get_architecture_empty) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":24,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"get_architecture\",\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"result\""));
    ASSERT_NOT_NULL(strstr(resp, "total_nodes"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_query_graph_missing_query) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":23,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"query_graph\","
                                   "\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    /* Should return error about missing query */
    ASSERT_NOT_NULL(strstr(resp, "required"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  PIPELINE-DEPENDENT TOOL HANDLERS
 * ══════════════════════════════════════════════════════════════════ */

TEST(tool_index_repository_missing_path) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":30,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"index_repository\","
                                   "\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "required"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_get_code_snippet_missing_qn) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":31,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"get_code_snippet\","
                                   "\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "required"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_get_code_snippet_not_found) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":32,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"get_code_snippet\","
                                   "\"arguments\":{\"qualified_name\":\"nonexistent.func\"}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "not found"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_search_code_missing_pattern) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":33,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"search_code\","
                                   "\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "required"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_search_code_no_project) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":34,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"search_code\","
                                   "\"arguments\":{\"pattern\":\"func main\"}}}");
    ASSERT_NOT_NULL(resp);
    /* No project indexed → error */
    ASSERT_TRUE(strstr(resp, "not found") || strstr(resp, "not indexed"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_detect_changes_no_project) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":35,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"detect_changes\","
                                   "\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "not found"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_manage_adr_no_project) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":36,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"manage_adr\","
                                   "\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "not found"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_ingest_traces_basic) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":37,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"ingest_traces\","
             "\"arguments\":{\"traces\":[{\"caller\":\"a\",\"callee\":\"b\"}]}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "accepted"));
    ASSERT_NOT_NULL(strstr(resp, "traces_received"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_ingest_traces_empty) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":38,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"ingest_traces\","
                                   "\"arguments\":{\"traces\":[]}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "accepted"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  IDLE STORE EVICTION
 * ══════════════════════════════════════════════════════════════════ */

TEST(store_idle_eviction) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    cbm_mcp_server_set_project(srv, "test-evict");

    /* Trigger resolve_store via a tool call to set store_last_used */
    char *resp = cbm_mcp_handle_tool(srv, "get_graph_schema", "{\"project\":\"test-evict\"}");
    free(resp);

    ASSERT_TRUE(cbm_mcp_server_has_cached_store(srv));

    /* Evict with 0s timeout → should evict immediately */
    cbm_mcp_server_evict_idle(srv, 0);
    ASSERT_FALSE(cbm_mcp_server_has_cached_store(srv));

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(store_idle_no_eviction_within_timeout) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    cbm_mcp_server_set_project(srv, "test-evict");

    char *resp = cbm_mcp_handle_tool(srv, "get_graph_schema", "{\"project\":\"test-evict\"}");
    free(resp);

    ASSERT_TRUE(cbm_mcp_server_has_cached_store(srv));

    /* Evict with large timeout → should NOT evict */
    cbm_mcp_server_evict_idle(srv, 99999);
    ASSERT_TRUE(cbm_mcp_server_has_cached_store(srv));

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(store_idle_evict_protects_initial_store) {
    /* Evicting with NULL server should not crash */
    cbm_mcp_server_evict_idle(NULL, 0);

    /* Evicting server whose store was never accessed via a named project
     * should NOT evict the initial in-memory store (store_last_used == 0). */
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_TRUE(cbm_mcp_server_has_cached_store(srv));
    cbm_mcp_server_evict_idle(srv, 0);
    ASSERT_TRUE(cbm_mcp_server_has_cached_store(srv));

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(store_idle_evict_access_resets_timer) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    cbm_mcp_server_set_project(srv, "test-evict");

    /* First access */
    char *resp = cbm_mcp_handle_tool(srv, "get_graph_schema", "{\"project\":\"test-evict\"}");
    free(resp);

    /* Second access (resets timer) */
    resp = cbm_mcp_handle_tool(srv, "get_graph_schema", "{\"project\":\"test-evict\"}");
    free(resp);

    ASSERT_TRUE(cbm_mcp_server_has_cached_store(srv));

    /* With large timeout, store should survive */
    cbm_mcp_server_evict_idle(srv, 99999);
    ASSERT_TRUE(cbm_mcp_server_has_cached_store(srv));

    /* With 0 timeout, store should be evicted */
    cbm_mcp_server_evict_idle(srv, 0);
    ASSERT_FALSE(cbm_mcp_server_has_cached_store(srv));

    cbm_mcp_server_free(srv);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  URI HELPERS
 * ══════════════════════════════════════════════════════════════════ */

TEST(parse_file_uri_unix) {
    char path[256];
    ASSERT_TRUE(cbm_parse_file_uri("file:///home/user/project", path, sizeof(path)));
    ASSERT_STR_EQ(path, "/home/user/project");

    ASSERT_TRUE(cbm_parse_file_uri("file:///tmp/test", path, sizeof(path)));
    ASSERT_STR_EQ(path, "/tmp/test");

    ASSERT_TRUE(cbm_parse_file_uri("file:///", path, sizeof(path)));
    ASSERT_STR_EQ(path, "/");
    PASS();
}

TEST(parse_file_uri_windows) {
    char path[256];
    /* Windows drive letter — leading / stripped */
    ASSERT_TRUE(cbm_parse_file_uri("file:///C:/Users/project", path, sizeof(path)));
    ASSERT_STR_EQ(path, "C:/Users/project");

    ASSERT_TRUE(cbm_parse_file_uri("file:///D:/Projects/myapp", path, sizeof(path)));
    ASSERT_STR_EQ(path, "D:/Projects/myapp");
    PASS();
}

TEST(parse_file_uri_invalid) {
    char path[256];
    /* Non-file URI */
    ASSERT_FALSE(cbm_parse_file_uri("https://example.com", path, sizeof(path)));
    ASSERT_STR_EQ(path, "");

    /* Empty string */
    ASSERT_FALSE(cbm_parse_file_uri("", path, sizeof(path)));
    ASSERT_STR_EQ(path, "");

    /* NULL */
    ASSERT_FALSE(cbm_parse_file_uri(NULL, path, sizeof(path)));
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  SNIPPET TESTS — Port of internal/tools/snippet_test.go
 * ══════════════════════════════════════════════════════════════════ */

#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

/* Create an MCP server pre-populated with nodes/edges matching Go testSnippetServer.
 * Writes a source file to tmp_dir/project/main.go.
 * Caller must free the server with cbm_mcp_server_free and
 * unlink the source file + rmdir manually. */
static cbm_mcp_server_t *setup_snippet_server(char *tmp_dir, size_t tmp_sz) {
    /* Create temp dir */
    snprintf(tmp_dir, tmp_sz, "/tmp/cbm_snippet_test_XXXXXX");
    if (!cbm_mkdtemp(tmp_dir))
        return NULL;

    char proj_dir[512];
    snprintf(proj_dir, sizeof(proj_dir), "%s/project", tmp_dir);
    cbm_mkdir(proj_dir);

    /* Write sample source file */
    char src_path[512];
    snprintf(src_path, sizeof(src_path), "%s/main.go", proj_dir);
    FILE *fp = fopen(src_path, "w");
    if (!fp)
        return NULL;
    fprintf(fp, "package main\n"
                "\n"
                "func HandleRequest() error {\n"
                "\treturn nil\n"
                "}\n"
                "\n"
                "func ProcessOrder(id int) {\n"
                "\t// process\n"
                "}\n"
                "\n"
                "func Run() {\n"
                "\t// server\n"
                "}\n");
    fclose(fp);

    /* Create server with in-memory store */
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    if (!srv)
        return NULL;

    cbm_store_t *st = cbm_mcp_server_store(srv);
    if (!st) {
        cbm_mcp_server_free(srv);
        return NULL;
    }

    const char *proj_name = "test-project";
    cbm_mcp_server_set_project(srv, proj_name);
    cbm_store_upsert_project(st, proj_name, proj_dir);

    /* Create nodes */
    cbm_node_t n_hr = {0};
    n_hr.project = proj_name;
    n_hr.label = "Function";
    n_hr.name = "HandleRequest";
    n_hr.qualified_name = "test-project.cmd.server.main.HandleRequest";
    n_hr.file_path = "main.go";
    n_hr.start_line = 3;
    n_hr.end_line = 5;
    n_hr.properties_json = "{\"signature\":\"func HandleRequest() error\","
                           "\"return_type\":\"error\","
                           "\"is_exported\":true}";
    int64_t id_hr = cbm_store_upsert_node(st, &n_hr);

    cbm_node_t n_po = {0};
    n_po.project = proj_name;
    n_po.label = "Function";
    n_po.name = "ProcessOrder";
    n_po.qualified_name = "test-project.cmd.server.main.ProcessOrder";
    n_po.file_path = "main.go";
    n_po.start_line = 7;
    n_po.end_line = 9;
    n_po.properties_json = "{\"signature\":\"func ProcessOrder(id int)\"}";
    int64_t id_po = cbm_store_upsert_node(st, &n_po);

    cbm_node_t n_run1 = {0};
    n_run1.project = proj_name;
    n_run1.label = "Function";
    n_run1.name = "Run";
    n_run1.qualified_name = "test-project.cmd.server.Run";
    n_run1.file_path = "main.go";
    n_run1.start_line = 11;
    n_run1.end_line = 13;
    int64_t id_run1 = cbm_store_upsert_node(st, &n_run1);

    cbm_node_t n_run2 = {0};
    n_run2.project = proj_name;
    n_run2.label = "Function";
    n_run2.name = "Run";
    n_run2.qualified_name = "test-project.cmd.worker.Run";
    n_run2.file_path = "main.go";
    n_run2.start_line = 11;
    n_run2.end_line = 13;
    cbm_store_upsert_node(st, &n_run2);

    /* Create edges: HandleRequest -> ProcessOrder, HandleRequest -> Run1 */
    cbm_edge_t e1 = {.project = proj_name, .source_id = id_hr, .target_id = id_po, .type = "CALLS"};
    cbm_store_insert_edge(st, &e1);

    cbm_edge_t e2 = {
        .project = proj_name, .source_id = id_hr, .target_id = id_run1, .type = "CALLS"};
    cbm_store_insert_edge(st, &e2);
    (void)id_run1; /* run1 used for edge above */

    return srv;
}

/* Cleanup temp files created by setup_snippet_server */
static void cleanup_snippet_dir(const char *tmp_dir) {
    char path[512];
    snprintf(path, sizeof(path), "%s/project/main.go", tmp_dir);
    unlink(path);
    snprintf(path, sizeof(path), "%s/project", tmp_dir);
    rmdir(path);
    rmdir(tmp_dir);
}

/* Extract the inner "text" value from an MCP tool result JSON.
 * The MCP envelope is: {"content":[{"type":"text","text":"<inner json>"}]}
 * This returns the unescaped inner JSON. Caller must free. */
static char *extract_text_content(const char *mcp_result) {
    if (!mcp_result)
        return NULL;
    yyjson_doc *doc = yyjson_read(mcp_result, strlen(mcp_result), 0);
    if (!doc)
        return strdup(mcp_result); /* fallback */
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *content = yyjson_obj_get(root, "content");
    if (!content || !yyjson_is_arr(content)) {
        yyjson_doc_free(doc);
        return strdup(mcp_result);
    }
    yyjson_val *item = yyjson_arr_get(content, 0);
    if (!item) {
        yyjson_doc_free(doc);
        return strdup(mcp_result);
    }
    yyjson_val *text = yyjson_obj_get(item, "text");
    const char *str = yyjson_get_str(text);
    char *result = str ? strdup(str) : strdup(mcp_result);
    yyjson_doc_free(doc);
    return result;
}

/* Call get_code_snippet and extract inner text content.
 * Caller must free returned string. */
static char *call_snippet(cbm_mcp_server_t *srv, const char *args_json) {
    char *raw = cbm_mcp_handle_tool(srv, "get_code_snippet", args_json);
    char *text = extract_text_content(raw);
    free(raw);
    return text;
}

static cbm_mcp_server_t *setup_file_context_server(char *tmp_dir, size_t tmp_sz) {
    snprintf(tmp_dir, tmp_sz, "/tmp/cbm_file_context_test_XXXXXX");
    if (!cbm_mkdtemp(tmp_dir))
        return NULL;

    char proj_dir[512];
    snprintf(proj_dir, sizeof(proj_dir), "%s/project", tmp_dir);
    cbm_mkdir(proj_dir);

    char main_path[512];
    snprintf(main_path, sizeof(main_path), "%s/main.go", proj_dir);
    FILE *main_fp = fopen(main_path, "w");
    if (!main_fp)
        return NULL;
    fprintf(main_fp, "package main\n\nfunc HandleRequest() error {\n\treturn nil\n}\n");
    fclose(main_fp);

    char other_path[512];
    snprintf(other_path, sizeof(other_path), "%s/other.go", proj_dir);
    FILE *other_fp = fopen(other_path, "w");
    if (!other_fp)
        return NULL;
    fprintf(other_fp, "package main\n\nfunc HelperCaller() {\n\tHandleRequest()\n}\n");
    fclose(other_fp);

    char test_path[512];
    snprintf(test_path, sizeof(test_path), "%s/main_test.go", proj_dir);
    FILE *test_fp = fopen(test_path, "w");
    if (!test_fp)
        return NULL;
    fprintf(test_fp, "package main\n\nfunc TestHandleRequest(t *testing.T) {\n\tHandleRequest()\n}\n");
    fclose(test_fp);

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    if (!srv)
        return NULL;

    cbm_store_t *st = cbm_mcp_server_store(srv);
    if (!st) {
        cbm_mcp_server_free(srv);
        return NULL;
    }

    const char *proj_name = "test-project";
    cbm_mcp_server_set_project(srv, proj_name);
    cbm_store_upsert_project(st, proj_name, proj_dir);

    cbm_node_t file_main = {0};
    file_main.project = proj_name;
    file_main.label = "File";
    file_main.name = "main.go";
    file_main.file_path = "main.go";
    int64_t id_file_main = cbm_store_upsert_node(st, &file_main);

    cbm_node_t file_other = {0};
    file_other.project = proj_name;
    file_other.label = "File";
    file_other.name = "other.go";
    file_other.file_path = "other.go";
    int64_t id_file_other = cbm_store_upsert_node(st, &file_other);

    cbm_node_t file_test = {0};
    file_test.project = proj_name;
    file_test.label = "File";
    file_test.name = "main_test.go";
    file_test.file_path = "main_test.go";
    int64_t id_file_test = cbm_store_upsert_node(st, &file_test);

    cbm_node_t fn_target = {0};
    fn_target.project = proj_name;
    fn_target.label = "Function";
    fn_target.name = "HandleRequest";
    fn_target.qualified_name = "test-project.main.HandleRequest";
    fn_target.file_path = "main.go";
    fn_target.start_line = 3;
    fn_target.end_line = 5;
    int64_t id_fn_target = cbm_store_upsert_node(st, &fn_target);

    cbm_node_t fn_caller = {0};
    fn_caller.project = proj_name;
    fn_caller.label = "Function";
    fn_caller.name = "HelperCaller";
    fn_caller.qualified_name = "test-project.main.HelperCaller";
    fn_caller.file_path = "other.go";
    fn_caller.start_line = 3;
    fn_caller.end_line = 5;
    int64_t id_fn_caller = cbm_store_upsert_node(st, &fn_caller);

    cbm_node_t fn_test = {0};
    fn_test.project = proj_name;
    fn_test.label = "Function";
    fn_test.name = "TestHandleRequest";
    fn_test.qualified_name = "test-project.main_test.TestHandleRequest";
    fn_test.file_path = "main_test.go";
    fn_test.start_line = 3;
    fn_test.end_line = 5;
    int64_t id_fn_test = cbm_store_upsert_node(st, &fn_test);

    cbm_node_t route = {0};
    route.project = proj_name;
    route.label = "Route";
    route.name = "GET /handle";
    route.file_path = "main.go";
    int64_t id_route = cbm_store_upsert_node(st, &route);

    cbm_edge_t call_edge = {
        .project = proj_name, .source_id = id_fn_caller, .target_id = id_fn_target, .type = "CALLS"};
    cbm_store_insert_edge(st, &call_edge);

    cbm_edge_t tests_edge = {
        .project = proj_name, .source_id = id_fn_test, .target_id = id_fn_target, .type = "TESTS"};
    cbm_store_insert_edge(st, &tests_edge);

    cbm_edge_t handles_edge = {
        .project = proj_name, .source_id = id_fn_target, .target_id = id_route, .type = "HANDLES"};
    cbm_store_insert_edge(st, &handles_edge);

    cbm_edge_t tests_file_edge = {.project = proj_name,
                                  .source_id = id_file_test,
                                  .target_id = id_file_main,
                                  .type = "TESTS_FILE"};
    cbm_store_insert_edge(st, &tests_file_edge);

    cbm_edge_t imports_edge = {.project = proj_name,
                               .source_id = id_file_other,
                               .target_id = id_file_main,
                               .type = "IMPORTS"};
    cbm_store_insert_edge(st, &imports_edge);

    cbm_edge_t cochange_edge = {.project = proj_name,
                                .source_id = id_file_main,
                                .target_id = id_file_other,
                                .type = "FILE_CHANGES_WITH"};
    cbm_store_insert_edge(st, &cochange_edge);

    return srv;
}

static void cleanup_file_context_dir(const char *tmp_dir) {
    char path[512];
    snprintf(path, sizeof(path), "%s/project/main.go", tmp_dir);
    unlink(path);
    snprintf(path, sizeof(path), "%s/project/other.go", tmp_dir);
    unlink(path);
    snprintf(path, sizeof(path), "%s/project/main_test.go", tmp_dir);
    unlink(path);
    snprintf(path, sizeof(path), "%s/project", tmp_dir);
    rmdir(path);
    rmdir(tmp_dir);
}

TEST(tool_get_file_context_missing_path) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);

    char *raw = cbm_mcp_handle_tool(srv, "get_file_context", "{}");
    ASSERT_NOT_NULL(raw);
    ASSERT_NOT_NULL(strstr(raw, "\"isError\":true"));

    char *text = extract_text_content(raw);
    ASSERT_NOT_NULL(text);
    ASSERT_NOT_NULL(strstr(text, "path is required"));
    free(text);
    free(raw);
    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_get_file_context_basic) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_file_context_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *raw = cbm_mcp_handle_tool(
        srv, "get_file_context",
        "{\"path\":\"main.go\",\"project\":\"test-project\",\"max_related_files\":5,\"max_tests\":5}");
    ASSERT_NOT_NULL(raw);
    ASSERT_NULL(strstr(raw, "\"isError\":true"));

    char *text = extract_text_content(raw);
    ASSERT_NOT_NULL(text);
    ASSERT_NOT_NULL(strstr(text, "\"tool_version\":\"1.0\""));
    ASSERT_NOT_NULL(strstr(text, "\"path\":\"main.go\""));
    ASSERT_NOT_NULL(strstr(text, "\"symbols\""));
    ASSERT_NOT_NULL(strstr(text, "\"related_files\""));
    ASSERT_NOT_NULL(strstr(text, "\"callers\""));
    ASSERT_NOT_NULL(strstr(text, "\"tests\""));
    ASSERT_NOT_NULL(strstr(text, "\"language\":\"go\""));
    ASSERT_NOT_NULL(strstr(text, "\"pitfalls\""));
    ASSERT_NOT_NULL(strstr(text, "\"test_command_hints\""));
    ASSERT_NOT_NULL(strstr(text, "\"tier\":\"unit\""));
    ASSERT_NOT_NULL(strstr(text, "other.go"));
    ASSERT_NOT_NULL(strstr(text, "main_test.go"));
    ASSERT_NOT_NULL(strstr(text, "HelperCaller"));
    free(text);
    free(raw);

    cbm_mcp_server_free(srv);
    cleanup_file_context_dir(tmp);
    PASS();
}

TEST(tool_get_file_context_options) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_file_context_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *raw = cbm_mcp_handle_tool(
        srv, "get_file_context",
        "{\"path\":\"main.go\",\"project\":\"test-project\",\"include_callers\":false,"
        "\"include_snippets\":true}");
    ASSERT_NOT_NULL(raw);
    ASSERT_NULL(strstr(raw, "\"isError\":true"));

    char *text = extract_text_content(raw);
    ASSERT_NOT_NULL(text);
    ASSERT_NOT_NULL(strstr(text, "\"include_callers\":false"));
    ASSERT_NOT_NULL(strstr(text, "\"include_snippets\":true"));
    ASSERT_NOT_NULL(strstr(text, "\"callers\":[]"));
    ASSERT_NOT_NULL(strstr(text, "\"snippet_preview\""));
    free(text);
    free(raw);

    cbm_mcp_server_free(srv);
    cleanup_file_context_dir(tmp);
    PASS();
}

TEST(tool_get_related_files_missing_path) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);

    char *raw = cbm_mcp_handle_tool(srv, "get_related_files", "{}");
    ASSERT_NOT_NULL(raw);
    ASSERT_NOT_NULL(strstr(raw, "\"isError\":true"));

    char *text = extract_text_content(raw);
    ASSERT_NOT_NULL(text);
    ASSERT_NOT_NULL(strstr(text, "path is required"));
    free(text);
    free(raw);
    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_get_related_files_basic) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_file_context_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *raw = cbm_mcp_handle_tool(
        srv, "get_related_files",
        "{\"path\":\"main.go\",\"project\":\"test-project\",\"limit\":5}");
    ASSERT_NOT_NULL(raw);
    ASSERT_NULL(strstr(raw, "\"isError\":true"));

    char *text = extract_text_content(raw);
    ASSERT_NOT_NULL(text);
    ASSERT_NOT_NULL(strstr(text, "\"tool_version\":\"1.0\""));
    ASSERT_NOT_NULL(strstr(text, "\"target\""));
    ASSERT_NOT_NULL(strstr(text, "\"related_files\""));
    ASSERT_NOT_NULL(strstr(text, "other.go"));
    ASSERT_NOT_NULL(strstr(text, "main_test.go"));
    ASSERT_NOT_NULL(strstr(text, "\"relationship_types\""));
    free(text);
    free(raw);

    cbm_mcp_server_free(srv);
    cleanup_file_context_dir(tmp);
    PASS();
}

TEST(tool_get_tests_missing_paths) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);

    char *raw = cbm_mcp_handle_tool(srv, "get_tests", "{}");
    ASSERT_NOT_NULL(raw);
    ASSERT_NOT_NULL(strstr(raw, "\"isError\":true"));

    char *text = extract_text_content(raw);
    ASSERT_NOT_NULL(text);
    ASSERT_NOT_NULL(strstr(text, "paths or path is required"));
    free(text);
    free(raw);
    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_get_tests_basic) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_file_context_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *raw = cbm_mcp_handle_tool(
        srv, "get_tests",
        "{\"paths\":[\"main.go\"],\"project\":\"test-project\",\"limit\":5,\"include_reasons\":true}");
    ASSERT_NOT_NULL(raw);
    ASSERT_NULL(strstr(raw, "\"isError\":true"));

    char *text = extract_text_content(raw);
    ASSERT_NOT_NULL(text);
    ASSERT_NOT_NULL(strstr(text, "\"tool_version\":\"1.0\""));
    ASSERT_NOT_NULL(strstr(text, "\"targets\""));
    ASSERT_NOT_NULL(strstr(text, "\"tests\""));
    ASSERT_NOT_NULL(strstr(text, "main_test.go"));
    ASSERT_NOT_NULL(strstr(text, "\"reason\""));
    ASSERT_NOT_NULL(strstr(text, "\"tier\":\"unit\""));
    ASSERT_NOT_NULL(strstr(text, "\"runner\":\"go\""));
    ASSERT_NOT_NULL(strstr(text, "\"command\":\"go test ./...\""));
    free(text);
    free(raw);

    cbm_mcp_server_free(srv);
    cleanup_file_context_dir(tmp);
    PASS();
}

TEST(tool_get_callers_missing_target) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);

    char *raw = cbm_mcp_handle_tool(srv, "get_callers", "{}");
    ASSERT_NOT_NULL(raw);
    ASSERT_NOT_NULL(strstr(raw, "\"isError\":true"));

    char *text = extract_text_content(raw);
    ASSERT_NOT_NULL(text);
    ASSERT_NOT_NULL(strstr(text, "symbol or path is required"));
    free(text);
    free(raw);
    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_get_callers_symbol_basic) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_file_context_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *raw = cbm_mcp_handle_tool(
        srv, "get_callers",
        "{\"symbol\":\"HandleRequest\",\"project\":\"test-project\",\"depth\":2,"
        "\"include_file_aggregation\":true}");
    ASSERT_NOT_NULL(raw);
    ASSERT_NULL(strstr(raw, "\"isError\":true"));

    char *text = extract_text_content(raw);
    ASSERT_NOT_NULL(text);
    ASSERT_NOT_NULL(strstr(text, "\"tool_version\":\"1.0\""));
    ASSERT_NOT_NULL(strstr(text, "\"matches\""));
    ASSERT_NOT_NULL(strstr(text, "\"callers\""));
    ASSERT_NOT_NULL(strstr(text, "\"by_file\""));
    ASSERT_NOT_NULL(strstr(text, "\"resolved_qualified_name\":\"test-project.main.HandleRequest\""));
    ASSERT_NOT_NULL(strstr(text, "HelperCaller"));
    ASSERT_NOT_NULL(strstr(text, "\"path\":\"other.go\""));
    ASSERT_NOT_NULL(strstr(text, "\"depth\":1"));
    free(text);
    free(raw);

    cbm_mcp_server_free(srv);
    cleanup_file_context_dir(tmp);
    PASS();
}

TEST(tool_get_change_risks_missing_target) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);

    char *raw = cbm_mcp_handle_tool(srv, "get_change_risks", "{}");
    ASSERT_NOT_NULL(raw);
    ASSERT_NOT_NULL(strstr(raw, "\"isError\":true"));

    char *text = extract_text_content(raw);
    ASSERT_NOT_NULL(text);
    ASSERT_NOT_NULL(strstr(text, "paths or diff_mode is required"));
    free(text);
    free(raw);
    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_get_change_risks_basic) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_file_context_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *raw = cbm_mcp_handle_tool(
        srv, "get_change_risks",
        "{\"paths\":[\"main.go\",\"other.go\"],\"project\":\"test-project\",\"include_tests\":true,"
        "\"include_routes\":true}");
    ASSERT_NOT_NULL(raw);
    ASSERT_NULL(strstr(raw, "\"isError\":true"));

    char *text = extract_text_content(raw);
    ASSERT_NOT_NULL(text);
    ASSERT_NOT_NULL(strstr(text, "\"tool_version\":\"1.0\""));
    ASSERT_NOT_NULL(strstr(text, "\"affected_symbols\""));
    ASSERT_NOT_NULL(strstr(text, "\"affected_routes\""));
    ASSERT_NOT_NULL(strstr(text, "\"related_tests\""));
    ASSERT_NOT_NULL(strstr(text, "\"test_command_hints\""));
    ASSERT_NOT_NULL(strstr(text, "\"risk_factors\""));
    ASSERT_NOT_NULL(strstr(text, "\"pitfalls\""));
    ASSERT_NOT_NULL(strstr(text, "\"blast_radius_score\""));
    ASSERT_NOT_NULL(strstr(text, "\"indexed_targets\""));
    ASSERT_NOT_NULL(strstr(text, "\"missing_targets\":[]"));
    ASSERT_NOT_NULL(strstr(text, "main_test.go"));
    ASSERT_NOT_NULL(strstr(text, "multi_file_change"));
    ASSERT_NOT_NULL(strstr(text, "route_and_service_changed"));
    ASSERT_NOT_NULL(strstr(text, "HandleRequest"));
    ASSERT_NOT_NULL(strstr(text, "HelperCaller"));
    ASSERT_NOT_NULL(strstr(text, "\"relationship_types\""));
    ASSERT_NOT_NULL(strstr(text, "\"priority_score\""));
    ASSERT_NOT_NULL(strstr(text, "\"primary_relationship\""));
    ASSERT_NOT_NULL(strstr(text, "\"primary_relationship\":\"TESTS\""));
    ASSERT_NOT_NULL(strstr(text, "\"tier\":\"unit\""));
    ASSERT_NOT_NULL(strstr(text, "\"runner\":\"go\""));
    free(text);
    free(raw);

    cbm_mcp_server_free(srv);
    cleanup_file_context_dir(tmp);
    PASS();
}

TEST(tool_get_change_risks_compact_options) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_file_context_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *raw = cbm_mcp_handle_tool(
        srv, "get_change_risks",
        "{\"paths\":[\"main.go\",\"other.go\"],\"project\":\"test-project\",\"include_tests\":true,"
        "\"include_routes\":true,\"max_related_files\":1,\"max_tests\":1}");
    ASSERT_NOT_NULL(raw);
    ASSERT_NULL(strstr(raw, "\"isError\":true"));

    char *text = extract_text_content(raw);
    ASSERT_NOT_NULL(text);
    ASSERT_NOT_NULL(strstr(text, "\"max_related_files\":1"));
    ASSERT_NOT_NULL(strstr(text, "\"max_tests\":1"));
    ASSERT_NOT_NULL(strstr(text, "\"test_command_hints\""));
    ASSERT_NOT_NULL(strstr(text, "\"relationship_types\""));
    ASSERT_NOT_NULL(strstr(text, "\"priority_score\""));
    free(text);
    free(raw);

    cbm_mcp_server_free(srv);
    cleanup_file_context_dir(tmp);
    PASS();
}

TEST(tool_get_change_risks_partial_coverage) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_file_context_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *raw = cbm_mcp_handle_tool(
        srv, "get_change_risks",
        "{\"paths\":[\"main.go\",\"missing.go\"],\"project\":\"test-project\",\"include_tests\":true,"
        "\"include_routes\":true}");
    ASSERT_NOT_NULL(raw);
    ASSERT_NULL(strstr(raw, "\"isError\":true"));

    char *text = extract_text_content(raw);
    ASSERT_NOT_NULL(text);
    ASSERT_NOT_NULL(strstr(text, "\"confidence\":\"medium\""));
    ASSERT_NOT_NULL(strstr(text, "\"indexed_targets\":[\"main.go\"]"));
    ASSERT_NOT_NULL(strstr(text, "\"missing_targets\":[\"missing.go\"]"));
    ASSERT_NOT_NULL(strstr(text, "partial_graph_coverage"));
    ASSERT_NOT_NULL(strstr(text, "Graph coverage is partial"));
    ASSERT_NOT_NULL(strstr(text, "Search code for missing targets"));
    free(text);
    free(raw);

    cbm_mcp_server_free(srv);
    cleanup_file_context_dir(tmp);
    PASS();
}

TEST(tool_get_edit_plan_missing_path) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);

    char *raw = cbm_mcp_handle_tool(srv, "get_edit_plan", "{}");
    ASSERT_NOT_NULL(raw);
    ASSERT_NOT_NULL(strstr(raw, "\"isError\":true"));

    char *text = extract_text_content(raw);
    ASSERT_NOT_NULL(text);
    ASSERT_NOT_NULL(strstr(text, "path is required"));
    free(text);
    free(raw);
    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_get_edit_plan_basic) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_file_context_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *raw = cbm_mcp_handle_tool(
        srv, "get_edit_plan",
        "{\"path\":\"main.go\",\"project\":\"test-project\",\"include_routes\":true,"
        "\"max_related_files\":4,\"max_tests\":3}");
    ASSERT_NOT_NULL(raw);
    ASSERT_NULL(strstr(raw, "\"isError\":true"));

    char *text = extract_text_content(raw);
    ASSERT_NOT_NULL(text);
    ASSERT_NOT_NULL(strstr(text, "\"tool_version\":\"1.0\""));
    ASSERT_NOT_NULL(strstr(text, "\"file_context\""));
    ASSERT_NOT_NULL(strstr(text, "\"change_risks\""));
    ASSERT_NOT_NULL(strstr(text, "\"immediate_steps\""));
    ASSERT_NOT_NULL(strstr(text, "\"path\":\"main.go\""));
    ASSERT_NOT_NULL(strstr(text, "Pre-edit plan for main.go."));
    ASSERT_NOT_NULL(strstr(text, "\"risk_level\""));
    ASSERT_NOT_NULL(strstr(text, "\"test_command_hints\""));
    ASSERT_NOT_NULL(strstr(text, "main_test.go"));
    ASSERT_NOT_NULL(strstr(text, "HandleRequest"));
    free(text);
    free(raw);

    cbm_mcp_server_free(srv);
    cleanup_file_context_dir(tmp);
    PASS();
}

TEST(tool_get_edit_plan_compact_mode) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_file_context_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *raw = cbm_mcp_handle_tool(
        srv, "get_edit_plan",
        "{\"path\":\"main.go\",\"project\":\"test-project\",\"mode\":\"compact\","
        "\"max_related_files\":3,\"max_tests\":2}");
    ASSERT_NOT_NULL(raw);
    ASSERT_NULL(strstr(raw, "\"isError\":true"));

    char *text = extract_text_content(raw);
    ASSERT_NOT_NULL(text);
    ASSERT_NOT_NULL(strstr(text, "\"mode\":\"compact\""));
    ASSERT_NOT_NULL(strstr(text, "\"top_related_files\""));
    ASSERT_NOT_NULL(strstr(text, "\"top_tests\""));
    ASSERT_NOT_NULL(strstr(text, "\"test_command_hints\""));

    yyjson_doc *doc = yyjson_read(text, strlen(text), 0);
    ASSERT_NOT_NULL(doc);
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *data = yyjson_obj_get(root, "data");
    ASSERT_NOT_NULL(data);
    yyjson_val *file_context = yyjson_obj_get(data, "file_context");
    yyjson_val *change_risks = yyjson_obj_get(data, "change_risks");
    ASSERT_NOT_NULL(file_context);
    ASSERT_NOT_NULL(change_risks);
    ASSERT_NULL(yyjson_obj_get(file_context, "data"));
    ASSERT_NULL(yyjson_obj_get(change_risks, "data"));
    yyjson_doc_free(doc);

    free(text);
    free(raw);

    cbm_mcp_server_free(srv);
    cleanup_file_context_dir(tmp);
    PASS();
}

TEST(tool_get_edit_plan_task_type_refactor) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_file_context_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *raw = cbm_mcp_handle_tool(
        srv, "get_edit_plan",
        "{\"path\":\"main.go\",\"project\":\"test-project\",\"task_type\":\"refactor\"}");
    ASSERT_NOT_NULL(raw);
    ASSERT_NULL(strstr(raw, "\"isError\":true"));

    char *text = extract_text_content(raw);
    ASSERT_NOT_NULL(text);
    ASSERT_NOT_NULL(strstr(text, "\"task_type\":\"refactor\""));
    ASSERT_NOT_NULL(strstr(text, "Preserve caller-visible contracts while restructuring internals."));
    ASSERT_NOT_NULL(strstr(text, "Run the highest-value tests plus broader regression coverage."));
    free(text);
    free(raw);

    cbm_mcp_server_free(srv);
    cleanup_file_context_dir(tmp);
    PASS();
}

TEST(tool_get_edit_plan_task_type_investigate) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_file_context_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *raw = cbm_mcp_handle_tool(
        srv, "get_edit_plan",
        "{\"path\":\"main.go\",\"project\":\"test-project\",\"task_type\":\"investigate\","
        "\"mode\":\"compact\"}");
    ASSERT_NOT_NULL(raw);
    ASSERT_NULL(strstr(raw, "\"isError\":true"));

    char *text = extract_text_content(raw);
    ASSERT_NOT_NULL(text);
    ASSERT_NOT_NULL(strstr(text, "\"task_type\":\"investigate\""));
    ASSERT_NOT_NULL(strstr(text, "avoid editing until the root cause is clear"));
    ASSERT_NOT_NULL(strstr(text, "confirming the blast radius before making code modifications"));
    free(text);
    free(raw);

    cbm_mcp_server_free(srv);
    cleanup_file_context_dir(tmp);
    PASS();
}

/* ── TestSnippet_ExactQN ──────────────────────────────────────── */

TEST(snippet_exact_qn) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *resp =
        call_snippet(srv, "{\"qualified_name\":\"test-project.cmd.server.main.HandleRequest\","
                          "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"name\":\"HandleRequest\""));
    ASSERT_NOT_NULL(strstr(resp, "\"source\""));
    /* Exact match should NOT have match_method */
    ASSERT_NULL(strstr(resp, "\"match_method\""));
    /* Enriched properties */
    ASSERT_NOT_NULL(strstr(resp, "\"signature\":\"func HandleRequest() error\""));
    ASSERT_NOT_NULL(strstr(resp, "\"return_type\":\"error\""));
    /* Caller/callee counts: 0 callers, 2 callees */
    ASSERT_NOT_NULL(strstr(resp, "\"callers\":0"));
    ASSERT_NOT_NULL(strstr(resp, "\"callees\":2"));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_QNSuffix ─────────────────────────────────────── */

TEST(snippet_qn_suffix) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *resp = call_snippet(srv, "{\"qualified_name\":\"main.HandleRequest\","
                                   "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"name\":\"HandleRequest\""));
    ASSERT_NOT_NULL(strstr(resp, "\"match_method\":\"suffix\""));
    ASSERT_NOT_NULL(strstr(resp, "\"source\""));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_UniqueShortName ──────────────────────────────── */

TEST(snippet_unique_short_name) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    /* "ProcessOrder" is unique — suffix tier matches (QN ends with .ProcessOrder) */
    char *resp = call_snippet(srv, "{\"qualified_name\":\"ProcessOrder\","
                                   "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"name\":\"ProcessOrder\""));
    ASSERT_NOT_NULL(strstr(resp, "\"match_method\":\"suffix\""));
    ASSERT_NOT_NULL(strstr(resp, "\"source\""));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_NameTier ─────────────────────────────────────── */

TEST(snippet_name_tier) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    /* "HandleRequest" — suffix tier finds it (QN ends with .HandleRequest) */
    char *resp = call_snippet(srv, "{\"qualified_name\":\"HandleRequest\","
                                   "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"name\":\"HandleRequest\""));
    ASSERT_NOT_NULL(strstr(resp, "\"match_method\":\"suffix\""));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_AmbiguousShortName ───────────────────────────── */

TEST(snippet_ambiguous_short_name) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    /* "Run" matches 2 nodes — should return suggestions */
    char *resp = call_snippet(srv, "{\"qualified_name\":\"Run\","
                                   "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"status\":\"ambiguous\""));
    ASSERT_NOT_NULL(strstr(resp, "\"message\""));
    ASSERT_NOT_NULL(strstr(resp, "\"suggestions\""));
    /* Must NOT have "error" key */
    ASSERT_NULL(strstr(resp, "\"error\""));
    /* Must NOT have "source" */
    ASSERT_NULL(strstr(resp, "\"source\""));
    /* Should have at least 2 suggestions with qualified_name */
    ASSERT_NOT_NULL(strstr(resp, "test-project.cmd.server.Run"));
    ASSERT_NOT_NULL(strstr(resp, "test-project.cmd.worker.Run"));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_NotFound ─────────────────────────────────────── */

TEST(snippet_not_found) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *resp = call_snippet(srv, "{\"qualified_name\":\"CompletelyNonexistentFunctionXYZ123\","
                                   "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    /* Should return error or suggestions */
    ASSERT_TRUE(strstr(resp, "not found") || strstr(resp, "suggestions"));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_FuzzySuggestions ─────────────────────────────── */

TEST(snippet_fuzzy_suggestions) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    /* "Handle" is not an exact QN or suffix — should get not-found guidance */
    char *resp = call_snippet(srv, "{\"qualified_name\":\"Handle\","
                                   "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    /* Should guide user to search_graph */
    ASSERT_NOT_NULL(strstr(resp, "search_graph"));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_EnrichedProperties ───────────────────────────── */

TEST(snippet_enriched_properties) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *resp =
        call_snippet(srv, "{\"qualified_name\":\"test-project.cmd.server.main.HandleRequest\","
                          "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"signature\""));
    ASSERT_NOT_NULL(strstr(resp, "\"return_type\""));
    ASSERT_NOT_NULL(strstr(resp, "\"is_exported\":true"));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_FuzzyLastSegment ─────────────────────────────── */

TEST(snippet_fuzzy_last_segment) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    /* "auth.handlers.HandleRequest" — suffix match should find HandleRequest */
    char *resp = call_snippet(srv, "{\"qualified_name\":\"auth.handlers.HandleRequest\","
                                   "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    /* Should either find it via suffix or guide to search_graph */
    ASSERT_TRUE(strstr(resp, "HandleRequest") != NULL || strstr(resp, "search_graph") != NULL);
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_AutoResolve_Default ──────────────────────────── */

TEST(snippet_auto_resolve_default) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    /* "Run" is ambiguous (2 candidates). Without auto_resolve → suggestions */
    char *resp = call_snippet(srv, "{\"qualified_name\":\"Run\","
                                   "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"status\":\"ambiguous\""));
    ASSERT_NULL(strstr(resp, "\"source\""));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_AutoResolve_Enabled ──────────────────────────── */

TEST(snippet_auto_resolve_enabled) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    /* "Run" — suffix match should find candidates or guide to search */
    char *resp = call_snippet(srv, "{\"qualified_name\":\"Run\","
                                   "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    /* "Run" matches multiple nodes via suffix → should get suggestions or source */
    ASSERT_TRUE(strstr(resp, "Run") != NULL);
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_IncludeNeighbors_Default ─────────────────────── */

TEST(snippet_include_neighbors_default) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *resp =
        call_snippet(srv, "{\"qualified_name\":\"test-project.cmd.server.main.HandleRequest\","
                          "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    /* Without include_neighbors → NO caller_names/callee_names */
    ASSERT_NULL(strstr(resp, "\"caller_names\""));
    ASSERT_NULL(strstr(resp, "\"callee_names\""));
    /* But should still have counts */
    ASSERT_NOT_NULL(strstr(resp, "\"callers\""));
    ASSERT_NOT_NULL(strstr(resp, "\"callees\""));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_IncludeNeighbors_Enabled ─────────────────────── */

TEST(snippet_include_neighbors_enabled) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *resp =
        call_snippet(srv, "{\"qualified_name\":\"test-project.cmd.server.main.HandleRequest\","
                          "\"include_neighbors\":true,\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"source\""));
    /* HandleRequest has 0 callers → no caller_names array */
    ASSERT_NULL(strstr(resp, "\"caller_names\""));
    /* HandleRequest has 2 callees: ProcessOrder and Run */
    ASSERT_NOT_NULL(strstr(resp, "\"callee_names\""));
    ASSERT_NOT_NULL(strstr(resp, "ProcessOrder"));
    ASSERT_NOT_NULL(strstr(resp, "Run"));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  SUITE
 * ══════════════════════════════════════════════════════════════════ */

SUITE(mcp) {
    /* JSON-RPC parsing */
    RUN_TEST(jsonrpc_parse_request);
    RUN_TEST(jsonrpc_parse_notification);
    RUN_TEST(jsonrpc_parse_invalid);
    RUN_TEST(jsonrpc_parse_tools_call);

    /* JSON-RPC formatting */
    RUN_TEST(jsonrpc_format_response);
    RUN_TEST(jsonrpc_format_error);

    /* MCP protocol helpers */
    RUN_TEST(mcp_initialize_response);
    RUN_TEST(mcp_tools_list);
    RUN_TEST(mcp_tools_array_schemas_have_items);
    RUN_TEST(mcp_text_result);
    RUN_TEST(mcp_text_result_error);

    /* Argument extraction */
    RUN_TEST(mcp_get_tool_name);
    RUN_TEST(mcp_get_arguments);
    RUN_TEST(mcp_get_string_arg);
    RUN_TEST(mcp_get_int_arg);
    RUN_TEST(mcp_get_bool_arg);

    /* Server protocol handling */
    RUN_TEST(server_handle_initialize);
    RUN_TEST(server_handle_initialized_notification);
    RUN_TEST(server_handle_tools_list);
    RUN_TEST(server_handle_unknown_method);

    /* Tool handlers */
    RUN_TEST(tool_list_projects_empty);
    RUN_TEST(tool_get_graph_schema_empty);
    RUN_TEST(tool_unknown_tool);
    RUN_TEST(tool_search_graph_basic);
    RUN_TEST(tool_query_graph_basic);
    RUN_TEST(tool_index_status_no_project);

    /* Tool handlers with validation */
    RUN_TEST(tool_trace_call_path_not_found);
    RUN_TEST(tool_trace_missing_function_name);
    RUN_TEST(tool_delete_project_not_found);
    RUN_TEST(tool_get_architecture_empty);
    RUN_TEST(tool_query_graph_missing_query);

    /* Pipeline-dependent tool handlers */
    RUN_TEST(tool_index_repository_missing_path);
    RUN_TEST(tool_get_file_context_missing_path);
    RUN_TEST(tool_get_file_context_basic);
    RUN_TEST(tool_get_file_context_options);
    RUN_TEST(tool_get_related_files_missing_path);
    RUN_TEST(tool_get_related_files_basic);
    RUN_TEST(tool_get_tests_missing_paths);
    RUN_TEST(tool_get_tests_basic);
    RUN_TEST(tool_get_callers_missing_target);
    RUN_TEST(tool_get_callers_symbol_basic);
    RUN_TEST(tool_get_change_risks_missing_target);
    RUN_TEST(tool_get_change_risks_basic);
    RUN_TEST(tool_get_change_risks_compact_options);
    RUN_TEST(tool_get_change_risks_partial_coverage);
    RUN_TEST(tool_get_edit_plan_missing_path);
    RUN_TEST(tool_get_edit_plan_basic);
    RUN_TEST(tool_get_edit_plan_compact_mode);
    RUN_TEST(tool_get_edit_plan_task_type_refactor);
    RUN_TEST(tool_get_edit_plan_task_type_investigate);
    RUN_TEST(tool_get_code_snippet_missing_qn);
    RUN_TEST(tool_get_code_snippet_not_found);
    RUN_TEST(tool_search_code_missing_pattern);
    RUN_TEST(tool_search_code_no_project);
    RUN_TEST(tool_detect_changes_no_project);
    RUN_TEST(tool_manage_adr_no_project);
    RUN_TEST(tool_ingest_traces_basic);
    RUN_TEST(tool_ingest_traces_empty);

    /* Idle store eviction */
    RUN_TEST(store_idle_eviction);
    RUN_TEST(store_idle_no_eviction_within_timeout);
    RUN_TEST(store_idle_evict_protects_initial_store);
    RUN_TEST(store_idle_evict_access_resets_timer);

    /* URI helpers */
    RUN_TEST(parse_file_uri_unix);
    RUN_TEST(parse_file_uri_windows);
    RUN_TEST(parse_file_uri_invalid);

    /* Snippet resolution (port of snippet_test.go) */
    RUN_TEST(snippet_exact_qn);
    RUN_TEST(snippet_qn_suffix);
    RUN_TEST(snippet_unique_short_name);
    RUN_TEST(snippet_name_tier);
    RUN_TEST(snippet_ambiguous_short_name);
    RUN_TEST(snippet_not_found);
    RUN_TEST(snippet_fuzzy_suggestions);
    RUN_TEST(snippet_enriched_properties);
    RUN_TEST(snippet_fuzzy_last_segment);
    RUN_TEST(snippet_auto_resolve_default);
    RUN_TEST(snippet_auto_resolve_enabled);
    RUN_TEST(snippet_include_neighbors_default);
    RUN_TEST(snippet_include_neighbors_enabled);
}

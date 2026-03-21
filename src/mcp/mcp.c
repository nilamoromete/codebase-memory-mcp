/*
 * mcp.c — MCP server: JSON-RPC 2.0 over stdio with graph tools.
 *
 * Uses yyjson for fast JSON parsing/building.
 * Single-threaded event loop: read line → parse → dispatch → respond.
 */

// operations

#include "mcp/mcp.h"
#include "store/store.h"
#include "cypher/cypher.h"
#include "pipeline/pipeline.h"
#include "pipeline/httplink.h"
#include "cli/cli.h"
#include "watcher/watcher.h"
#include "foundation/mem.h"
#include "foundation/platform.h"
#include "foundation/compat.h"
#include "foundation/compat_fs.h"
#include "foundation/compat_thread.h"
#include "foundation/log.h"
#include "foundation/str_util.h"

#ifdef _WIN32
#include <process.h> /* _getpid */
#else
#include <unistd.h>
#include <sys/unistd.h>
#include <sys/poll.h>
#include <poll.h>
#endif
#include <yyjson/yyjson.h>
#include <stdint.h> // int64_t
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* ── Constants ────────────────────────────────────────────────── */

/* Default snippet fallback line count */
#define SNIPPET_DEFAULT_LINES 50

/* Idle store eviction: close cached project store after this many seconds
 * of inactivity to free SQLite memory during idle periods. */
#define STORE_IDLE_TIMEOUT_S 60

/* Directory permissions: rwxr-xr-x */
#define ADR_DIR_PERMS 0755

/* JSON-RPC 2.0 standard error codes */
#define JSONRPC_PARSE_ERROR (-32700)
#define JSONRPC_METHOD_NOT_FOUND (-32601)

/* ── Helpers ────────────────────────────────────────────────────── */

static char *heap_strdup(const char *s) {
    if (!s) {
        return NULL;
    }
    size_t len = strlen(s);
    char *d = malloc(len + 1);
    if (d) {
        memcpy(d, s, len + 1);
    }
    return d;
}

/* Write yyjson_mut_doc to heap-allocated JSON string.
 * ALLOW_INVALID_UNICODE: some database strings may contain non-UTF-8 bytes
 * from older indexing runs — don't fail serialization over it. */
static char *yy_doc_to_str(yyjson_mut_doc *doc) {
    size_t len = 0;
    char *s = yyjson_mut_write(doc, YYJSON_WRITE_ALLOW_INVALID_UNICODE, &len);
    return s;
}

/* ══════════════════════════════════════════════════════════════════
 *  JSON-RPC PARSING
 * ══════════════════════════════════════════════════════════════════ */

int cbm_jsonrpc_parse(const char *line, cbm_jsonrpc_request_t *out) {
    memset(out, 0, sizeof(*out));
    out->id = -1;

    yyjson_doc *doc = yyjson_read(line, strlen(line), 0);
    if (!doc) {
        return -1;
    }

    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        return -1;
    }

    yyjson_val *v_jsonrpc = yyjson_obj_get(root, "jsonrpc");
    yyjson_val *v_method = yyjson_obj_get(root, "method");
    yyjson_val *v_id = yyjson_obj_get(root, "id");
    yyjson_val *v_params = yyjson_obj_get(root, "params");

    if (!v_method || !yyjson_is_str(v_method)) {
        yyjson_doc_free(doc);
        return -1;
    }

    out->jsonrpc =
        heap_strdup(v_jsonrpc && yyjson_is_str(v_jsonrpc) ? yyjson_get_str(v_jsonrpc) : "2.0");
    out->method = heap_strdup(yyjson_get_str(v_method));

    if (v_id) {
        out->has_id = true;
        if (yyjson_is_int(v_id)) {
            out->id = yyjson_get_int(v_id);
        } else if (yyjson_is_str(v_id)) {
            out->id = strtol(yyjson_get_str(v_id), NULL, 10);
        }
    }

    if (v_params) {
        out->params_raw = yyjson_val_write(v_params, 0, NULL);
    }

    yyjson_doc_free(doc);
    return 0;
}

void cbm_jsonrpc_request_free(cbm_jsonrpc_request_t *r) {
    if (!r) {
        return;
    }
    free((void *)r->jsonrpc);
    free((void *)r->method);
    free((void *)r->params_raw);
    memset(r, 0, sizeof(*r));
}

/* ══════════════════════════════════════════════════════════════════
 *  JSON-RPC FORMATTING
 * ══════════════════════════════════════════════════════════════════ */

char *cbm_jsonrpc_format_response(const cbm_jsonrpc_response_t *resp) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_str(doc, root, "jsonrpc", "2.0");
    yyjson_mut_obj_add_int(doc, root, "id", resp->id);

    if (resp->error_json) {
        /* Parse the error JSON and embed */
        yyjson_doc *err_doc = yyjson_read(resp->error_json, strlen(resp->error_json), 0);
        if (err_doc) {
            yyjson_mut_val *err_val = yyjson_val_mut_copy(doc, yyjson_doc_get_root(err_doc));
            yyjson_mut_obj_add_val(doc, root, "error", err_val);
            yyjson_doc_free(err_doc);
        }
    } else if (resp->result_json) {
        /* Parse the result JSON and embed */
        yyjson_doc *res_doc = yyjson_read(resp->result_json, strlen(resp->result_json), 0);
        if (res_doc) {
            yyjson_mut_val *res_val = yyjson_val_mut_copy(doc, yyjson_doc_get_root(res_doc));
            yyjson_mut_obj_add_val(doc, root, "result", res_val);
            yyjson_doc_free(res_doc);
        }
    }

    char *out = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    return out;
}

char *cbm_jsonrpc_format_error(int64_t id, int code, const char *message) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_str(doc, root, "jsonrpc", "2.0");
    yyjson_mut_obj_add_int(doc, root, "id", id);

    yyjson_mut_val *err = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_int(doc, err, "code", code);
    yyjson_mut_obj_add_str(doc, err, "message", message);
    yyjson_mut_obj_add_val(doc, root, "error", err);

    char *out = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    return out;
}

/* ══════════════════════════════════════════════════════════════════
 *  MCP PROTOCOL HELPERS
 * ══════════════════════════════════════════════════════════════════ */

char *cbm_mcp_text_result(const char *text, bool is_error) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_val *content = yyjson_mut_arr(doc);
    yyjson_mut_val *item = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, item, "type", "text");
    yyjson_mut_obj_add_str(doc, item, "text", text);
    yyjson_mut_arr_add_val(content, item);
    yyjson_mut_obj_add_val(doc, root, "content", content);

    if (is_error) {
        yyjson_mut_obj_add_bool(doc, root, "isError", true);
    }

    char *out = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    return out;
}

/* ── Tool definitions ─────────────────────────────────────────── */

typedef struct {
    const char *name;
    const char *description;
    const char *input_schema; /* JSON string */
} tool_def_t;

static const tool_def_t TOOLS[] = {
    {"index_repository", "Index a repository into the knowledge graph",
     "{\"type\":\"object\",\"properties\":{\"repo_path\":{\"type\":\"string\",\"description\":"
     "\"Path to the "
     "repository\"},\"mode\":{\"type\":\"string\",\"enum\":[\"full\",\"fast\"],\"default\":"
     "\"full\"}},\"required\":[\"repo_path\"]}"},

    {"search_graph",
     "Search the code knowledge graph for functions, classes, routes, and variables. Use INSTEAD "
     "OF grep/glob when finding code definitions, implementations, or relationships. Returns "
     "precise results in one call.",
     "{\"type\":\"object\",\"properties\":{\"project\":{\"type\":\"string\"},\"label\":{\"type\":"
     "\"string\"},\"name_pattern\":{\"type\":\"string\"},\"qn_pattern\":{\"type\":\"string\"},"
     "\"file_pattern\":{\"type\":\"string\"},\"relationship\":{\"type\":\"string\"},\"min_degree\":"
     "{\"type\":\"integer\"},\"max_degree\":{\"type\":\"integer\"},\"exclude_entry_points\":{"
     "\"type\":\"boolean\"},\"include_connected\":{\"type\":\"boolean\"},\"limit\":{\"type\":"
     "\"integer\",\"description\":\"Max results. Default: "
     "unlimited\"},\"offset\":{\"type\":\"integer\",\"default\":0}}}"},

    {"query_graph",
     "Execute a Cypher query against the knowledge graph for complex multi-hop patterns, "
     "aggregations, and cross-service analysis.",
     "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"Cypher "
     "query\"},\"project\":{\"type\":\"string\"},\"max_rows\":{\"type\":\"integer\","
     "\"description\":"
     "\"Optional row limit. Default: unlimited (100k ceiling)\"}},\"required\":[\"query\"]}"},

    {"trace_call_path",
     "Trace function call paths — who calls a function and what it calls. Use INSTEAD OF grep when "
     "finding callers, dependencies, or impact analysis.",
     "{\"type\":\"object\",\"properties\":{\"function_name\":{\"type\":\"string\"},\"project\":{"
     "\"type\":\"string\"},\"direction\":{\"type\":\"string\",\"enum\":[\"inbound\",\"outbound\","
     "\"both\"],\"default\":\"both\"},\"depth\":{\"type\":\"integer\",\"default\":3},\"edge_"
     "types\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}},\"required\":[\"function_"
     "name\"]}"},

    {"get_code_snippet",
     "Read source code for a function/class/symbol. IMPORTANT: First call search_graph to find the "
     "exact qualified_name, then pass it here. This is a read tool, not a search tool. Accepts "
     "full qualified_name (exact match) or short function name (returns suggestions if ambiguous).",
     "{\"type\":\"object\",\"properties\":{\"qualified_name\":{\"type\":\"string\",\"description\":"
     "\"Full qualified_name from search_graph, or short function name\"},\"project\":{"
     "\"type\":\"string\"},\"include_neighbors\":{"
     "\"type\":\"boolean\",\"default\":false}},\"required\":[\"qualified_name\"]}"},

    {"get_graph_schema", "Get the schema of the knowledge graph (node labels, edge types)",
     "{\"type\":\"object\",\"properties\":{\"project\":{\"type\":\"string\"}}}"},

    {"get_architecture",
     "Get high-level architecture overview — packages, services, dependencies, and project "
     "structure at a glance.",
     "{\"type\":\"object\",\"properties\":{\"project\":{\"type\":\"string\"},\"aspects\":{\"type\":"
     "\"array\",\"items\":{\"type\":\"string\"}}}}"},

    {"get_file_context",
     "Get compact pre-edit context for a file: symbols, callers, related files, tests, and basic "
     "risk guidance.",
     "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Repo-"
     "relative file path\"},\"project\":{\"type\":\"string\"},\"max_related_files\":{\"type\":"
     "\"integer\",\"default\":8},\"max_tests\":{\"type\":\"integer\",\"default\":6},"
     "\"include_symbols\":{\"type\":\"boolean\",\"default\":true},"
     "\"include_callers\":{\"type\":\"boolean\",\"default\":true},"
     "\"include_snippets\":{\"type\":\"boolean\",\"default\":false}},"
     "\"required\":[\"path\"]}"},

    {"get_related_files",
     "Get ranked files related to a repo-relative file path, with explicit relationship types and "
     "reasons.",
     "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Repo-"
     "relative file path\"},\"project\":{\"type\":\"string\"},\"limit\":{\"type\":\"integer\","
     "\"default\":10}},\"required\":[\"path\"]}"},

    {"get_tests",
     "Get recommended tests for one or more repo-relative file paths.",
     "{\"type\":\"object\",\"properties\":{\"paths\":{\"type\":\"array\",\"items\":{\"type\":"
     "\"string\"}},\"path\":{\"type\":\"string\"},\"project\":{\"type\":\"string\"},\"limit\":{"
     "\"type\":\"integer\",\"default\":8},\"include_reasons\":{\"type\":\"boolean\",\"default\":"
     "true}}}"},

    {"get_callers",
     "Get inbound callers for a symbol or repo-relative file path.",
     "{\"type\":\"object\",\"properties\":{\"symbol\":{\"type\":\"string\"},\"path\":{\"type\":"
     "\"string\"},\"project\":{\"type\":\"string\"},\"depth\":{\"type\":\"integer\",\"default\":2},"
     "\"include_file_aggregation\":{\"type\":\"boolean\",\"default\":true}}}"},

    {"get_change_risks",
     "Get a compact blast-radius and regression-risk summary for changed files.",
     "{\"type\":\"object\",\"properties\":{\"paths\":{\"type\":\"array\",\"items\":{\"type\":"
     "\"string\"}},\"path\":{\"type\":\"string\"},\"diff_mode\":{\"type\":\"string\",\"enum\":["
     "\"working_tree\"]},\"project\":{\"type\":\"string\"},\"include_tests\":{\"type\":\"boolean\","
     "\"default\":true},\"include_routes\":{\"type\":\"boolean\",\"default\":true},"
     "\"max_related_files\":{\"type\":\"integer\",\"default\":8},\"max_tests\":{\"type\":"
     "\"integer\",\"default\":6}}}"},

    {"get_edit_plan",
     "Get a compact pre-edit plan for a repo-relative file by composing file context and change risk "
     "signals into one response.",
     "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Repo-"
     "relative file path\"},\"project\":{\"type\":\"string\"},\"mode\":{\"type\":\"string\","
     "\"enum\":[\"compact\",\"detailed\"],\"default\":\"detailed\"},\"task_type\":{\"type\":"
     "\"string\",\"enum\":[\"fix\",\"refactor\",\"investigate\"],\"default\":\"fix\"},"
     "\"include_routes\":{\"type\":"
     "\"boolean\",\"default\":true},\"include_snippets\":{\"type\":\"boolean\",\"default\":false},"
     "\"max_related_files\":{\"type\":\"integer\",\"default\":6},\"max_tests\":{\"type\":"
     "\"integer\",\"default\":5}},\"required\":[\"path\"]}"},

    {"search_code",
     "Search source code content with text or regex patterns. Use for string literals, error "
     "messages, and config values that are not in the knowledge graph.",
     "{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\"},\"project\":{\"type\":"
     "\"string\"},\"file_pattern\":{\"type\":\"string\"},\"regex\":{\"type\":\"boolean\","
     "\"default\":false},\"limit\":{\"type\":\"integer\",\"description\":\"Max results. Default: "
     "unlimited\"}},\"required\":["
     "\"pattern\"]}"},

    {"list_projects", "List all indexed projects", "{\"type\":\"object\",\"properties\":{}}"},

    {"delete_project", "Delete a project from the index",
     "{\"type\":\"object\",\"properties\":{\"project_name\":{\"type\":\"string\"}},\"required\":["
     "\"project_name\"]}"},

    {"index_status", "Get the indexing status of a project",
     "{\"type\":\"object\",\"properties\":{\"project\":{\"type\":\"string\"}}}"},

    {"detect_changes", "Detect code changes and their impact",
     "{\"type\":\"object\",\"properties\":{\"project\":{\"type\":\"string\"},\"scope\":{\"type\":"
     "\"string\"},\"depth\":{\"type\":\"integer\",\"default\":2},\"base_branch\":{\"type\":"
     "\"string\",\"default\":\"main\"}}}"},

    {"manage_adr", "Create or update Architecture Decision Records",
     "{\"type\":\"object\",\"properties\":{\"project\":{\"type\":\"string\"},\"mode\":{\"type\":"
     "\"string\",\"enum\":[\"get\",\"update\",\"sections\"]},\"content\":{\"type\":\"string\"},"
     "\"sections\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}}}"},

    {"ingest_traces", "Ingest runtime traces to enhance the knowledge graph",
     "{\"type\":\"object\",\"properties\":{\"traces\":{\"type\":\"array\",\"items\":{\"type\":"
     "\"object\"}},\"project\":{\"type\":"
     "\"string\"}},\"required\":[\"traces\"]}"},
};

static const int TOOL_COUNT = sizeof(TOOLS) / sizeof(TOOLS[0]);

char *cbm_mcp_tools_list(void) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_val *tools = yyjson_mut_arr(doc);

    for (int i = 0; i < TOOL_COUNT; i++) {
        yyjson_mut_val *tool = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, tool, "name", TOOLS[i].name);
        yyjson_mut_obj_add_str(doc, tool, "description", TOOLS[i].description);

        /* Parse input schema JSON and embed */
        yyjson_doc *schema_doc =
            yyjson_read(TOOLS[i].input_schema, strlen(TOOLS[i].input_schema), 0);
        if (schema_doc) {
            yyjson_mut_val *schema = yyjson_val_mut_copy(doc, yyjson_doc_get_root(schema_doc));
            yyjson_mut_obj_add_val(doc, tool, "inputSchema", schema);
            yyjson_doc_free(schema_doc);
        }

        yyjson_mut_arr_add_val(tools, tool);
    }

    yyjson_mut_obj_add_val(doc, root, "tools", tools);

    char *out = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    return out;
}

/* Supported protocol versions, newest first. The server picks the newest
 * version that it shares with the client (per MCP spec version negotiation). */
static const char *SUPPORTED_PROTOCOL_VERSIONS[] = {
    "2025-11-25",
    "2025-06-18",
    "2025-03-26",
    "2024-11-05",
};
static const int SUPPORTED_VERSION_COUNT =
    (int)(sizeof(SUPPORTED_PROTOCOL_VERSIONS) / sizeof(SUPPORTED_PROTOCOL_VERSIONS[0]));

char *cbm_mcp_initialize_response(const char *params_json) {
    /* Determine protocol version: if client requests a version we support,
     * echo it back; otherwise respond with our latest. */
    const char *version = SUPPORTED_PROTOCOL_VERSIONS[0]; /* default: latest */
    if (params_json) {
        yyjson_doc *pdoc = yyjson_read(params_json, strlen(params_json), 0);
        if (pdoc) {
            yyjson_val *pv = yyjson_obj_get(yyjson_doc_get_root(pdoc), "protocolVersion");
            if (pv && yyjson_is_str(pv)) {
                const char *requested = yyjson_get_str(pv);
                for (int i = 0; i < SUPPORTED_VERSION_COUNT; i++) {
                    if (strcmp(requested, SUPPORTED_PROTOCOL_VERSIONS[i]) == 0) {
                        version = SUPPORTED_PROTOCOL_VERSIONS[i];
                        break;
                    }
                }
            }
            yyjson_doc_free(pdoc);
        }
    }

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_str(doc, root, "protocolVersion", version);

    yyjson_mut_val *impl = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, impl, "name", "codebase-memory-mcp");
    yyjson_mut_obj_add_str(doc, impl, "version", "0.10.0");
    yyjson_mut_obj_add_val(doc, root, "serverInfo", impl);

    yyjson_mut_val *caps = yyjson_mut_obj(doc);
    yyjson_mut_val *tools_cap = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, caps, "tools", tools_cap);
    yyjson_mut_obj_add_val(doc, root, "capabilities", caps);

    char *out = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    return out;
}

/* ══════════════════════════════════════════════════════════════════
 *  ARGUMENT EXTRACTION
 * ══════════════════════════════════════════════════════════════════ */

char *cbm_mcp_get_tool_name(const char *params_json) {
    yyjson_doc *doc = yyjson_read(params_json, strlen(params_json), 0);
    if (!doc) {
        return NULL;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *name = yyjson_obj_get(root, "name");
    char *result = NULL;
    if (name && yyjson_is_str(name)) {
        result = heap_strdup(yyjson_get_str(name));
    }
    yyjson_doc_free(doc);
    return result;
}

char *cbm_mcp_get_arguments(const char *params_json) {
    yyjson_doc *doc = yyjson_read(params_json, strlen(params_json), 0);
    if (!doc) {
        return NULL;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *args = yyjson_obj_get(root, "arguments");
    char *result = NULL;
    if (args) {
        result = yyjson_val_write(args, 0, NULL);
    }
    yyjson_doc_free(doc);
    return result ? result : heap_strdup("{}");
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
char *cbm_mcp_get_string_arg(const char *args_json, const char *key) {
    yyjson_doc *doc = yyjson_read(args_json, strlen(args_json), 0);
    if (!doc) {
        return NULL;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *val = yyjson_obj_get(root, key);
    char *result = NULL;
    if (val && yyjson_is_str(val)) {
        result = heap_strdup(yyjson_get_str(val));
    }
    yyjson_doc_free(doc);
    return result;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
int cbm_mcp_get_int_arg(const char *args_json, const char *key, int default_val) {
    yyjson_doc *doc = yyjson_read(args_json, strlen(args_json), 0);
    if (!doc) {
        return default_val;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *val = yyjson_obj_get(root, key);
    int result = default_val;
    if (val && yyjson_is_int(val)) {
        result = yyjson_get_int(val);
    }
    yyjson_doc_free(doc);
    return result;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool cbm_mcp_get_bool_arg(const char *args_json, const char *key) {
    yyjson_doc *doc = yyjson_read(args_json, strlen(args_json), 0);
    if (!doc) {
        return false;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *val = yyjson_obj_get(root, key);
    bool result = false;
    if (val && yyjson_is_bool(val)) {
        result = yyjson_get_bool(val);
    }
    yyjson_doc_free(doc);
    return result;
}

char **cbm_mcp_get_string_array_arg(const char *args_json, const char *key, int *out_count) {
    if (out_count) {
        *out_count = 0;
    }

    yyjson_doc *doc = yyjson_read(args_json, strlen(args_json), 0);
    if (!doc) {
        return NULL;
    }

    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *val = yyjson_obj_get(root, key);
    if (!val || !yyjson_is_arr(val)) {
        yyjson_doc_free(doc);
        return NULL;
    }

    size_t count = yyjson_arr_size(val);
    if (count == 0) {
        yyjson_doc_free(doc);
        return NULL;
    }

    char **items = calloc(count, sizeof(char *));
    if (!items) {
        yyjson_doc_free(doc);
        return NULL;
    }

    size_t actual = 0;
    yyjson_val *item = NULL;
    yyjson_arr_iter iter = yyjson_arr_iter_with(val);
    while ((item = yyjson_arr_iter_next(&iter)) != NULL) {
        if (yyjson_is_str(item)) {
            items[actual++] = heap_strdup(yyjson_get_str(item));
        }
    }

    yyjson_doc_free(doc);

    if (actual == 0) {
        free(items);
        return NULL;
    }

    if (out_count) {
        *out_count = (int)actual;
    }
    return items;
}

static void free_string_array(char **items, int count) {
    if (!items) {
        return;
    }
    for (int i = 0; i < count; i++) {
        free(items[i]);
    }
    free(items);
}

/* ══════════════════════════════════════════════════════════════════
 *  MCP SERVER
 * ══════════════════════════════════════════════════════════════════ */

struct cbm_mcp_server {
    cbm_store_t *store;        /* currently open project store (or NULL) */
    bool owns_store;           /* true if we opened the store */
    char *current_project;     /* which project store is open for (heap) */
    time_t store_last_used;    /* last time resolve_store was called for a named project */
    char update_notice[256];   /* one-shot update notice, cleared after first injection */
    bool update_checked;       /* true after background check has been launched */
    cbm_thread_t update_tid;   /* background update check thread */
    bool update_thread_active; /* true if update thread was started and needs joining */

    /* Session + auto-index state */
    char session_root[1024];     /* detected project root path */
    char session_project[256];   /* derived project name */
    bool session_detected;       /* true after first detection attempt */
    struct cbm_watcher *watcher; /* external watcher ref (not owned) */
    struct cbm_config *config;   /* external config ref (not owned) */
    cbm_thread_t autoindex_tid;
    bool autoindex_active; /* true if auto-index thread was started */
};

cbm_mcp_server_t *cbm_mcp_server_new(const char *store_path) {
    cbm_mcp_server_t *srv = calloc(1, sizeof(*srv));
    if (!srv) {
        return NULL;
    }

    /* If a store_path is given, open that project directly.
     * Otherwise, create an in-memory store for test/embedded use. */
    if (store_path) {
        srv->store = cbm_store_open(store_path);
        srv->current_project = heap_strdup(store_path);
    } else {
        srv->store = cbm_store_open_memory();
    }
    srv->owns_store = true;

    return srv;
}

cbm_store_t *cbm_mcp_server_store(cbm_mcp_server_t *srv) {
    return srv ? srv->store : NULL;
}

void cbm_mcp_server_set_project(cbm_mcp_server_t *srv, const char *project) {
    if (!srv) {
        return;
    }
    free(srv->current_project);
    srv->current_project = project ? heap_strdup(project) : NULL;
}

void cbm_mcp_server_set_watcher(cbm_mcp_server_t *srv, struct cbm_watcher *w) {
    if (srv) {
        srv->watcher = w;
    }
}

void cbm_mcp_server_set_config(cbm_mcp_server_t *srv, struct cbm_config *cfg) {
    if (srv) {
        srv->config = cfg;
    }
}

void cbm_mcp_server_free(cbm_mcp_server_t *srv) {
    if (!srv) {
        return;
    }
    if (srv->update_thread_active) {
        cbm_thread_join(&srv->update_tid);
    }
    if (srv->autoindex_active) {
        cbm_thread_join(&srv->autoindex_tid);
    }
    if (srv->owns_store && srv->store) {
        cbm_store_close(srv->store);
    }
    free(srv->current_project);
    free(srv);
}

/* ── Idle store eviction ──────────────────────────────────────── */

void cbm_mcp_server_evict_idle(cbm_mcp_server_t *srv, int timeout_s) {
    if (!srv || !srv->store) {
        return;
    }
    /* Protect initial in-memory stores that were never accessed via a named project.
     * store_last_used stays 0 until resolve_store is called with a non-NULL project. */
    if (srv->store_last_used == 0) {
        return;
    }

    time_t now = time(NULL);
    if ((now - srv->store_last_used) < timeout_s) {
        return;
    }

    if (srv->owns_store) {
        cbm_store_close(srv->store);
    }
    srv->store = NULL;
    free(srv->current_project);
    srv->current_project = NULL;
    srv->store_last_used = 0;
}

bool cbm_mcp_server_has_cached_store(cbm_mcp_server_t *srv) {
    return (srv && srv->store != NULL) != 0;
}

/* ── Cache dir + project DB path helpers ───────────────────────── */

/* Returns the platform cache directory: ~/.cache/codebase-memory-mcp
 * Writes to buf, returns buf for convenience. */
static const char *cache_dir(char *buf, size_t bufsz) {
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    const char *home = getenv("HOME");
    if (!home) {
        home = "/tmp";
    }
    snprintf(buf, bufsz, "%s/.cache/codebase-memory-mcp", home);
    return buf;
}

/* Returns full .db path for a project: <cache_dir>/<project>.db */
static const char *project_db_path(const char *project, char *buf, size_t bufsz) {
    char dir[1024];
    cache_dir(dir, sizeof(dir));
    snprintf(buf, bufsz, "%s/%s.db", dir, project);
    return buf;
}

/* ── Store resolution ──────────────────────────────────────────── */

/* Open the right project's .db file for query tools.
 * Caches the connection — reopens only when project changes.
 * Tracks last-access time so the event loop can evict idle stores. */
static cbm_store_t *resolve_store(cbm_mcp_server_t *srv, const char *project) {
    if (!project) {
        return srv->store; /* no project specified → use whatever's open */
    }

    srv->store_last_used = time(NULL);

    /* Already open for this project? */
    if (srv->current_project && strcmp(srv->current_project, project) == 0 && srv->store) {
        return srv->store;
    }

    /* Close old store */
    if (srv->owns_store && srv->store) {
        cbm_store_close(srv->store);
        srv->store = NULL;
    }

    /* Open project's .db file */
    char path[1024];
    project_db_path(project, path, sizeof(path));
    srv->store = cbm_store_open_path(path);
    srv->owns_store = true;
    free(srv->current_project);
    srv->current_project = heap_strdup(project);

    return srv->store;
}

/* Bail with empty JSON result when no store is available. */
#define REQUIRE_STORE(store, project)                                              \
    do {                                                                           \
        if (!(store)) {                                                            \
            free(project);                                                         \
            return cbm_mcp_text_result("{\"error\":\"no project loaded\"}", true); \
        }                                                                          \
    } while (0)

static void free_node_contents(cbm_node_t *n);
static char *get_project_root(cbm_mcp_server_t *srv, const char *project);
static void free_string_array(char **items, int count);
static void copy_node(const cbm_node_t *src, cbm_node_t *dst);

typedef struct {
    char *path;
    int callers;
    int callees;
    int imports;
    int tests;
    int cochanges;
    int score;
} file_context_related_t;

typedef struct {
    char *name;
    char *qualified_name;
    char *path;
    int score;
} file_context_caller_t;

typedef struct {
    char *path;
    int direct_tests;
    int file_tests;
    int score;
} file_context_test_t;

typedef struct {
    char *path;
    int direct_tests;
    int file_tests;
    int score;
    char **covers;
    int cover_count;
} test_recommendation_t;

typedef struct {
    char *name;
    char *qualified_name;
    char *path;
    int depth;
    int score;
} caller_result_t;

typedef struct {
    char *path;
    int caller_count;
    int score;
} caller_file_agg_t;

typedef struct {
    char *qualified_name;
    char *path;
    int score;
} symbol_match_t;

typedef struct {
    char *method;
    char *path;
    int score;
} route_risk_t;

static bool is_meta_label(const char *label) {
    if (!label) {
        return false;
    }
    return strcmp(label, "File") == 0 || strcmp(label, "Folder") == 0 ||
           strcmp(label, "Module") == 0 || strcmp(label, "Package") == 0;
}

static void free_related_files(file_context_related_t *items, int count) {
    if (!items) {
        return;
    }
    for (int i = 0; i < count; i++) {
        free(items[i].path);
    }
    free(items);
}

static void free_callers(file_context_caller_t *items, int count) {
    if (!items) {
        return;
    }
    for (int i = 0; i < count; i++) {
        free(items[i].name);
        free(items[i].qualified_name);
        free(items[i].path);
    }
    free(items);
}

static void free_tests(file_context_test_t *items, int count) {
    if (!items) {
        return;
    }
    for (int i = 0; i < count; i++) {
        free(items[i].path);
    }
    free(items);
}

static void free_test_recommendations(test_recommendation_t *items, int count) {
    if (!items) {
        return;
    }
    for (int i = 0; i < count; i++) {
        free(items[i].path);
        free_string_array(items[i].covers, items[i].cover_count);
    }
    free(items);
}

static void free_caller_results(caller_result_t *items, int count) {
    if (!items) {
        return;
    }
    for (int i = 0; i < count; i++) {
        free(items[i].name);
        free(items[i].qualified_name);
        free(items[i].path);
    }
    free(items);
}

static void free_caller_file_aggs(caller_file_agg_t *items, int count) {
    if (!items) {
        return;
    }
    for (int i = 0; i < count; i++) {
        free(items[i].path);
    }
    free(items);
}

static void free_symbol_matches(symbol_match_t *items, int count) {
    if (!items) {
        return;
    }
    for (int i = 0; i < count; i++) {
        free(items[i].qualified_name);
        free(items[i].path);
    }
    free(items);
}

static void free_route_risks(route_risk_t *items, int count) {
    if (!items) {
        return;
    }
    for (int i = 0; i < count; i++) {
        free(items[i].method);
        free(items[i].path);
    }
    free(items);
}

static void add_related_file(file_context_related_t **items, int *count, const char *path,
                             const char *current_path, int callers, int callees, int imports,
                             int tests, int cochanges) {
    if (!path || !path[0]) {
        return;
    }
    if (current_path && strcmp(path, current_path) == 0) {
        return;
    }

    for (int i = 0; i < *count; i++) {
        if (strcmp((*items)[i].path, path) == 0) {
            (*items)[i].callers += callers;
            (*items)[i].callees += callees;
            (*items)[i].imports += imports;
            (*items)[i].tests += tests;
            (*items)[i].cochanges += cochanges;
            (*items)[i].score += callers * 5 + callees * 4 + imports * 3 + tests * 4 + cochanges * 2;
            return;
        }
    }

    file_context_related_t *next = safe_realloc(*items, (size_t)(*count + 1) * sizeof(**items));
    *items = next;
    memset(&(*items)[*count], 0, sizeof(**items));
    (*items)[*count].path = heap_strdup(path);
    (*items)[*count].callers = callers;
    (*items)[*count].callees = callees;
    (*items)[*count].imports = imports;
    (*items)[*count].tests = tests;
    (*items)[*count].cochanges = cochanges;
    (*items)[*count].score = callers * 5 + callees * 4 + imports * 3 + tests * 4 + cochanges * 2;
    (*count)++;
}

static void add_caller(file_context_caller_t **items, int *count, const cbm_node_t *node) {
    if (!node || !node->qualified_name || !node->qualified_name[0]) {
        return;
    }

    for (int i = 0; i < *count; i++) {
        if (strcmp((*items)[i].qualified_name, node->qualified_name) == 0) {
            (*items)[i].score++;
            return;
        }
    }

    file_context_caller_t *next = safe_realloc(*items, (size_t)(*count + 1) * sizeof(**items));
    *items = next;
    memset(&(*items)[*count], 0, sizeof(**items));
    (*items)[*count].name = heap_strdup(node->name ? node->name : "");
    (*items)[*count].qualified_name = heap_strdup(node->qualified_name ? node->qualified_name : "");
    (*items)[*count].path = heap_strdup(node->file_path ? node->file_path : "");
    (*items)[*count].score = 1;
    (*count)++;
}

static void add_test_file(file_context_test_t **items, int *count, const char *path, int direct_tests,
                          int file_tests) {
    if (!path || !path[0]) {
        return;
    }

    for (int i = 0; i < *count; i++) {
        if (strcmp((*items)[i].path, path) == 0) {
            (*items)[i].direct_tests += direct_tests;
            (*items)[i].file_tests += file_tests;
            (*items)[i].score += direct_tests * 5 + file_tests * 4;
            return;
        }
    }

    file_context_test_t *next = safe_realloc(*items, (size_t)(*count + 1) * sizeof(**items));
    *items = next;
    memset(&(*items)[*count], 0, sizeof(**items));
    (*items)[*count].path = heap_strdup(path);
    (*items)[*count].direct_tests = direct_tests;
    (*items)[*count].file_tests = file_tests;
    (*items)[*count].score = direct_tests * 5 + file_tests * 4;
    (*count)++;
}

static void add_test_cover(char ***covers, int *cover_count, const char *covered_path) {
    if (!covered_path || !covered_path[0]) {
        return;
    }

    for (int i = 0; i < *cover_count; i++) {
        if (strcmp((*covers)[i], covered_path) == 0) {
            return;
        }
    }

    char **next = safe_realloc(*covers, (size_t)(*cover_count + 1) * sizeof(**covers));
    *covers = next;
    (*covers)[*cover_count] = heap_strdup(covered_path);
    (*cover_count)++;
}

static void add_test_recommendation(test_recommendation_t **items, int *count, const char *path,
                                    const char *covered_path, int direct_tests, int file_tests) {
    if (!path || !path[0]) {
        return;
    }

    for (int i = 0; i < *count; i++) {
        if (strcmp((*items)[i].path, path) == 0) {
            (*items)[i].direct_tests += direct_tests;
            (*items)[i].file_tests += file_tests;
            (*items)[i].score += direct_tests * 5 + file_tests * 4;
            add_test_cover(&(*items)[i].covers, &(*items)[i].cover_count, covered_path);
            return;
        }
    }

    test_recommendation_t *next = safe_realloc(*items, (size_t)(*count + 1) * sizeof(**items));
    *items = next;
    memset(&(*items)[*count], 0, sizeof(**items));
    (*items)[*count].path = heap_strdup(path);
    (*items)[*count].direct_tests = direct_tests;
    (*items)[*count].file_tests = file_tests;
    (*items)[*count].score = direct_tests * 5 + file_tests * 4;
    add_test_cover(&(*items)[*count].covers, &(*items)[*count].cover_count, covered_path);
    (*count)++;
}

static void add_caller_result(caller_result_t **items, int *count, const cbm_node_t *node, int depth) {
    if (!node || !node->qualified_name || !node->qualified_name[0]) {
        return;
    }

    for (int i = 0; i < *count; i++) {
        if (strcmp((*items)[i].qualified_name, node->qualified_name) == 0) {
            if (depth < (*items)[i].depth) {
                (*items)[i].depth = depth;
            }
            (*items)[i].score++;
            return;
        }
    }

    caller_result_t *next = safe_realloc(*items, (size_t)(*count + 1) * sizeof(**items));
    *items = next;
    memset(&(*items)[*count], 0, sizeof(**items));
    (*items)[*count].name = heap_strdup(node->name ? node->name : "");
    (*items)[*count].qualified_name = heap_strdup(node->qualified_name ? node->qualified_name : "");
    (*items)[*count].path = heap_strdup(node->file_path ? node->file_path : "");
    (*items)[*count].depth = depth;
    (*items)[*count].score = depth > 0 ? (6 - depth) : 1;
    (*count)++;
}

static void add_caller_file_agg(caller_file_agg_t **items, int *count, const char *path, int score) {
    if (!path || !path[0]) {
        return;
    }

    for (int i = 0; i < *count; i++) {
        if (strcmp((*items)[i].path, path) == 0) {
            (*items)[i].caller_count++;
            (*items)[i].score += score;
            return;
        }
    }

    caller_file_agg_t *next = safe_realloc(*items, (size_t)(*count + 1) * sizeof(**items));
    *items = next;
    memset(&(*items)[*count], 0, sizeof(**items));
    (*items)[*count].path = heap_strdup(path);
    (*items)[*count].caller_count = 1;
    (*items)[*count].score = score;
    (*count)++;
}

static void add_symbol_match(symbol_match_t **items, int *count, const cbm_node_t *node, int score) {
    if (!node || !node->qualified_name || !node->qualified_name[0]) {
        return;
    }

    for (int i = 0; i < *count; i++) {
        if (strcmp((*items)[i].qualified_name, node->qualified_name) == 0) {
            if (score > (*items)[i].score) {
                (*items)[i].score = score;
            }
            return;
        }
    }

    symbol_match_t *next = safe_realloc(*items, (size_t)(*count + 1) * sizeof(**items));
    *items = next;
    memset(&(*items)[*count], 0, sizeof(**items));
    (*items)[*count].qualified_name = heap_strdup(node->qualified_name ? node->qualified_name : "");
    (*items)[*count].path = heap_strdup(node->file_path ? node->file_path : "");
    (*items)[*count].score = score;
    (*count)++;
}

static void add_route_risk(route_risk_t **items, int *count, const cbm_node_t *node, int score) {
    const char *name = node && node->name ? node->name : "";
    const char *space = strchr(name, ' ');
    const char *method_src = "";
    const char *path_src = name;
    char method_buf[32] = {0};

    if (space) {
        size_t method_len = (size_t)(space - name);
        if (method_len >= sizeof(method_buf)) {
            method_len = sizeof(method_buf) - 1;
        }
        memcpy(method_buf, name, method_len);
        method_buf[method_len] = '\0';
        method_src = method_buf;
        path_src = space + 1;
    }

    if (!path_src || !*path_src) {
        path_src = node && node->file_path ? node->file_path : "";
    }

    for (int i = 0; i < *count; i++) {
        if (strcmp((*items)[i].method, method_src) == 0 && strcmp((*items)[i].path, path_src) == 0) {
            if (score > (*items)[i].score) {
                (*items)[i].score = score;
            }
            return;
        }
    }

    route_risk_t *next = safe_realloc(*items, (size_t)(*count + 1) * sizeof(**items));
    *items = next;
    memset(&(*items)[*count], 0, sizeof(**items));
    (*items)[*count].method = heap_strdup(method_src);
    (*items)[*count].path = heap_strdup(path_src);
    (*items)[*count].score = score;
    (*count)++;
}

static int compare_related_files_desc(const void *a, const void *b) {
    const file_context_related_t *left = a;
    const file_context_related_t *right = b;
    int left_priority = left->callers * 7 + left->tests * 6 + left->callees * 4 + left->imports * 3 +
                        left->cochanges;
    int right_priority = right->callers * 7 + right->tests * 6 + right->callees * 4 +
                         right->imports * 3 + right->cochanges;
    if (left_priority != right_priority) {
        return right_priority - left_priority;
    }
    if (left->score != right->score) {
        return right->score - left->score;
    }
    return strcmp(left->path, right->path);
}

static int compare_callers_desc(const void *a, const void *b) {
    const file_context_caller_t *left = a;
    const file_context_caller_t *right = b;
    if (left->score != right->score) {
        return right->score - left->score;
    }
    return strcmp(left->qualified_name, right->qualified_name);
}

static int compare_tests_desc(const void *a, const void *b) {
    const file_context_test_t *left = a;
    const file_context_test_t *right = b;
    if (left->score != right->score) {
        return right->score - left->score;
    }
    return strcmp(left->path, right->path);
}

static int compare_test_recommendations_desc(const void *a, const void *b) {
    const test_recommendation_t *left = a;
    const test_recommendation_t *right = b;
    if (left->score != right->score) {
        return right->score - left->score;
    }
    return strcmp(left->path, right->path);
}

static int compare_caller_results_desc(const void *a, const void *b) {
    const caller_result_t *left = a;
    const caller_result_t *right = b;
    if (left->depth != right->depth) {
        return left->depth - right->depth;
    }
    if (left->score != right->score) {
        return right->score - left->score;
    }
    return strcmp(left->qualified_name, right->qualified_name);
}

static int compare_caller_file_aggs_desc(const void *a, const void *b) {
    const caller_file_agg_t *left = a;
    const caller_file_agg_t *right = b;
    if (left->score != right->score) {
        return right->score - left->score;
    }
    return strcmp(left->path, right->path);
}

static int compare_symbol_matches_desc(const void *a, const void *b) {
    const symbol_match_t *left = a;
    const symbol_match_t *right = b;
    if (left->score != right->score) {
        return right->score - left->score;
    }
    return strcmp(left->qualified_name, right->qualified_name);
}

static int compare_route_risks_desc(const void *a, const void *b) {
    const route_risk_t *left = a;
    const route_risk_t *right = b;
    if (left->score != right->score) {
        return right->score - left->score;
    }
    return strcmp(left->path, right->path);
}

static double normalize_score(int value, int ceiling) {
    if (value <= 0 || ceiling <= 0) {
        return 0.0;
    }
    if (value >= ceiling) {
        return 1.0;
    }
    return (double)value / (double)ceiling;
}

static const char *risk_level_for_context(int inbound_calls, int related_count, int tests_count) {
    if (inbound_calls >= 5 || related_count >= 8) {
        return "high";
    }
    if (inbound_calls >= 1 || related_count >= 3 || tests_count >= 1) {
        return "medium";
    }
    return "low";
}

static const char *confidence_for_context(bool has_file_node, int symbol_count) {
    if (has_file_node && symbol_count > 0) {
        return "high";
    }
    if (has_file_node || symbol_count > 0) {
        return "medium";
    }
    return "low";
}

static char **collect_working_tree_paths(const char *root_path, int *out_count) {
    *out_count = 0;
    if (!root_path || !cbm_validate_shell_arg(root_path)) {
        return NULL;
    }

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "cd '%s' && { git diff --name-only --cached 2>/dev/null; git diff --name-only "
             "2>/dev/null; } | sort -u",
             root_path);

    // NOLINTNEXTLINE(bugprone-command-processor,cert-env33-c)
    FILE *fp = cbm_popen(cmd, "r");
    if (!fp) {
        return NULL;
    }

    char **paths = NULL;
    int count = 0;
    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0) {
            continue;
        }

        char **next = safe_realloc(paths, (size_t)(count + 1) * sizeof(*next));
        paths = next;
        paths[count++] = heap_strdup(line);
    }

    cbm_pclose(fp);
    if (count == 0) {
        paths = calloc(1, sizeof(*paths));
    }
    *out_count = count;
    return paths;
}

static bool str_ends_with(const char *value, const char *suffix) {
    if (!value || !suffix) {
        return false;
    }
    size_t value_len = strlen(value);
    size_t suffix_len = strlen(suffix);
    if (suffix_len > value_len) {
        return false;
    }
    return strcmp(value + value_len - suffix_len, suffix) == 0;
}

static const char *detect_test_tier(const char *path) {
    if (!path) {
        return "unknown";
    }
    if (strstr(path, "e2e") || strstr(path, "playwright") || strstr(path, "cypress")) {
        return "e2e";
    }
    if (strstr(path, "integration")) {
        return "integration";
    }
    return "unit";
}

static const char *detect_language_for_path(const char *path) {
    if (!path) {
        return "unknown";
    }
    if (str_ends_with(path, ".go")) {
        return "go";
    }
    if (str_ends_with(path, ".py")) {
        return "python";
    }
    if (str_ends_with(path, ".ts")) {
        return "typescript";
    }
    if (str_ends_with(path, ".tsx")) {
        return "tsx";
    }
    if (str_ends_with(path, ".js")) {
        return "javascript";
    }
    if (str_ends_with(path, ".jsx")) {
        return "jsx";
    }
    if (str_ends_with(path, ".java")) {
        return "java";
    }
    if (str_ends_with(path, ".kt")) {
        return "kotlin";
    }
    if (str_ends_with(path, ".php")) {
        return "php";
    }
    if (str_ends_with(path, ".rb")) {
        return "ruby";
    }
    if (str_ends_with(path, ".rs")) {
        return "rust";
    }
    if (str_ends_with(path, ".c")) {
        return "c";
    }
    if (str_ends_with(path, ".h")) {
        return "c-header";
    }
    if (str_ends_with(path, ".cpp") || str_ends_with(path, ".cc") || str_ends_with(path, ".cxx")) {
        return "cpp";
    }
    return "unknown";
}

static void append_related_file_relationship_types(yyjson_mut_doc *doc, yyjson_mut_val *target,
                                                   const file_context_related_t *related) {
    yyjson_mut_val *types = yyjson_mut_arr(doc);
    if (related->callers > 0) {
        yyjson_mut_arr_add_str(doc, types, "CALLS_IN");
    }
    if (related->callees > 0) {
        yyjson_mut_arr_add_str(doc, types, "CALLS_OUT");
    }
    if (related->imports > 0) {
        yyjson_mut_arr_add_str(doc, types, "IMPORTS");
    }
    if (related->tests > 0) {
        yyjson_mut_arr_add_str(doc, types, "TESTS");
    }
    if (related->cochanges > 0) {
        yyjson_mut_arr_add_str(doc, types, "FILE_CHANGES_WITH");
    }
    yyjson_mut_obj_add_val(doc, target, "relationship_types", types);
}

static void append_related_file_reason(yyjson_mut_doc *doc, yyjson_mut_val *target,
                                       const file_context_related_t *related) {
    if (related->callers > 0) {
        yyjson_mut_obj_add_str(doc, target, "reason", "Direct caller file dependency.");
    } else if (related->tests > 0) {
        yyjson_mut_obj_add_str(doc, target, "reason", "Direct test relationship.");
    } else if (related->imports > 0) {
        yyjson_mut_obj_add_str(doc, target, "reason", "File import relationship.");
    } else if (related->cochanges > 0) {
        yyjson_mut_obj_add_str(doc, target, "reason", "Historical co-change relationship.");
    } else {
        yyjson_mut_obj_add_str(doc, target, "reason", "Symbol-level call relationship.");
    }
}

static int related_file_priority_score(const file_context_related_t *related) {
    return related->callers * 7 + related->tests * 6 + related->callees * 4 + related->imports * 3 +
           related->cochanges;
}

static const char *related_file_primary_relationship(const file_context_related_t *related) {
    int best_score = -1;
    const char *best = "CALLS_OUT";

    if (related->callers * 7 > best_score) {
        best_score = related->callers * 7;
        best = "CALLS_IN";
    }
    if (related->tests * 6 > best_score) {
        best_score = related->tests * 6;
        best = "TESTS";
    }
    if (related->callees * 4 > best_score) {
        best_score = related->callees * 4;
        best = "CALLS_OUT";
    }
    if (related->imports * 3 > best_score) {
        best_score = related->imports * 3;
        best = "IMPORTS";
    }
    if (related->cochanges > best_score) {
        best = "FILE_CHANGES_WITH";
    }

    return best;
}

static void append_test_command_hints(yyjson_mut_doc *doc, yyjson_mut_val *data,
                                      const test_recommendation_t *tests, int test_count, int limit) {
    yyjson_mut_val *hints = yyjson_mut_arr(doc);
    bool all_go = test_count > 0;
    bool all_py = test_count > 0;

    for (int i = 0; i < test_count && i < limit; i++) {
        all_go = all_go && str_ends_with(tests[i].path, "_test.go");
        all_py = all_py && str_ends_with(tests[i].path, ".py");
    }

    if (test_count > 0) {
        yyjson_mut_val *hint = yyjson_mut_obj(doc);
        if (all_go) {
            yyjson_mut_obj_add_str(doc, hint, "runner", "go");
            yyjson_mut_obj_add_str(doc, hint, "command", "go test ./...");
        } else if (all_py) {
            size_t command_cap = 1024;
            char *command = calloc(command_cap, 1);
            if (command) {
                snprintf(command, command_cap, "pytest");
                for (int i = 0; i < test_count && i < limit; i++) {
                    size_t need = strlen(command) + strlen(tests[i].path) + 4;
                    if (need >= command_cap) {
                        command_cap *= 2;
                        command = safe_realloc(command, command_cap);
                    }
                    strcat(command, " ");
                    strcat(command, tests[i].path);
                }
                strcat(command, " -q");
                yyjson_mut_obj_add_str(doc, hint, "runner", "pytest");
                yyjson_mut_obj_add_strcpy(doc, hint, "command", command);
                free(command);
            }
        } else {
            yyjson_mut_obj_add_str(doc, hint, "runner", "unknown");
            yyjson_mut_obj_add_str(doc, hint, "command",
                                   "Run the project test runner for the listed test files.");
        }
        yyjson_mut_arr_add_val(hints, hint);
    }

    yyjson_mut_obj_add_val(doc, data, "test_command_hints", hints);
}

static yyjson_doc *parse_mcp_text_payload_doc(const char *tool_result_json) {
    if (!tool_result_json) {
        return NULL;
    }

    yyjson_doc *outer = yyjson_read(tool_result_json, strlen(tool_result_json), 0);
    if (!outer) {
        return NULL;
    }

    yyjson_val *outer_root = yyjson_doc_get_root(outer);
    yyjson_val *content = yyjson_obj_get(outer_root, "content");
    yyjson_val *item = content && yyjson_is_arr(content) ? yyjson_arr_get(content, 0) : NULL;
    yyjson_val *text = item ? yyjson_obj_get(item, "text") : NULL;
    const char *payload = text && yyjson_is_str(text) ? yyjson_get_str(text) : NULL;

    yyjson_doc *inner = payload ? yyjson_read(payload, strlen(payload), 0) : NULL;
    yyjson_doc_free(outer);
    return inner;
}

static void append_string_array_values(yyjson_mut_doc *doc, yyjson_mut_val *target, yyjson_val *source) {
    if (!source || !yyjson_is_arr(source)) {
        return;
    }

    yyjson_val *item = NULL;
    yyjson_arr_iter iter = yyjson_arr_iter_with(source);
    while ((item = yyjson_arr_iter_next(&iter)) != NULL) {
        if (yyjson_is_str(item)) {
            yyjson_mut_arr_add_str(doc, target, yyjson_get_str(item));
        }
    }
}

static void append_edit_plan_steps(yyjson_mut_doc *doc, yyjson_mut_val *steps, const char *task_type,
                                   bool include_routes) {
    if (!task_type || strcmp(task_type, "fix") == 0) {
        yyjson_mut_arr_add_str(doc, steps, "Confirm the failing behavior and keep the patch narrow.");
        yyjson_mut_arr_add_str(doc, steps, "Run the highest-value regression tests before finalizing.");
        yyjson_mut_arr_add_str(doc, steps, "Inspect callers and affected symbols before changing behavior.");
    } else if (strcmp(task_type, "refactor") == 0) {
        yyjson_mut_arr_add_str(doc, steps, "Read the top related files before moving logic or signatures.");
        yyjson_mut_arr_add_str(doc, steps, "Preserve caller-visible contracts while restructuring internals.");
        yyjson_mut_arr_add_str(doc, steps, "Run the highest-value tests plus broader regression coverage.");
    } else {
        yyjson_mut_arr_add_str(doc, steps, "Inspect evidence first and avoid editing until the root cause is clear.");
        yyjson_mut_arr_add_str(doc, steps, "Trace callers, related files, and tests before proposing a change.");
        yyjson_mut_arr_add_str(doc, steps, "Prefer confirming the blast radius before making code modifications.");
    }

    if (include_routes) {
        yyjson_mut_arr_add_str(doc, steps,
                               "Verify route and response compatibility if public behavior changes.");
    }
}

/* ── Tool handler implementations ─────────────────────────────── */

/* list_projects: scan cache directory for .db files.
 * Each project is a single .db file — no central registry needed. */
static char *handle_list_projects(cbm_mcp_server_t *srv, const char *args) {
    (void)srv;
    (void)args;

    char dir_path[1024];
    cache_dir(dir_path, sizeof(dir_path));

    cbm_dir_t *d = cbm_opendir(dir_path);

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_val *arr = yyjson_mut_arr(doc);

    if (d) {
        cbm_dirent_t *entry;
        while ((entry = cbm_readdir(d)) != NULL) {
            const char *name = entry->name;
            size_t len = strlen(name);

            /* Must end with .db and be at least 4 chars (x.db) */
            if (len < 4 || strcmp(name + len - 3, ".db") != 0) {
                continue;
            }

            /* Skip temp/internal files */
            if (strncmp(name, "tmp-", 4) == 0 || strncmp(name, "_", 1) == 0 ||
                strncmp(name, ":memory:", 8) == 0) {
                continue;
            }

            /* Extract project name = filename without .db suffix */
            char project_name[1024];
            snprintf(project_name, sizeof(project_name), "%.*s", (int)(len - 3), name);

            /* Get file metadata */
            char full_path[2048];
            snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, name);
            struct stat st;
            if (stat(full_path, &st) != 0) {
                continue;
            }

            /* Open briefly to get node/edge count + root_path */
            cbm_store_t *pstore = cbm_store_open_path(full_path);
            int nodes = 0;
            int edges = 0;
            char root_path_buf[1024] = "";
            if (pstore) {
                nodes = cbm_store_count_nodes(pstore, project_name);
                edges = cbm_store_count_edges(pstore, project_name);
                cbm_project_t proj = {0};
                if (cbm_store_get_project(pstore, project_name, &proj) == CBM_STORE_OK) {
                    if (proj.root_path) {
                        snprintf(root_path_buf, sizeof(root_path_buf), "%s", proj.root_path);
                    }
                    free((void *)proj.name);
                    free((void *)proj.indexed_at);
                    free((void *)proj.root_path);
                }
                cbm_store_close(pstore);
            }

            yyjson_mut_val *p = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_strcpy(doc, p, "name", project_name);
            yyjson_mut_obj_add_strcpy(doc, p, "root_path", root_path_buf);
            yyjson_mut_obj_add_int(doc, p, "nodes", nodes);
            yyjson_mut_obj_add_int(doc, p, "edges", edges);
            yyjson_mut_obj_add_int(doc, p, "size_bytes", (int64_t)st.st_size);
            yyjson_mut_arr_add_val(arr, p);
        }
        cbm_closedir(d);
    }

    yyjson_mut_obj_add_val(doc, root, "projects", arr);

    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);

    char *result = cbm_mcp_text_result(json, false);
    free(json);
    return result;
}

static char *handle_get_graph_schema(cbm_mcp_server_t *srv, const char *args) {
    char *project = cbm_mcp_get_string_arg(args, "project");
    cbm_store_t *store = resolve_store(srv, project);
    REQUIRE_STORE(store, project);

    cbm_schema_info_t schema = {0};
    cbm_store_get_schema(store, project, &schema);

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_val *labels = yyjson_mut_arr(doc);
    for (int i = 0; i < schema.node_label_count; i++) {
        yyjson_mut_val *lbl = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, lbl, "label", schema.node_labels[i].label);
        yyjson_mut_obj_add_int(doc, lbl, "count", schema.node_labels[i].count);
        yyjson_mut_arr_add_val(labels, lbl);
    }
    yyjson_mut_obj_add_val(doc, root, "node_labels", labels);

    yyjson_mut_val *types = yyjson_mut_arr(doc);
    for (int i = 0; i < schema.edge_type_count; i++) {
        yyjson_mut_val *typ = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, typ, "type", schema.edge_types[i].type);
        yyjson_mut_obj_add_int(doc, typ, "count", schema.edge_types[i].count);
        yyjson_mut_arr_add_val(types, typ);
    }
    yyjson_mut_obj_add_val(doc, root, "edge_types", types);

    /* Check ADR presence */
    cbm_project_t proj_info = {0};
    if (cbm_store_get_project(store, project, &proj_info) == 0 && proj_info.root_path) {
        char adr_path[4096];
        snprintf(adr_path, sizeof(adr_path), "%s/.codebase-memory/adr.md", proj_info.root_path);
        struct stat adr_st;
        // NOLINTNEXTLINE(readability-implicit-bool-conversion)
        bool adr_exists = (stat(adr_path, &adr_st) == 0);
        yyjson_mut_obj_add_bool(doc, root, "adr_present", adr_exists);
        if (!adr_exists) {
            yyjson_mut_obj_add_str(
                doc, root, "adr_hint",
                "No ADR found. Use manage_adr(mode='update') to persist architectural "
                "decisions across sessions. Run get_architecture(aspects=['all']) first.");
        }
        cbm_project_free_fields(&proj_info);
    }

    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    cbm_store_schema_free(&schema);
    free(project);

    char *result = cbm_mcp_text_result(json, false);
    free(json);
    return result;
}

static char *handle_get_file_context(cbm_mcp_server_t *srv, const char *args) {
    char *path = cbm_mcp_get_string_arg(args, "path");
    char *project = cbm_mcp_get_string_arg(args, "project");
    int max_related_files = cbm_mcp_get_int_arg(args, "max_related_files", 8);
    int max_tests = cbm_mcp_get_int_arg(args, "max_tests", 6);
    bool include_symbols = true;
    bool include_callers = true;
    bool include_snippets = false;
    cbm_store_t *store = resolve_store(srv, project);

    if (strstr(args, "\"include_symbols\"")) {
        include_symbols = cbm_mcp_get_bool_arg(args, "include_symbols");
    }
    if (strstr(args, "\"include_callers\"")) {
        include_callers = cbm_mcp_get_bool_arg(args, "include_callers");
    }
    if (strstr(args, "\"include_snippets\"")) {
        include_snippets = cbm_mcp_get_bool_arg(args, "include_snippets");
    }

    if (!path) {
        free(project);
        return cbm_mcp_text_result("path is required", true);
    }
    if (!store) {
        free(path);
        free(project);
        return cbm_mcp_text_result("{\"error\":\"no project loaded\"}", true);
    }

    const char *effective_project = project ? project : srv->current_project;
    if (!effective_project) {
        free(path);
        free(project);
        return cbm_mcp_text_result("{\"error\":\"no project loaded\"}", true);
    }

    if (max_related_files <= 0) {
        max_related_files = 8;
    }
    if (max_tests <= 0) {
        max_tests = 6;
    }

    cbm_node_t *nodes = NULL;
    int node_count = 0;
    cbm_store_find_nodes_by_file(store, effective_project, path, &nodes, &node_count);
    if (node_count == 0) {
        free(path);
        free(project);
        cbm_store_free_nodes(nodes, node_count);
        return cbm_mcp_text_result("{\"error\":\"file not found in graph\"}", true);
    }

    cbm_node_t *file_node = NULL;
    int symbol_count = 0;
    int inbound_calls_total = 0;
    int outbound_calls_total = 0;
    file_context_related_t *related = NULL;
    int related_count = 0;
    file_context_caller_t *callers = NULL;
    int caller_count = 0;
    file_context_test_t *tests = NULL;
    int test_count = 0;

    for (int i = 0; i < node_count; i++) {
        cbm_node_t *node = &nodes[i];
        if (node->label && strcmp(node->label, "File") == 0) {
            file_node = node;
            continue;
        }
        if (is_meta_label(node->label)) {
            continue;
        }
        symbol_count++;

        int in_deg = 0;
        int out_deg = 0;
        cbm_store_node_degree(store, node->id, &in_deg, &out_deg);
        inbound_calls_total += in_deg;
        outbound_calls_total += out_deg;

        cbm_edge_t *in_edges = NULL;
        int in_edge_count = 0;
        if (cbm_store_find_edges_by_target_type(store, node->id, "CALLS", &in_edges, &in_edge_count) ==
            CBM_STORE_OK) {
            for (int e = 0; e < in_edge_count; e++) {
                cbm_node_t src = {0};
                if (cbm_store_find_node_by_id(store, in_edges[e].source_id, &src) == CBM_STORE_OK) {
                    add_caller(&callers, &caller_count, &src);
                    add_related_file(&related, &related_count, src.file_path, path, 1, 0, 0, 0, 0);
                    free_node_contents(&src);
                }
            }
        }
        cbm_store_free_edges(in_edges, in_edge_count);

        cbm_edge_t *out_edges = NULL;
        int out_edge_count = 0;
        if (cbm_store_find_edges_by_source_type(store, node->id, "CALLS", &out_edges, &out_edge_count) ==
            CBM_STORE_OK) {
            for (int e = 0; e < out_edge_count; e++) {
                cbm_node_t tgt = {0};
                if (cbm_store_find_node_by_id(store, out_edges[e].target_id, &tgt) == CBM_STORE_OK) {
                    add_related_file(&related, &related_count, tgt.file_path, path, 0, 1, 0, 0, 0);
                    free_node_contents(&tgt);
                }
            }
        }
        cbm_store_free_edges(out_edges, out_edge_count);

        cbm_edge_t *test_edges = NULL;
        int test_edge_count = 0;
        if (cbm_store_find_edges_by_target_type(store, node->id, "TESTS", &test_edges, &test_edge_count) ==
            CBM_STORE_OK) {
            for (int e = 0; e < test_edge_count; e++) {
                cbm_node_t src = {0};
                if (cbm_store_find_node_by_id(store, test_edges[e].source_id, &src) == CBM_STORE_OK) {
                    add_test_file(&tests, &test_count, src.file_path, 1, 0);
                    add_related_file(&related, &related_count, src.file_path, path, 0, 0, 0, 1, 0);
                    free_node_contents(&src);
                }
            }
        }
        cbm_store_free_edges(test_edges, test_edge_count);
    }

    if (file_node) {
        cbm_edge_t *import_out = NULL;
        int import_out_count = 0;
        if (cbm_store_find_edges_by_source_type(store, file_node->id, "IMPORTS", &import_out,
                                                &import_out_count) == CBM_STORE_OK) {
            for (int e = 0; e < import_out_count; e++) {
                cbm_node_t tgt = {0};
                if (cbm_store_find_node_by_id(store, import_out[e].target_id, &tgt) == CBM_STORE_OK) {
                    add_related_file(&related, &related_count, tgt.file_path, path, 0, 0, 1, 0, 0);
                    free_node_contents(&tgt);
                }
            }
        }
        cbm_store_free_edges(import_out, import_out_count);

        cbm_edge_t *import_in = NULL;
        int import_in_count = 0;
        if (cbm_store_find_edges_by_target_type(store, file_node->id, "IMPORTS", &import_in,
                                                &import_in_count) == CBM_STORE_OK) {
            for (int e = 0; e < import_in_count; e++) {
                cbm_node_t src = {0};
                if (cbm_store_find_node_by_id(store, import_in[e].source_id, &src) == CBM_STORE_OK) {
                    add_related_file(&related, &related_count, src.file_path, path, 0, 0, 1, 0, 0);
                    free_node_contents(&src);
                }
            }
        }
        cbm_store_free_edges(import_in, import_in_count);

        cbm_edge_t *tests_file = NULL;
        int tests_file_count = 0;
        if (cbm_store_find_edges_by_target_type(store, file_node->id, "TESTS_FILE", &tests_file,
                                                &tests_file_count) == CBM_STORE_OK) {
            for (int e = 0; e < tests_file_count; e++) {
                cbm_node_t src = {0};
                if (cbm_store_find_node_by_id(store, tests_file[e].source_id, &src) == CBM_STORE_OK) {
                    add_test_file(&tests, &test_count, src.file_path, 0, 1);
                    add_related_file(&related, &related_count, src.file_path, path, 0, 0, 0, 1, 0);
                    free_node_contents(&src);
                }
            }
        }
        cbm_store_free_edges(tests_file, tests_file_count);

        cbm_edge_t *cochange_out = NULL;
        int cochange_out_count = 0;
        if (cbm_store_find_edges_by_source_type(store, file_node->id, "FILE_CHANGES_WITH", &cochange_out,
                                                &cochange_out_count) == CBM_STORE_OK) {
            for (int e = 0; e < cochange_out_count; e++) {
                cbm_node_t tgt = {0};
                if (cbm_store_find_node_by_id(store, cochange_out[e].target_id, &tgt) == CBM_STORE_OK) {
                    add_related_file(&related, &related_count, tgt.file_path, path, 0, 0, 0, 0, 1);
                    free_node_contents(&tgt);
                }
            }
        }
        cbm_store_free_edges(cochange_out, cochange_out_count);
    }

    if (related_count > 1) {
        qsort(related, (size_t)related_count, sizeof(*related), compare_related_files_desc);
    }
    if (caller_count > 1) {
        qsort(callers, (size_t)caller_count, sizeof(*callers), compare_callers_desc);
    }
    if (test_count > 1) {
        qsort(tests, (size_t)test_count, sizeof(*tests), compare_tests_desc);
    }

    const char *risk_level = risk_level_for_context(inbound_calls_total, related_count, test_count);
    const char *confidence = confidence_for_context(file_node != NULL, symbol_count);
    double importance_score = normalize_score(inbound_calls_total + outbound_calls_total + symbol_count, 12);
    double blast_radius_score = normalize_score(inbound_calls_total + related_count, 12);
    char *root_path = get_project_root(srv, effective_project);

    char summary[512];
    snprintf(summary, sizeof(summary),
             "Indexed file with %d symbol%s, %d caller%s, %d related file%s, and %d recommended "
             "test%s.",
             symbol_count, symbol_count == 1 ? "" : "s", caller_count, caller_count == 1 ? "" : "s",
             related_count, related_count == 1 ? "" : "s", test_count, test_count == 1 ? "" : "s");

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_str(doc, root, "tool_version", "1.0");
    yyjson_mut_val *project_obj = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, project_obj, "id", effective_project);
    yyjson_mut_obj_add_str(doc, project_obj, "root_path", root_path ? root_path : "");
    yyjson_mut_obj_add_val(doc, root, "project", project_obj);

    yyjson_mut_val *query_obj = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, query_obj, "path", path);
    yyjson_mut_obj_add_int(doc, query_obj, "max_related_files", max_related_files);
    yyjson_mut_obj_add_int(doc, query_obj, "max_tests", max_tests);
    yyjson_mut_obj_add_bool(doc, query_obj, "include_symbols", include_symbols);
    yyjson_mut_obj_add_bool(doc, query_obj, "include_callers", include_callers);
    yyjson_mut_obj_add_bool(doc, query_obj, "include_snippets", include_snippets);
    yyjson_mut_obj_add_val(doc, root, "query", query_obj);

    yyjson_mut_obj_add_str(doc, root, "summary", summary);
    yyjson_mut_obj_add_str(doc, root, "risk_level", risk_level);
    yyjson_mut_obj_add_str(doc, root, "confidence", confidence);

    yyjson_mut_val *evidence = yyjson_mut_arr(doc);
    yyjson_mut_val *ev1 = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, ev1, "kind", "symbols_in_file");
    yyjson_mut_obj_add_str(doc, ev1, "message", "Symbols were resolved directly from the indexed file.");
    yyjson_mut_obj_add_real(doc, ev1, "weight", normalize_score(symbol_count, 8));
    yyjson_mut_obj_add_str(doc, ev1, "source", "graph");
    yyjson_mut_arr_add_val(evidence, ev1);

    yyjson_mut_val *ev2 = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, ev2, "kind", "call_neighbors");
    yyjson_mut_obj_add_str(doc, ev2, "message", "Inbound and outbound CALLS edges contributed to related files and caller detection.");
    yyjson_mut_obj_add_real(doc, ev2, "weight", normalize_score(inbound_calls_total + outbound_calls_total, 12));
    yyjson_mut_obj_add_str(doc, ev2, "source", "graph");
    yyjson_mut_arr_add_val(evidence, ev2);
    if (test_count > 0) {
        yyjson_mut_val *ev3 = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, ev3, "kind", "test_edges");
        yyjson_mut_obj_add_str(doc, ev3, "message", "Direct TESTS or TESTS_FILE edges were found for this file.");
        yyjson_mut_obj_add_real(doc, ev3, "weight", normalize_score(test_count, 6));
        yyjson_mut_obj_add_str(doc, ev3, "source", "graph");
        yyjson_mut_arr_add_val(evidence, ev3);
    }
    yyjson_mut_obj_add_val(doc, root, "evidence", evidence);

    yyjson_mut_val *next_actions = yyjson_mut_arr(doc);
    yyjson_mut_arr_add_str(doc, next_actions, "Review related files before changing shared contracts.");
    yyjson_mut_arr_add_str(doc, next_actions, "Run recommended tests before finalizing edits.");
    yyjson_mut_obj_add_val(doc, root, "next_actions", next_actions);

    yyjson_mut_val *warnings = yyjson_mut_arr(doc);
    if (!file_node) {
        yyjson_mut_arr_add_str(doc, warnings,
                               "File node missing in graph; file-level relations are partially inferred from symbol edges.");
    }
    if (test_count == 0) {
        yyjson_mut_arr_add_str(doc, warnings,
                               "No direct TESTS or TESTS_FILE edges were found for this file.");
    }
    yyjson_mut_obj_add_val(doc, root, "warnings", warnings);

    yyjson_mut_val *data = yyjson_mut_obj(doc);
    yyjson_mut_val *file_obj = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, file_obj, "path", path);
    yyjson_mut_obj_add_str(doc, file_obj, "language", detect_language_for_path(path));
    yyjson_mut_obj_add_bool(doc, file_obj, "exists_in_graph", true);
    yyjson_mut_obj_add_int(doc, file_obj, "symbol_count", symbol_count);
    yyjson_mut_obj_add_int(doc, file_obj, "caller_count", caller_count);
    yyjson_mut_obj_add_real(doc, file_obj, "importance_score", importance_score);
    yyjson_mut_obj_add_real(doc, file_obj, "blast_radius_score", blast_radius_score);
    yyjson_mut_obj_add_val(doc, data, "file", file_obj);

    yyjson_mut_val *symbols = yyjson_mut_arr(doc);
    if (include_symbols) {
        for (int i = 0; i < node_count; i++) {
            cbm_node_t *node = &nodes[i];
            if (is_meta_label(node->label)) {
                continue;
            }
            yyjson_mut_val *item = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, item, "name", node->name ? node->name : "");
            yyjson_mut_obj_add_str(doc, item, "kind", node->label ? node->label : "");
            yyjson_mut_obj_add_str(doc, item, "qualified_name",
                                   node->qualified_name ? node->qualified_name : "");
            yyjson_mut_obj_add_int(doc, item, "line", node->start_line);
            if (include_snippets && root_path && node->file_path && node->start_line > 0) {
                int snippet_end = node->end_line > node->start_line ? node->end_line : node->start_line + 8;
                if (snippet_end > node->start_line + 12) {
                    snippet_end = node->start_line + 12;
                }
                char *snippet =
                    cbm_read_source_lines_disk(root_path, node->file_path, node->start_line, snippet_end);
                if (snippet) {
                    yyjson_mut_obj_add_strcpy(doc, item, "snippet_preview", snippet);
                    free(snippet);
                }
            }
            yyjson_mut_arr_add_val(symbols, item);
        }
    }
    yyjson_mut_obj_add_val(doc, data, "symbols", symbols);

    yyjson_mut_val *related_arr = yyjson_mut_arr(doc);
    for (int i = 0; i < related_count && i < max_related_files; i++) {
        yyjson_mut_val *item = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, item, "path", related[i].path);
        yyjson_mut_obj_add_real(doc, item, "score", normalize_score(related[i].score, 12));
        yyjson_mut_val *types = yyjson_mut_arr(doc);
        if (related[i].callers > 0) {
            yyjson_mut_arr_add_str(doc, types, "CALLS_IN");
        }
        if (related[i].callees > 0) {
            yyjson_mut_arr_add_str(doc, types, "CALLS_OUT");
        }
        if (related[i].imports > 0) {
            yyjson_mut_arr_add_str(doc, types, "IMPORTS");
        }
        if (related[i].tests > 0) {
            yyjson_mut_arr_add_str(doc, types, "TESTS");
        }
        if (related[i].cochanges > 0) {
            yyjson_mut_arr_add_str(doc, types, "FILE_CHANGES_WITH");
        }
        yyjson_mut_obj_add_val(doc, item, "relationship_types", types);
        if (related[i].callers > 0) {
            yyjson_mut_obj_add_str(doc, item, "reason", "Direct caller file dependency.");
        } else if (related[i].tests > 0) {
            yyjson_mut_obj_add_str(doc, item, "reason", "Direct test relationship.");
        } else if (related[i].imports > 0) {
            yyjson_mut_obj_add_str(doc, item, "reason", "File import relationship.");
        } else if (related[i].cochanges > 0) {
            yyjson_mut_obj_add_str(doc, item, "reason", "Historical co-change relationship.");
        } else {
            yyjson_mut_obj_add_str(doc, item, "reason", "Symbol-level call relationship.");
        }
        yyjson_mut_arr_add_val(related_arr, item);
    }
    yyjson_mut_obj_add_val(doc, data, "related_files", related_arr);

    yyjson_mut_val *callers_arr = yyjson_mut_arr(doc);
    if (include_callers) {
        for (int i = 0; i < caller_count; i++) {
            yyjson_mut_val *item = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, item, "symbol", callers[i].name ? callers[i].name : "");
            yyjson_mut_obj_add_str(doc, item, "qualified_name",
                                   callers[i].qualified_name ? callers[i].qualified_name : "");
            yyjson_mut_obj_add_str(doc, item, "path", callers[i].path ? callers[i].path : "");
            yyjson_mut_obj_add_real(doc, item, "score", normalize_score(callers[i].score, 6));
            yyjson_mut_arr_add_val(callers_arr, item);
        }
    }
    yyjson_mut_obj_add_val(doc, data, "callers", callers_arr);

    yyjson_mut_val *tests_arr = yyjson_mut_arr(doc);
    bool all_go_tests = test_count > 0;
    bool all_py_tests = test_count > 0;
    for (int i = 0; i < test_count && i < max_tests; i++) {
        yyjson_mut_val *item = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, item, "path", tests[i].path);
        yyjson_mut_obj_add_real(doc, item, "score", normalize_score(tests[i].score, 10));
        yyjson_mut_obj_add_str(doc, item, "tier", detect_test_tier(tests[i].path));
        if (tests[i].direct_tests > 0 && tests[i].file_tests > 0) {
            yyjson_mut_obj_add_str(doc, item, "reason", "Direct TESTS and TESTS_FILE edges");
        } else if (tests[i].direct_tests > 0) {
            yyjson_mut_obj_add_str(doc, item, "reason", "Direct TESTS edge");
        } else {
            yyjson_mut_obj_add_str(doc, item, "reason", "Direct TESTS_FILE edge");
        }
        all_go_tests = all_go_tests && str_ends_with(tests[i].path, "_test.go");
        all_py_tests = all_py_tests && str_ends_with(tests[i].path, ".py");
        yyjson_mut_arr_add_val(tests_arr, item);
    }
    yyjson_mut_obj_add_val(doc, data, "tests", tests_arr);

    yyjson_mut_val *hints = yyjson_mut_arr(doc);
    if (test_count > 0) {
        yyjson_mut_val *hint = yyjson_mut_obj(doc);
        if (all_go_tests) {
            yyjson_mut_obj_add_str(doc, hint, "runner", "go");
            yyjson_mut_obj_add_str(doc, hint, "command", "go test ./...");
        } else if (all_py_tests) {
            size_t command_cap = 1024;
            char *command = calloc(command_cap, 1);
            if (command) {
                snprintf(command, command_cap, "pytest");
                for (int i = 0; i < test_count && i < max_tests; i++) {
                    size_t need = strlen(command) + strlen(tests[i].path) + 4;
                    if (need >= command_cap) {
                        command_cap *= 2;
                        command = safe_realloc(command, command_cap);
                    }
                    strcat(command, " ");
                    strcat(command, tests[i].path);
                }
                strcat(command, " -q");
                yyjson_mut_obj_add_str(doc, hint, "runner", "pytest");
                yyjson_mut_obj_add_strcpy(doc, hint, "command", command);
                free(command);
            }
        } else {
            yyjson_mut_obj_add_str(doc, hint, "runner", "unknown");
            yyjson_mut_obj_add_str(doc, hint, "command",
                                   "Run the project test runner for the listed test files.");
        }
        yyjson_mut_arr_add_val(hints, hint);
    }
    yyjson_mut_obj_add_val(doc, data, "test_command_hints", hints);

    yyjson_mut_val *pitfalls = yyjson_mut_arr(doc);
    if (inbound_calls_total > 0) {
        yyjson_mut_arr_add_str(doc, pitfalls, "Behavior changes may affect existing callers.");
    }
    if (related_count >= 3) {
        yyjson_mut_arr_add_str(doc, pitfalls,
                               "File is coupled to multiple nearby files; review related modules before refactors.");
    }
    if (test_count == 0) {
        yyjson_mut_arr_add_str(doc, pitfalls,
                               "No direct tests are linked to this file; validate with broader project tests.");
    }
    for (int i = 0; i < related_count; i++) {
        if (related[i].cochanges > 0) {
            yyjson_mut_arr_add_str(doc, pitfalls,
                                   "Historical co-change suggests this file often changes with nearby files.");
            break;
        }
    }
    yyjson_mut_obj_add_val(doc, data, "pitfalls", pitfalls);

    yyjson_mut_obj_add_val(doc, root, "data", data);

    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);

    free_related_files(related, related_count);
    free_callers(callers, caller_count);
    free_tests(tests, test_count);
    cbm_store_free_nodes(nodes, node_count);
    free(root_path);
    free(path);
    free(project);

    char *result = cbm_mcp_text_result(json, false);
    free(json);
    return result;
}

static char *handle_get_related_files(cbm_mcp_server_t *srv, const char *args) {
    char *path = cbm_mcp_get_string_arg(args, "path");
    char *project = cbm_mcp_get_string_arg(args, "project");
    int limit = cbm_mcp_get_int_arg(args, "limit", 10);
    cbm_store_t *store = resolve_store(srv, project);

    if (!path) {
        free(project);
        return cbm_mcp_text_result("path is required", true);
    }
    if (!store) {
        free(path);
        free(project);
        return cbm_mcp_text_result("{\"error\":\"no project loaded\"}", true);
    }

    const char *effective_project = project ? project : srv->current_project;
    if (!effective_project) {
        free(path);
        free(project);
        return cbm_mcp_text_result("{\"error\":\"no project loaded\"}", true);
    }
    if (limit <= 0) {
        limit = 10;
    }

    cbm_node_t *nodes = NULL;
    int node_count = 0;
    cbm_store_find_nodes_by_file(store, effective_project, path, &nodes, &node_count);
    if (node_count == 0) {
        free(path);
        free(project);
        cbm_store_free_nodes(nodes, node_count);
        return cbm_mcp_text_result("{\"error\":\"file not found in graph\"}", true);
    }

    cbm_node_t *file_node = NULL;
    int inbound_calls_total = 0;
    file_context_related_t *related = NULL;
    int related_count = 0;

    for (int i = 0; i < node_count; i++) {
        cbm_node_t *node = &nodes[i];
        if (node->label && strcmp(node->label, "File") == 0) {
            file_node = node;
            continue;
        }
        if (is_meta_label(node->label)) {
            continue;
        }

        int in_deg = 0;
        int out_deg = 0;
        cbm_store_node_degree(store, node->id, &in_deg, &out_deg);
        inbound_calls_total += in_deg;

        cbm_edge_t *in_edges = NULL;
        int in_edge_count = 0;
        if (cbm_store_find_edges_by_target_type(store, node->id, "CALLS", &in_edges, &in_edge_count) ==
            CBM_STORE_OK) {
            for (int e = 0; e < in_edge_count; e++) {
                cbm_node_t src = {0};
                if (cbm_store_find_node_by_id(store, in_edges[e].source_id, &src) == CBM_STORE_OK) {
                    add_related_file(&related, &related_count, src.file_path, path, 1, 0, 0, 0, 0);
                    free_node_contents(&src);
                }
            }
        }
        cbm_store_free_edges(in_edges, in_edge_count);

        cbm_edge_t *out_edges = NULL;
        int out_edge_count = 0;
        if (cbm_store_find_edges_by_source_type(store, node->id, "CALLS", &out_edges,
                                                &out_edge_count) == CBM_STORE_OK) {
            for (int e = 0; e < out_edge_count; e++) {
                cbm_node_t tgt = {0};
                if (cbm_store_find_node_by_id(store, out_edges[e].target_id, &tgt) == CBM_STORE_OK) {
                    add_related_file(&related, &related_count, tgt.file_path, path, 0, 1, 0, 0, 0);
                    free_node_contents(&tgt);
                }
            }
        }
        cbm_store_free_edges(out_edges, out_edge_count);

        cbm_edge_t *test_edges = NULL;
        int test_edge_count = 0;
        if (cbm_store_find_edges_by_target_type(store, node->id, "TESTS", &test_edges,
                                                &test_edge_count) == CBM_STORE_OK) {
            for (int e = 0; e < test_edge_count; e++) {
                cbm_node_t src = {0};
                if (cbm_store_find_node_by_id(store, test_edges[e].source_id, &src) == CBM_STORE_OK) {
                    add_related_file(&related, &related_count, src.file_path, path, 0, 0, 0, 1, 0);
                    free_node_contents(&src);
                }
            }
        }
        cbm_store_free_edges(test_edges, test_edge_count);
    }

    if (file_node) {
        cbm_edge_t *import_out = NULL;
        int import_out_count = 0;
        if (cbm_store_find_edges_by_source_type(store, file_node->id, "IMPORTS", &import_out,
                                                &import_out_count) == CBM_STORE_OK) {
            for (int e = 0; e < import_out_count; e++) {
                cbm_node_t tgt = {0};
                if (cbm_store_find_node_by_id(store, import_out[e].target_id, &tgt) == CBM_STORE_OK) {
                    add_related_file(&related, &related_count, tgt.file_path, path, 0, 0, 1, 0, 0);
                    free_node_contents(&tgt);
                }
            }
        }
        cbm_store_free_edges(import_out, import_out_count);

        cbm_edge_t *import_in = NULL;
        int import_in_count = 0;
        if (cbm_store_find_edges_by_target_type(store, file_node->id, "IMPORTS", &import_in,
                                                &import_in_count) == CBM_STORE_OK) {
            for (int e = 0; e < import_in_count; e++) {
                cbm_node_t src = {0};
                if (cbm_store_find_node_by_id(store, import_in[e].source_id, &src) == CBM_STORE_OK) {
                    add_related_file(&related, &related_count, src.file_path, path, 0, 0, 1, 0, 0);
                    free_node_contents(&src);
                }
            }
        }
        cbm_store_free_edges(import_in, import_in_count);

        cbm_edge_t *tests_file = NULL;
        int tests_file_count = 0;
        if (cbm_store_find_edges_by_target_type(store, file_node->id, "TESTS_FILE", &tests_file,
                                                &tests_file_count) == CBM_STORE_OK) {
            for (int e = 0; e < tests_file_count; e++) {
                cbm_node_t src = {0};
                if (cbm_store_find_node_by_id(store, tests_file[e].source_id, &src) == CBM_STORE_OK) {
                    add_related_file(&related, &related_count, src.file_path, path, 0, 0, 0, 1, 0);
                    free_node_contents(&src);
                }
            }
        }
        cbm_store_free_edges(tests_file, tests_file_count);

        cbm_edge_t *cochange_out = NULL;
        int cochange_out_count = 0;
        if (cbm_store_find_edges_by_source_type(store, file_node->id, "FILE_CHANGES_WITH", &cochange_out,
                                                &cochange_out_count) == CBM_STORE_OK) {
            for (int e = 0; e < cochange_out_count; e++) {
                cbm_node_t tgt = {0};
                if (cbm_store_find_node_by_id(store, cochange_out[e].target_id, &tgt) == CBM_STORE_OK) {
                    add_related_file(&related, &related_count, tgt.file_path, path, 0, 0, 0, 0, 1);
                    free_node_contents(&tgt);
                }
            }
        }
        cbm_store_free_edges(cochange_out, cochange_out_count);
    }

    if (related_count > 1) {
        qsort(related, (size_t)related_count, sizeof(*related), compare_related_files_desc);
    }

    char *root_path = get_project_root(srv, effective_project);
    const char *risk_level = risk_level_for_context(inbound_calls_total, related_count, 0);
    const char *confidence = confidence_for_context(file_node != NULL, node_count > 0 ? node_count - 1 : 0);

    char summary[512];
    snprintf(summary, sizeof(summary), "Found %d related file%s for %s.", related_count,
             related_count == 1 ? "" : "s", path);

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_str(doc, root, "tool_version", "1.0");
    yyjson_mut_val *project_obj = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, project_obj, "id", effective_project);
    yyjson_mut_obj_add_str(doc, project_obj, "root_path", root_path ? root_path : "");
    yyjson_mut_obj_add_val(doc, root, "project", project_obj);

    yyjson_mut_val *query_obj = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, query_obj, "path", path);
    yyjson_mut_obj_add_int(doc, query_obj, "limit", limit);
    yyjson_mut_obj_add_val(doc, root, "query", query_obj);

    yyjson_mut_obj_add_str(doc, root, "summary", summary);
    yyjson_mut_obj_add_str(doc, root, "risk_level", risk_level);
    yyjson_mut_obj_add_str(doc, root, "confidence", confidence);

    yyjson_mut_val *evidence = yyjson_mut_arr(doc);
    yyjson_mut_val *ev1 = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, ev1, "kind", "related_file_edges");
    yyjson_mut_obj_add_str(doc, ev1, "message",
                           "Related files were ranked from CALLS, IMPORTS, TESTS, and FILE_CHANGES_WITH edges.");
    yyjson_mut_obj_add_real(doc, ev1, "weight", normalize_score(related_count, limit > 0 ? limit : 10));
    yyjson_mut_obj_add_str(doc, ev1, "source", "graph");
    yyjson_mut_arr_add_val(evidence, ev1);
    yyjson_mut_obj_add_val(doc, root, "evidence", evidence);

    yyjson_mut_val *next_actions = yyjson_mut_arr(doc);
    yyjson_mut_arr_add_str(doc, next_actions, "Inspect the top related files before changing shared behavior.");
    yyjson_mut_obj_add_val(doc, root, "next_actions", next_actions);

    yyjson_mut_val *warnings = yyjson_mut_arr(doc);
    if (!file_node) {
        yyjson_mut_arr_add_str(doc, warnings,
                               "File node missing in graph; some file-level relationships may be absent.");
    }
    yyjson_mut_obj_add_val(doc, root, "warnings", warnings);

    yyjson_mut_val *data = yyjson_mut_obj(doc);
    yyjson_mut_val *target = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, target, "path", path);
    yyjson_mut_obj_add_val(doc, data, "target", target);

    yyjson_mut_val *related_arr = yyjson_mut_arr(doc);
    for (int i = 0; i < related_count && i < limit; i++) {
        yyjson_mut_val *item = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, item, "path", related[i].path);
        yyjson_mut_obj_add_real(doc, item, "score", normalize_score(related[i].score, 12));
        yyjson_mut_obj_add_real(doc, item, "importance_score",
                                normalize_score(related[i].callers + related[i].callees + related[i].imports +
                                                    related[i].tests + related[i].cochanges,
                                                8));
        yyjson_mut_val *types = yyjson_mut_arr(doc);
        if (related[i].callers > 0) {
            yyjson_mut_arr_add_str(doc, types, "CALLS_IN");
        }
        if (related[i].callees > 0) {
            yyjson_mut_arr_add_str(doc, types, "CALLS_OUT");
        }
        if (related[i].imports > 0) {
            yyjson_mut_arr_add_str(doc, types, "IMPORTS");
        }
        if (related[i].tests > 0) {
            yyjson_mut_arr_add_str(doc, types, "TESTS");
        }
        if (related[i].cochanges > 0) {
            yyjson_mut_arr_add_str(doc, types, "FILE_CHANGES_WITH");
        }
        yyjson_mut_obj_add_val(doc, item, "relationship_types", types);
        if (related[i].callers > 0) {
            yyjson_mut_obj_add_str(doc, item, "reason", "Direct caller file dependency.");
        } else if (related[i].tests > 0) {
            yyjson_mut_obj_add_str(doc, item, "reason", "Direct test relationship.");
        } else if (related[i].imports > 0) {
            yyjson_mut_obj_add_str(doc, item, "reason", "File import relationship.");
        } else if (related[i].cochanges > 0) {
            yyjson_mut_obj_add_str(doc, item, "reason", "Historical co-change relationship.");
        } else {
            yyjson_mut_obj_add_str(doc, item, "reason", "Symbol-level call relationship.");
        }
        yyjson_mut_arr_add_val(related_arr, item);
    }
    yyjson_mut_obj_add_val(doc, data, "related_files", related_arr);
    yyjson_mut_obj_add_val(doc, root, "data", data);

    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);

    free_related_files(related, related_count);
    cbm_store_free_nodes(nodes, node_count);
    free(root_path);
    free(path);
    free(project);

    char *result = cbm_mcp_text_result(json, false);
    free(json);
    return result;
}

static char *handle_get_tests(cbm_mcp_server_t *srv, const char *args) {
    char *project = cbm_mcp_get_string_arg(args, "project");
    char *single_path = cbm_mcp_get_string_arg(args, "path");
    int path_count = 0;
    char **paths = cbm_mcp_get_string_array_arg(args, "paths", &path_count);
    int limit = cbm_mcp_get_int_arg(args, "limit", 8);
    bool include_reasons = true;
    if (strstr(args, "\"include_reasons\"")) {
        include_reasons = cbm_mcp_get_bool_arg(args, "include_reasons");
    }
    cbm_store_t *store = resolve_store(srv, project);

    if (!paths && single_path) {
        paths = calloc(1, sizeof(char *));
        if (paths) {
            paths[0] = single_path;
            single_path = NULL;
            path_count = 1;
        }
    }

    if (!paths || path_count <= 0) {
        free(single_path);
        free(project);
        free_string_array(paths, path_count);
        return cbm_mcp_text_result("paths or path is required", true);
    }
    if (!store) {
        free(single_path);
        free(project);
        free_string_array(paths, path_count);
        return cbm_mcp_text_result("{\"error\":\"no project loaded\"}", true);
    }

    const char *effective_project = project ? project : srv->current_project;
    if (!effective_project) {
        free(single_path);
        free(project);
        free_string_array(paths, path_count);
        return cbm_mcp_text_result("{\"error\":\"no project loaded\"}", true);
    }
    if (limit <= 0) {
        limit = 8;
    }

    test_recommendation_t *tests = NULL;
    int test_count = 0;
    int indexed_target_count = 0;
    int missing_target_count = 0;

    for (int p = 0; p < path_count; p++) {
        const char *target_path = paths[p];
        cbm_node_t *nodes = NULL;
        int node_count = 0;
        cbm_store_find_nodes_by_file(store, effective_project, target_path, &nodes, &node_count);
        if (node_count == 0) {
            missing_target_count++;
            cbm_store_free_nodes(nodes, node_count);
            continue;
        }
        indexed_target_count++;

        cbm_node_t *file_node = NULL;
        for (int i = 0; i < node_count; i++) {
            cbm_node_t *node = &nodes[i];
            if (node->label && strcmp(node->label, "File") == 0) {
                file_node = node;
                continue;
            }
            if (is_meta_label(node->label)) {
                continue;
            }

            cbm_edge_t *test_edges = NULL;
            int test_edge_count = 0;
            if (cbm_store_find_edges_by_target_type(store, node->id, "TESTS", &test_edges,
                                                    &test_edge_count) == CBM_STORE_OK) {
                for (int e = 0; e < test_edge_count; e++) {
                    cbm_node_t src = {0};
                    if (cbm_store_find_node_by_id(store, test_edges[e].source_id, &src) == CBM_STORE_OK) {
                        add_test_recommendation(&tests, &test_count, src.file_path, target_path, 1, 0);
                        free_node_contents(&src);
                    }
                }
            }
            cbm_store_free_edges(test_edges, test_edge_count);
        }

        if (file_node) {
            cbm_edge_t *tests_file = NULL;
            int tests_file_count = 0;
            if (cbm_store_find_edges_by_target_type(store, file_node->id, "TESTS_FILE", &tests_file,
                                                    &tests_file_count) == CBM_STORE_OK) {
                for (int e = 0; e < tests_file_count; e++) {
                    cbm_node_t src = {0};
                    if (cbm_store_find_node_by_id(store, tests_file[e].source_id, &src) == CBM_STORE_OK) {
                        add_test_recommendation(&tests, &test_count, src.file_path, target_path, 0, 1);
                        free_node_contents(&src);
                    }
                }
            }
            cbm_store_free_edges(tests_file, tests_file_count);
        }

        cbm_store_free_nodes(nodes, node_count);
    }

    if (test_count > 1) {
        qsort(tests, (size_t)test_count, sizeof(*tests), compare_test_recommendations_desc);
    }

    char *root_path = get_project_root(srv, effective_project);
    const char *risk_level = test_count == 0 ? "medium" : "low";
    const char *confidence = indexed_target_count > 0 ? "high" : "low";

    char summary[512];
    snprintf(summary, sizeof(summary), "Found %d recommended test%s for %d indexed target%s.", test_count,
             test_count == 1 ? "" : "s", indexed_target_count, indexed_target_count == 1 ? "" : "s");

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_str(doc, root, "tool_version", "1.0");
    yyjson_mut_val *project_obj = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, project_obj, "id", effective_project);
    yyjson_mut_obj_add_str(doc, project_obj, "root_path", root_path ? root_path : "");
    yyjson_mut_obj_add_val(doc, root, "project", project_obj);

    yyjson_mut_val *query_obj = yyjson_mut_obj(doc);
    yyjson_mut_val *query_paths = yyjson_mut_arr(doc);
    for (int i = 0; i < path_count; i++) {
        yyjson_mut_arr_add_str(doc, query_paths, paths[i]);
    }
    yyjson_mut_obj_add_val(doc, query_obj, "paths", query_paths);
    yyjson_mut_obj_add_int(doc, query_obj, "limit", limit);
    yyjson_mut_obj_add_bool(doc, query_obj, "include_reasons", include_reasons);
    yyjson_mut_obj_add_val(doc, root, "query", query_obj);

    yyjson_mut_obj_add_str(doc, root, "summary", summary);
    yyjson_mut_obj_add_str(doc, root, "risk_level", risk_level);
    yyjson_mut_obj_add_str(doc, root, "confidence", confidence);

    yyjson_mut_val *evidence = yyjson_mut_arr(doc);
    yyjson_mut_val *ev1 = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, ev1, "kind", "test_edges");
    yyjson_mut_obj_add_str(doc, ev1, "message",
                           "Test recommendations were derived from TESTS and TESTS_FILE edges.");
    yyjson_mut_obj_add_real(doc, ev1, "weight", normalize_score(test_count, limit > 0 ? limit : 8));
    yyjson_mut_obj_add_str(doc, ev1, "source", "graph");
    yyjson_mut_arr_add_val(evidence, ev1);
    yyjson_mut_obj_add_val(doc, root, "evidence", evidence);

    yyjson_mut_val *next_actions = yyjson_mut_arr(doc);
    yyjson_mut_arr_add_str(doc, next_actions, "Run the highest-ranked tests before finalizing edits.");
    yyjson_mut_obj_add_val(doc, root, "next_actions", next_actions);

    yyjson_mut_val *warnings = yyjson_mut_arr(doc);
    if (missing_target_count > 0) {
        yyjson_mut_arr_add_str(doc, warnings,
                               "Some requested paths were not found in the indexed project.");
    }
    if (test_count == 0) {
        yyjson_mut_arr_add_str(doc, warnings,
                               "No direct TESTS or TESTS_FILE edges were found for the requested paths.");
    }
    yyjson_mut_obj_add_val(doc, root, "warnings", warnings);

    yyjson_mut_val *data = yyjson_mut_obj(doc);
    yyjson_mut_val *targets = yyjson_mut_arr(doc);
    for (int i = 0; i < path_count; i++) {
        yyjson_mut_val *item = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, item, "path", paths[i]);
        yyjson_mut_arr_add_val(targets, item);
    }
    yyjson_mut_obj_add_val(doc, data, "targets", targets);

    yyjson_mut_val *tests_arr = yyjson_mut_arr(doc);
    bool all_go = test_count > 0;
    bool all_py = test_count > 0;
    for (int i = 0; i < test_count && i < limit; i++) {
        yyjson_mut_val *item = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, item, "path", tests[i].path);
        yyjson_mut_obj_add_real(doc, item, "score", normalize_score(tests[i].score, 10));
        yyjson_mut_obj_add_str(doc, item, "tier", detect_test_tier(tests[i].path));
        if (include_reasons) {
            if (tests[i].direct_tests > 0 && tests[i].file_tests > 0) {
                yyjson_mut_obj_add_str(doc, item, "reason", "Direct TESTS and TESTS_FILE edges");
            } else if (tests[i].direct_tests > 0) {
                yyjson_mut_obj_add_str(doc, item, "reason", "Direct TESTS edge");
            } else {
                yyjson_mut_obj_add_str(doc, item, "reason", "Direct TESTS_FILE edge");
            }
        }
        yyjson_mut_val *covers = yyjson_mut_arr(doc);
        for (int c = 0; c < tests[i].cover_count; c++) {
            yyjson_mut_arr_add_str(doc, covers, tests[i].covers[c]);
        }
        yyjson_mut_obj_add_val(doc, item, "covers", covers);
        yyjson_mut_arr_add_val(tests_arr, item);

        all_go = all_go && str_ends_with(tests[i].path, "_test.go");
        all_py = all_py && str_ends_with(tests[i].path, ".py");
    }
    yyjson_mut_obj_add_val(doc, data, "tests", tests_arr);

    yyjson_mut_val *hints = yyjson_mut_arr(doc);
    if (test_count > 0) {
        yyjson_mut_val *hint = yyjson_mut_obj(doc);
        if (all_go) {
            yyjson_mut_obj_add_str(doc, hint, "runner", "go");
            yyjson_mut_obj_add_str(doc, hint, "command", "go test ./...");
        } else if (all_py) {
            size_t command_cap = 1024;
            char *command = calloc(command_cap, 1);
            if (command) {
                snprintf(command, command_cap, "pytest");
                for (int i = 0; i < test_count && i < limit; i++) {
                    size_t need = strlen(command) + strlen(tests[i].path) + 4;
                    if (need >= command_cap) {
                        command_cap *= 2;
                        command = safe_realloc(command, command_cap);
                    }
                    strcat(command, " ");
                    strcat(command, tests[i].path);
                }
                strcat(command, " -q");
                yyjson_mut_obj_add_str(doc, hint, "runner", "pytest");
                yyjson_mut_obj_add_strcpy(doc, hint, "command", command);
                free(command);
            }
        } else {
            yyjson_mut_obj_add_str(doc, hint, "runner", "unknown");
            yyjson_mut_obj_add_str(doc, hint, "command", "Run the project test runner for the listed test files.");
        }
        yyjson_mut_arr_add_val(hints, hint);
    }
    yyjson_mut_obj_add_val(doc, data, "test_command_hints", hints);
    yyjson_mut_obj_add_val(doc, root, "data", data);

    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);

    free(root_path);
    free(single_path);
    free(project);
    free_string_array(paths, path_count);
    free_test_recommendations(tests, test_count);

    char *result = cbm_mcp_text_result(json, false);
    free(json);
    return result;
}

static char *handle_get_callers(cbm_mcp_server_t *srv, const char *args) {
    char *symbol = cbm_mcp_get_string_arg(args, "symbol");
    char *path = cbm_mcp_get_string_arg(args, "path");
    char *project = cbm_mcp_get_string_arg(args, "project");
    int depth = cbm_mcp_get_int_arg(args, "depth", 2);
    bool include_file_aggregation = true;
    if (strstr(args, "\"include_file_aggregation\"")) {
        include_file_aggregation = cbm_mcp_get_bool_arg(args, "include_file_aggregation");
    }
    cbm_store_t *store = resolve_store(srv, project);

    if (!symbol && !path) {
        free(symbol);
        free(path);
        free(project);
        return cbm_mcp_text_result("symbol or path is required", true);
    }
    if (!store) {
        free(symbol);
        free(path);
        free(project);
        return cbm_mcp_text_result("{\"error\":\"no project loaded\"}", true);
    }

    const char *effective_project = project ? project : srv->current_project;
    if (!effective_project) {
        free(symbol);
        free(path);
        free(project);
        return cbm_mcp_text_result("{\"error\":\"no project loaded\"}", true);
    }
    if (depth <= 0) {
        depth = 2;
    }

    cbm_node_t *targets = NULL;
    int target_count = 0;
    symbol_match_t *matches = NULL;
    int match_count = 0;
    bool ambiguous_symbol = false;

    if (symbol) {
        cbm_node_t exact = {0};
        if (cbm_store_find_node_by_qn(store, effective_project, symbol, &exact) == CBM_STORE_OK) {
            targets = calloc(1, sizeof(cbm_node_t));
            if (!targets) {
                free_node_contents(&exact);
                free(symbol);
                free(path);
                free(project);
                return cbm_mcp_text_result("{\"error\":\"allocation failed\"}", true);
            }
            copy_node(&exact, &targets[0]);
            target_count = 1;
            add_symbol_match(&matches, &match_count, &exact, 10);
            free_node_contents(&exact);
        } else {
            cbm_node_t *suffix_nodes = NULL;
            int suffix_count = 0;
            cbm_store_find_nodes_by_qn_suffix(store, effective_project, symbol, &suffix_nodes, &suffix_count);
            if (suffix_count > 0) {
                for (int i = 0; i < suffix_count; i++) {
                    add_symbol_match(&matches, &match_count, &suffix_nodes[i], 8);
                }
                if (suffix_count == 1) {
                    targets = calloc(1, sizeof(cbm_node_t));
                    if (targets) {
                        copy_node(&suffix_nodes[0], &targets[0]);
                        target_count = 1;
                    }
                } else {
                    ambiguous_symbol = true;
                }
                cbm_store_free_nodes(suffix_nodes, suffix_count);
            } else {
                cbm_node_t *name_nodes = NULL;
                int name_count = 0;
                cbm_store_find_nodes_by_name(store, effective_project, symbol, &name_nodes, &name_count);
                if (name_count == 0) {
                    free(symbol);
                    free(path);
                    free(project);
                    free_symbol_matches(matches, match_count);
                    return cbm_mcp_text_result("{\"error\":\"symbol not found\"}", true);
                }
                for (int i = 0; i < name_count; i++) {
                    add_symbol_match(&matches, &match_count, &name_nodes[i], 6);
                }
                if (name_count == 1) {
                    targets = calloc(1, sizeof(cbm_node_t));
                    if (targets) {
                        copy_node(&name_nodes[0], &targets[0]);
                        target_count = 1;
                    }
                } else {
                    ambiguous_symbol = true;
                }
                cbm_store_free_nodes(name_nodes, name_count);
            }
        }
    } else {
        cbm_node_t *nodes = NULL;
        int node_count = 0;
        cbm_store_find_nodes_by_file(store, effective_project, path, &nodes, &node_count);
        if (node_count == 0) {
            free(symbol);
            free(path);
            free(project);
            return cbm_mcp_text_result("{\"error\":\"file not found in graph\"}", true);
        }

        for (int i = 0; i < node_count; i++) {
            cbm_node_t *node = &nodes[i];
            if (node->label && strcmp(node->label, "File") == 0) {
                continue;
            }
            if (is_meta_label(node->label)) {
                continue;
            }
            cbm_node_t *next = safe_realloc(targets, (size_t)(target_count + 1) * sizeof(*targets));
            targets = next;
            memset(&targets[target_count], 0, sizeof(*targets));
            copy_node(node, &targets[target_count]);
            target_count++;
            add_symbol_match(&matches, &match_count, node, 10);
        }
        cbm_store_free_nodes(nodes, node_count);
        if (target_count == 0) {
            free(symbol);
            free(path);
            free(project);
            free_symbol_matches(matches, match_count);
            free(targets);
            return cbm_mcp_text_result("{\"error\":\"no callable symbols found in file\"}", true);
        }
    }

    if (match_count > 1) {
        qsort(matches, (size_t)match_count, sizeof(*matches), compare_symbol_matches_desc);
    }

    caller_result_t *callers = NULL;
    int caller_count = 0;
    caller_file_agg_t *by_file = NULL;
    int by_file_count = 0;

    if (!ambiguous_symbol) {
        const char *edge_types[] = {"CALLS"};
        int edge_type_count = 1;
        for (int i = 0; i < target_count; i++) {
            cbm_traverse_result_t tr = {0};
            if (cbm_store_bfs(store, targets[i].id, "inbound", edge_types, edge_type_count, depth, 200,
                              &tr) == CBM_STORE_OK) {
                for (int v = 0; v < tr.visited_count; v++) {
                    add_caller_result(&callers, &caller_count, &tr.visited[v].node, tr.visited[v].hop);
                }
                cbm_store_traverse_free(&tr);
            }
        }
    }

    if (caller_count > 1) {
        qsort(callers, (size_t)caller_count, sizeof(*callers), compare_caller_results_desc);
    }

    if (include_file_aggregation) {
        for (int i = 0; i < caller_count; i++) {
            add_caller_file_agg(&by_file, &by_file_count, callers[i].path, callers[i].score);
        }
        if (by_file_count > 1) {
            qsort(by_file, (size_t)by_file_count, sizeof(*by_file), compare_caller_file_aggs_desc);
        }
    }

    char *root_path = get_project_root(srv, effective_project);
    const char *risk_level = caller_count >= 5 ? "high" : (caller_count > 0 ? "medium" : "low");
    const char *confidence = ambiguous_symbol ? "medium" : (target_count > 0 ? "high" : "low");

    char summary[512];
    if (ambiguous_symbol) {
        snprintf(summary, sizeof(summary), "Found %d candidate symbol matches for %s; caller expansion was skipped.",
                 match_count, symbol ? symbol : "");
    } else {
        snprintf(summary, sizeof(summary), "Found %d caller%s across %d target%s.", caller_count,
                 caller_count == 1 ? "" : "s", target_count, target_count == 1 ? "" : "s");
    }

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_str(doc, root, "tool_version", "1.0");
    yyjson_mut_val *project_obj = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, project_obj, "id", effective_project);
    yyjson_mut_obj_add_str(doc, project_obj, "root_path", root_path ? root_path : "");
    yyjson_mut_obj_add_val(doc, root, "project", project_obj);

    yyjson_mut_val *query_obj = yyjson_mut_obj(doc);
    if (symbol) {
        yyjson_mut_obj_add_str(doc, query_obj, "symbol", symbol);
    }
    if (path) {
        yyjson_mut_obj_add_str(doc, query_obj, "path", path);
    }
    yyjson_mut_obj_add_int(doc, query_obj, "depth", depth);
    yyjson_mut_obj_add_bool(doc, query_obj, "include_file_aggregation", include_file_aggregation);
    yyjson_mut_obj_add_val(doc, root, "query", query_obj);

    yyjson_mut_obj_add_str(doc, root, "summary", summary);
    yyjson_mut_obj_add_str(doc, root, "risk_level", risk_level);
    yyjson_mut_obj_add_str(doc, root, "confidence", confidence);

    yyjson_mut_val *evidence = yyjson_mut_arr(doc);
    yyjson_mut_val *ev1 = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, ev1, "kind", "inbound_call_edges");
    yyjson_mut_obj_add_str(doc, ev1, "message", "Callers were derived from inbound CALLS traversal.");
    yyjson_mut_obj_add_real(doc, ev1, "weight", normalize_score(caller_count, 10));
    yyjson_mut_obj_add_str(doc, ev1, "source", "graph");
    yyjson_mut_arr_add_val(evidence, ev1);
    yyjson_mut_obj_add_val(doc, root, "evidence", evidence);

    yyjson_mut_val *next_actions = yyjson_mut_arr(doc);
    yyjson_mut_arr_add_str(doc, next_actions, "Review inbound callers before changing shared symbol behavior.");
    yyjson_mut_obj_add_val(doc, root, "next_actions", next_actions);

    yyjson_mut_val *warnings = yyjson_mut_arr(doc);
    if (ambiguous_symbol) {
        yyjson_mut_arr_add_str(doc, warnings,
                               "Multiple strong symbol matches were found; callers were not expanded.");
    }
    if (!ambiguous_symbol && caller_count == 0) {
        yyjson_mut_arr_add_str(doc, warnings, "No inbound CALLS edges were found for the target.");
    }
    yyjson_mut_obj_add_val(doc, root, "warnings", warnings);

    yyjson_mut_val *data = yyjson_mut_obj(doc);
    yyjson_mut_val *target_obj = yyjson_mut_obj(doc);
    if (symbol) {
        yyjson_mut_obj_add_str(doc, target_obj, "symbol", symbol);
    }
    if (path) {
        yyjson_mut_obj_add_str(doc, target_obj, "path", path);
    }
    if (!ambiguous_symbol && target_count == 1 && symbol) {
        yyjson_mut_obj_add_str(doc, target_obj, "resolved_qualified_name",
                               targets[0].qualified_name ? targets[0].qualified_name : "");
    }
    yyjson_mut_obj_add_val(doc, data, "target", target_obj);

    yyjson_mut_val *matches_arr = yyjson_mut_arr(doc);
    for (int i = 0; i < match_count; i++) {
        yyjson_mut_val *item = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, item, "qualified_name", matches[i].qualified_name);
        yyjson_mut_obj_add_str(doc, item, "path", matches[i].path ? matches[i].path : "");
        yyjson_mut_obj_add_real(doc, item, "score", normalize_score(matches[i].score, 10));
        yyjson_mut_arr_add_val(matches_arr, item);
    }
    yyjson_mut_obj_add_val(doc, data, "matches", matches_arr);

    yyjson_mut_val *callers_arr = yyjson_mut_arr(doc);
    for (int i = 0; i < caller_count; i++) {
        yyjson_mut_val *item = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, item, "symbol", callers[i].name ? callers[i].name : "");
        yyjson_mut_obj_add_str(doc, item, "qualified_name",
                               callers[i].qualified_name ? callers[i].qualified_name : "");
        yyjson_mut_obj_add_str(doc, item, "path", callers[i].path ? callers[i].path : "");
        yyjson_mut_obj_add_int(doc, item, "depth", callers[i].depth);
        yyjson_mut_obj_add_real(doc, item, "score", normalize_score(callers[i].score, 6));
        yyjson_mut_arr_add_val(callers_arr, item);
    }
    yyjson_mut_obj_add_val(doc, data, "callers", callers_arr);

    yyjson_mut_val *by_file_arr = yyjson_mut_arr(doc);
    if (include_file_aggregation) {
        for (int i = 0; i < by_file_count; i++) {
            yyjson_mut_val *item = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, item, "path", by_file[i].path ? by_file[i].path : "");
            yyjson_mut_obj_add_int(doc, item, "caller_count", by_file[i].caller_count);
            yyjson_mut_obj_add_real(doc, item, "score", normalize_score(by_file[i].score, 10));
            yyjson_mut_arr_add_val(by_file_arr, item);
        }
    }
    yyjson_mut_obj_add_val(doc, data, "by_file", by_file_arr);
    yyjson_mut_obj_add_val(doc, root, "data", data);

    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);

    free(root_path);
    free(symbol);
    free(path);
    free(project);
    for (int i = 0; i < target_count; i++) {
        free_node_contents(&targets[i]);
    }
    free(targets);
    free_symbol_matches(matches, match_count);
    free_caller_results(callers, caller_count);
    free_caller_file_aggs(by_file, by_file_count);

    char *result = cbm_mcp_text_result(json, false);
    free(json);
    return result;
}

static char *handle_get_change_risks(cbm_mcp_server_t *srv, const char *args) {
    int path_count = 0;
    char **paths = cbm_mcp_get_string_array_arg(args, "paths", &path_count);
    char *single_path = cbm_mcp_get_string_arg(args, "path");
    char *diff_mode = cbm_mcp_get_string_arg(args, "diff_mode");
    char *project = cbm_mcp_get_string_arg(args, "project");
    bool include_tests = true;
    bool include_routes = true;
    int max_related_files = cbm_mcp_get_int_arg(args, "max_related_files", 8);
    int max_tests = cbm_mcp_get_int_arg(args, "max_tests", 6);
    cbm_store_t *store = resolve_store(srv, project);

    if (strstr(args, "\"include_tests\"")) {
        include_tests = cbm_mcp_get_bool_arg(args, "include_tests");
    }
    if (strstr(args, "\"include_routes\"")) {
        include_routes = cbm_mcp_get_bool_arg(args, "include_routes");
    }
    if (max_related_files <= 0) {
        max_related_files = 8;
    }
    if (max_tests <= 0) {
        max_tests = 6;
    }

    if (path_count == 0 && single_path) {
        paths = calloc(1, sizeof(*paths));
        if (!paths) {
            free(single_path);
            free(diff_mode);
            free(project);
            return cbm_mcp_text_result("{\"error\":\"allocation failed\"}", true);
        }
        paths[0] = single_path;
        single_path = NULL;
        path_count = 1;
    }

    if (path_count == 0 && !diff_mode) {
        free(single_path);
        free(diff_mode);
        free(project);
        free_string_array(paths, path_count);
        return cbm_mcp_text_result("paths or diff_mode is required", true);
    }
    if (!store) {
        free(single_path);
        free(diff_mode);
        free(project);
        free_string_array(paths, path_count);
        return cbm_mcp_text_result("{\"error\":\"no project loaded\"}", true);
    }

    const char *effective_project = project ? project : srv->current_project;
    if (!effective_project) {
        free(single_path);
        free(diff_mode);
        free(project);
        free_string_array(paths, path_count);
        return cbm_mcp_text_result("{\"error\":\"no project loaded\"}", true);
    }

    char *root_path = NULL;
    if (path_count == 0 && diff_mode) {
        if (strcmp(diff_mode, "working_tree") != 0) {
            free(single_path);
            free(diff_mode);
            free(project);
            free_string_array(paths, path_count);
            return cbm_mcp_text_result("unsupported diff_mode", true);
        }
        root_path = get_project_root(srv, effective_project);
        if (!root_path) {
            free(single_path);
            free(diff_mode);
            free(project);
            free_string_array(paths, path_count);
            return cbm_mcp_text_result("{\"error\":\"project not found\"}", true);
        }
        paths = collect_working_tree_paths(root_path, &path_count);
        if (!paths && path_count == 0) {
            free(single_path);
            free(diff_mode);
            free(project);
            free(root_path);
            return cbm_mcp_text_result("{\"error\":\"git diff failed\"}", true);
        }
    } else {
        root_path = get_project_root(srv, effective_project);
    }

    symbol_match_t *affected_symbols = NULL;
    int affected_symbol_count = 0;
    route_risk_t *affected_routes = NULL;
    int affected_route_count = 0;
    file_context_related_t *related = NULL;
    int related_count = 0;
    test_recommendation_t *tests = NULL;
    int test_count = 0;
    int indexed_target_count = 0;
    int missing_target_count = 0;
    int inbound_calls_total = 0;
    int outbound_calls_total = 0;
    int cochange_total = 0;
    bool *path_indexed = path_count > 0 ? calloc((size_t)path_count, sizeof(*path_indexed)) : NULL;

    for (int p = 0; p < path_count; p++) {
        const char *target_path = paths[p];
        cbm_node_t *nodes = NULL;
        int node_count = 0;
        cbm_store_find_nodes_by_file(store, effective_project, target_path, &nodes, &node_count);
        if (node_count == 0) {
            missing_target_count++;
            cbm_store_free_nodes(nodes, node_count);
            continue;
        }

        indexed_target_count++;
        if (path_indexed) {
            path_indexed[p] = true;
        }
        cbm_node_t *file_node = NULL;
        for (int i = 0; i < node_count; i++) {
            cbm_node_t *node = &nodes[i];
            if (node->label && strcmp(node->label, "File") == 0) {
                file_node = node;
                continue;
            }
            if (include_routes && node->label && strcmp(node->label, "Route") == 0) {
                add_route_risk(&affected_routes, &affected_route_count, node, 8);
                continue;
            }
            if (is_meta_label(node->label)) {
                continue;
            }

            int in_deg = 0;
            int out_deg = 0;
            cbm_store_node_degree(store, node->id, &in_deg, &out_deg);
            inbound_calls_total += in_deg;
            outbound_calls_total += out_deg;
            add_symbol_match(&affected_symbols, &affected_symbol_count, node,
                             2 + (in_deg > out_deg ? in_deg : out_deg));

            cbm_edge_t *in_edges = NULL;
            int in_edge_count = 0;
            if (cbm_store_find_edges_by_target_type(store, node->id, "CALLS", &in_edges, &in_edge_count) ==
                CBM_STORE_OK) {
                for (int e = 0; e < in_edge_count; e++) {
                    cbm_node_t src = {0};
                    if (cbm_store_find_node_by_id(store, in_edges[e].source_id, &src) == CBM_STORE_OK) {
                        add_related_file(&related, &related_count, src.file_path, target_path, 1, 0, 0, 0, 0);
                        free_node_contents(&src);
                    }
                }
            }
            cbm_store_free_edges(in_edges, in_edge_count);

            cbm_edge_t *out_edges = NULL;
            int out_edge_count = 0;
            if (cbm_store_find_edges_by_source_type(store, node->id, "CALLS", &out_edges,
                                                    &out_edge_count) == CBM_STORE_OK) {
                for (int e = 0; e < out_edge_count; e++) {
                    cbm_node_t tgt = {0};
                    if (cbm_store_find_node_by_id(store, out_edges[e].target_id, &tgt) == CBM_STORE_OK) {
                        add_related_file(&related, &related_count, tgt.file_path, target_path, 0, 1, 0, 0, 0);
                        free_node_contents(&tgt);
                    }
                }
            }
            cbm_store_free_edges(out_edges, out_edge_count);

            if (include_tests) {
                cbm_edge_t *test_edges = NULL;
                int test_edge_count = 0;
                if (cbm_store_find_edges_by_target_type(store, node->id, "TESTS", &test_edges,
                                                        &test_edge_count) == CBM_STORE_OK) {
                    for (int e = 0; e < test_edge_count; e++) {
                        cbm_node_t src = {0};
                        if (cbm_store_find_node_by_id(store, test_edges[e].source_id, &src) == CBM_STORE_OK) {
                            add_test_recommendation(&tests, &test_count, src.file_path, target_path, 1, 0);
                            add_related_file(&related, &related_count, src.file_path, target_path, 0, 0, 0, 1, 0);
                            free_node_contents(&src);
                        }
                    }
                }
                cbm_store_free_edges(test_edges, test_edge_count);
            }

            if (include_routes) {
                cbm_edge_t *handles_edges = NULL;
                int handles_edge_count = 0;
                if (cbm_store_find_edges_by_source_type(store, node->id, "HANDLES", &handles_edges,
                                                        &handles_edge_count) == CBM_STORE_OK) {
                    for (int e = 0; e < handles_edge_count; e++) {
                        cbm_node_t route_node = {0};
                        if (cbm_store_find_node_by_id(store, handles_edges[e].target_id, &route_node) ==
                            CBM_STORE_OK) {
                            add_route_risk(&affected_routes, &affected_route_count, &route_node, 9);
                            free_node_contents(&route_node);
                        }
                    }
                }
                cbm_store_free_edges(handles_edges, handles_edge_count);
            }
        }

        if (file_node) {
            cbm_edge_t *import_out = NULL;
            int import_out_count = 0;
            if (cbm_store_find_edges_by_source_type(store, file_node->id, "IMPORTS", &import_out,
                                                    &import_out_count) == CBM_STORE_OK) {
                for (int e = 0; e < import_out_count; e++) {
                    cbm_node_t tgt = {0};
                    if (cbm_store_find_node_by_id(store, import_out[e].target_id, &tgt) == CBM_STORE_OK) {
                        add_related_file(&related, &related_count, tgt.file_path, target_path, 0, 0, 1, 0, 0);
                        free_node_contents(&tgt);
                    }
                }
            }
            cbm_store_free_edges(import_out, import_out_count);

            cbm_edge_t *import_in = NULL;
            int import_in_count = 0;
            if (cbm_store_find_edges_by_target_type(store, file_node->id, "IMPORTS", &import_in,
                                                    &import_in_count) == CBM_STORE_OK) {
                for (int e = 0; e < import_in_count; e++) {
                    cbm_node_t src = {0};
                    if (cbm_store_find_node_by_id(store, import_in[e].source_id, &src) == CBM_STORE_OK) {
                        add_related_file(&related, &related_count, src.file_path, target_path, 0, 0, 1, 0, 0);
                        free_node_contents(&src);
                    }
                }
            }
            cbm_store_free_edges(import_in, import_in_count);

            if (include_tests) {
                cbm_edge_t *tests_file = NULL;
                int tests_file_count = 0;
                if (cbm_store_find_edges_by_target_type(store, file_node->id, "TESTS_FILE", &tests_file,
                                                        &tests_file_count) == CBM_STORE_OK) {
                    for (int e = 0; e < tests_file_count; e++) {
                        cbm_node_t src = {0};
                        if (cbm_store_find_node_by_id(store, tests_file[e].source_id, &src) == CBM_STORE_OK) {
                            add_test_recommendation(&tests, &test_count, src.file_path, target_path, 0, 1);
                            add_related_file(&related, &related_count, src.file_path, target_path, 0, 0, 0, 1, 0);
                            free_node_contents(&src);
                        }
                    }
                }
                cbm_store_free_edges(tests_file, tests_file_count);
            }

            cbm_edge_t *cochange_out = NULL;
            int cochange_out_count = 0;
            if (cbm_store_find_edges_by_source_type(store, file_node->id, "FILE_CHANGES_WITH",
                                                    &cochange_out, &cochange_out_count) == CBM_STORE_OK) {
                for (int e = 0; e < cochange_out_count; e++) {
                    cbm_node_t tgt = {0};
                    if (cbm_store_find_node_by_id(store, cochange_out[e].target_id, &tgt) == CBM_STORE_OK) {
                        add_related_file(&related, &related_count, tgt.file_path, target_path, 0, 0, 0, 0, 1);
                        cochange_total++;
                        free_node_contents(&tgt);
                    }
                }
            }
            cbm_store_free_edges(cochange_out, cochange_out_count);
        }

        cbm_store_free_nodes(nodes, node_count);
    }

    if (affected_symbol_count > 1) {
        qsort(affected_symbols, (size_t)affected_symbol_count, sizeof(*affected_symbols),
              compare_symbol_matches_desc);
    }
    if (affected_route_count > 1) {
        qsort(affected_routes, (size_t)affected_route_count, sizeof(*affected_routes),
              compare_route_risks_desc);
    }
    if (related_count > 1) {
        qsort(related, (size_t)related_count, sizeof(*related), compare_related_files_desc);
    }
    if (test_count > 1) {
        qsort(tests, (size_t)test_count, sizeof(*tests), compare_test_recommendations_desc);
    }

    const char *risk_level = risk_level_for_context(
        inbound_calls_total + (affected_route_count > 0 ? 2 : 0) + (path_count > 1 ? 1 : 0),
        related_count + (cochange_total > 0 ? 1 : 0), include_tests ? test_count : 0);
    if ((affected_route_count > 0 && path_count > 1) ||
        (include_tests && test_count == 0 && inbound_calls_total > 0) || path_count >= 3) {
        risk_level = "high";
    }
    const char *confidence =
        indexed_target_count == path_count && path_count > 0
            ? "high"
            : (indexed_target_count > 0 || affected_symbol_count > 0 ? "medium" : "low");

    double blast_radius_score = normalize_score(
        inbound_calls_total + outbound_calls_total + related_count + affected_route_count + path_count, 20);

    char summary[512];
    snprintf(summary, sizeof(summary),
             "Analyzed %d target%s (%d indexed, %d missing) with %d affected symbol%s, %d related "
             "file%s, %d test%s, and %d route%s in blast radius.",
             path_count, path_count == 1 ? "" : "s", indexed_target_count, missing_target_count,
             affected_symbol_count,
             affected_symbol_count == 1 ? "" : "s", related_count, related_count == 1 ? "" : "s",
             test_count, test_count == 1 ? "" : "s", affected_route_count,
             affected_route_count == 1 ? "" : "s");

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_str(doc, root, "tool_version", "1.0");
    yyjson_mut_val *project_obj = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, project_obj, "id", effective_project);
    yyjson_mut_obj_add_str(doc, project_obj, "root_path", root_path ? root_path : "");
    yyjson_mut_obj_add_val(doc, root, "project", project_obj);

    yyjson_mut_val *query_obj = yyjson_mut_obj(doc);
    yyjson_mut_val *query_paths = yyjson_mut_arr(doc);
    for (int i = 0; i < path_count; i++) {
        yyjson_mut_arr_add_str(doc, query_paths, paths[i]);
    }
    yyjson_mut_obj_add_val(doc, query_obj, "paths", query_paths);
    if (diff_mode) {
        yyjson_mut_obj_add_str(doc, query_obj, "diff_mode", diff_mode);
    }
    yyjson_mut_obj_add_bool(doc, query_obj, "include_tests", include_tests);
    yyjson_mut_obj_add_bool(doc, query_obj, "include_routes", include_routes);
    yyjson_mut_obj_add_int(doc, query_obj, "max_related_files", max_related_files);
    yyjson_mut_obj_add_int(doc, query_obj, "max_tests", max_tests);
    yyjson_mut_obj_add_val(doc, root, "query", query_obj);

    yyjson_mut_obj_add_str(doc, root, "summary", summary);
    yyjson_mut_obj_add_str(doc, root, "risk_level", risk_level);
    yyjson_mut_obj_add_str(doc, root, "confidence", confidence);

    yyjson_mut_val *evidence = yyjson_mut_arr(doc);
    yyjson_mut_val *ev1 = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, ev1, "kind", "call_neighbors");
    yyjson_mut_obj_add_str(doc, ev1, "message",
                           "Blast radius used CALLS traversal and file-level IMPORTS relationships.");
    yyjson_mut_obj_add_real(doc, ev1, "weight",
                            normalize_score(inbound_calls_total + outbound_calls_total + related_count, 16));
    yyjson_mut_obj_add_str(doc, ev1, "source", "graph");
    yyjson_mut_arr_add_val(evidence, ev1);
    if (include_tests) {
        yyjson_mut_val *ev2 = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, ev2, "kind", "test_edges");
        yyjson_mut_obj_add_str(doc, ev2, "message",
                               "Related tests were derived from TESTS and TESTS_FILE edges.");
        yyjson_mut_obj_add_real(doc, ev2, "weight", normalize_score(test_count, 8));
        yyjson_mut_obj_add_str(doc, ev2, "source", "graph");
        yyjson_mut_arr_add_val(evidence, ev2);
    }
    if (cochange_total > 0) {
        yyjson_mut_val *ev3 = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, ev3, "kind", "historical_cochange");
        yyjson_mut_obj_add_str(doc, ev3, "message",
                               "FILE_CHANGES_WITH edges increased the estimated blast radius.");
        yyjson_mut_obj_add_real(doc, ev3, "weight", normalize_score(cochange_total, 6));
        yyjson_mut_obj_add_str(doc, ev3, "source", "graph");
        yyjson_mut_arr_add_val(evidence, ev3);
    }
    yyjson_mut_obj_add_val(doc, root, "evidence", evidence);

    yyjson_mut_val *next_actions = yyjson_mut_arr(doc);
    yyjson_mut_arr_add_str(doc, next_actions, "Review the highest-ranked related files before broad edits.");
    if (include_tests) {
        yyjson_mut_arr_add_str(doc, next_actions, "Run recommended tests before finalizing the patch.");
    }
    if (affected_route_count > 0) {
        yyjson_mut_arr_add_str(doc, next_actions,
                               "Verify route and response contract compatibility before merge.");
    }
    yyjson_mut_obj_add_val(doc, root, "next_actions", next_actions);

    yyjson_mut_val *warnings = yyjson_mut_arr(doc);
    if (missing_target_count > 0) {
        char warning_buf[160];
        snprintf(warning_buf, sizeof(warning_buf),
                 "%d requested path%s were not found in the indexed project.",
                 missing_target_count, missing_target_count == 1 ? "" : "s");
        yyjson_mut_arr_add_strcpy(doc, warnings, warning_buf);
    }
    if (path_count == 0 && diff_mode) {
        yyjson_mut_arr_add_str(doc, warnings, "No changed files were detected for the requested diff mode.");
    }
    if (include_tests && test_count == 0) {
        yyjson_mut_arr_add_str(doc, warnings, "No direct tests were found for the changed files.");
    }
    yyjson_mut_obj_add_val(doc, root, "warnings", warnings);

    yyjson_mut_val *data = yyjson_mut_obj(doc);
    yyjson_mut_val *targets = yyjson_mut_arr(doc);
    yyjson_mut_val *indexed_targets = yyjson_mut_arr(doc);
    yyjson_mut_val *missing_targets = yyjson_mut_arr(doc);
    for (int i = 0; i < path_count; i++) {
        yyjson_mut_arr_add_str(doc, targets, paths[i]);
        if (path_indexed && path_indexed[i]) {
            yyjson_mut_arr_add_str(doc, indexed_targets, paths[i]);
        } else {
            yyjson_mut_arr_add_str(doc, missing_targets, paths[i]);
        }
    }
    yyjson_mut_obj_add_val(doc, data, "targets", targets);
    yyjson_mut_obj_add_val(doc, data, "indexed_targets", indexed_targets);
    yyjson_mut_obj_add_val(doc, data, "missing_targets", missing_targets);

    yyjson_mut_val *symbols_arr = yyjson_mut_arr(doc);
    for (int i = 0; i < affected_symbol_count && i < 10; i++) {
        yyjson_mut_val *item = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, item, "qualified_name", affected_symbols[i].qualified_name);
        yyjson_mut_obj_add_str(doc, item, "path", affected_symbols[i].path ? affected_symbols[i].path : "");
        yyjson_mut_obj_add_real(doc, item, "risk_score", normalize_score(affected_symbols[i].score, 10));
        yyjson_mut_arr_add_val(symbols_arr, item);
    }
    yyjson_mut_obj_add_val(doc, data, "affected_symbols", symbols_arr);

    yyjson_mut_val *routes_arr = yyjson_mut_arr(doc);
    if (include_routes) {
        for (int i = 0; i < affected_route_count && i < 10; i++) {
            yyjson_mut_val *item = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, item, "method", affected_routes[i].method ? affected_routes[i].method : "");
            yyjson_mut_obj_add_str(doc, item, "path", affected_routes[i].path ? affected_routes[i].path : "");
            yyjson_mut_obj_add_real(doc, item, "score", normalize_score(affected_routes[i].score, 10));
            yyjson_mut_arr_add_val(routes_arr, item);
        }
    }
    yyjson_mut_obj_add_val(doc, data, "affected_routes", routes_arr);

    yyjson_mut_val *tests_arr = yyjson_mut_arr(doc);
    if (include_tests) {
        for (int i = 0; i < test_count && i < max_tests; i++) {
            yyjson_mut_val *item = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, item, "path", tests[i].path);
            yyjson_mut_obj_add_real(doc, item, "score", normalize_score(tests[i].score, 10));
            yyjson_mut_obj_add_str(doc, item, "tier", detect_test_tier(tests[i].path));
            if (tests[i].direct_tests > 0 && tests[i].file_tests > 0) {
                yyjson_mut_obj_add_str(doc, item, "reason", "Direct TESTS and TESTS_FILE edges");
            } else if (tests[i].direct_tests > 0) {
                yyjson_mut_obj_add_str(doc, item, "reason", "Direct TESTS edge");
            } else {
                yyjson_mut_obj_add_str(doc, item, "reason", "Direct TESTS_FILE edge");
            }
            yyjson_mut_arr_add_val(tests_arr, item);
        }
    }
    yyjson_mut_obj_add_val(doc, data, "related_tests", tests_arr);
    if (include_tests) {
        append_test_command_hints(doc, data, tests, test_count, max_tests);
    } else {
        yyjson_mut_obj_add_val(doc, data, "test_command_hints", yyjson_mut_arr(doc));
    }

    yyjson_mut_val *related_arr = yyjson_mut_arr(doc);
    for (int i = 0; i < related_count && i < max_related_files; i++) {
        yyjson_mut_val *item = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, item, "path", related[i].path ? related[i].path : "");
        yyjson_mut_obj_add_real(doc, item, "score", normalize_score(related[i].score, 10));
        yyjson_mut_obj_add_real(doc, item, "priority_score",
                                normalize_score(related_file_priority_score(&related[i]), 12));
        yyjson_mut_obj_add_real(doc, item, "blast_radius_score",
                                normalize_score(related[i].callers + related[i].callees + related[i].imports +
                                                    related[i].tests + related[i].cochanges,
                                                8));
        yyjson_mut_obj_add_str(doc, item, "primary_relationship",
                               related_file_primary_relationship(&related[i]));
        append_related_file_relationship_types(doc, item, &related[i]);
        append_related_file_reason(doc, item, &related[i]);
        yyjson_mut_arr_add_val(related_arr, item);
    }
    yyjson_mut_obj_add_val(doc, data, "related_files", related_arr);

    yyjson_mut_val *risk_factors = yyjson_mut_arr(doc);
    if (path_count > 1) {
        yyjson_mut_val *item = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, item, "kind", "multi_file_change");
        yyjson_mut_obj_add_str(doc, item, "message", "Multiple files are part of the same change.");
        yyjson_mut_obj_add_real(doc, item, "weight", normalize_score(path_count, 4));
        yyjson_mut_arr_add_val(risk_factors, item);
    }
    if (inbound_calls_total > 0) {
        yyjson_mut_val *item = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, item, "kind", "call_surface");
        yyjson_mut_obj_add_str(doc, item, "message", "Changed symbols have inbound callers in the graph.");
        yyjson_mut_obj_add_real(doc, item, "weight", normalize_score(inbound_calls_total, 10));
        yyjson_mut_arr_add_val(risk_factors, item);
    }
    if (affected_route_count > 0) {
        yyjson_mut_val *item = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, item, "kind", "route_surface_changed");
        yyjson_mut_obj_add_str(doc, item, "message", "At least one HTTP route is directly affected.");
        yyjson_mut_obj_add_real(doc, item, "weight", normalize_score(affected_route_count, 4));
        yyjson_mut_arr_add_val(risk_factors, item);
    }
    if (affected_route_count > 0 && path_count > 1) {
        yyjson_mut_val *item = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, item, "kind", "route_and_service_changed");
        yyjson_mut_obj_add_str(doc, item, "message", "Both route and service layers are touched.");
        yyjson_mut_obj_add_real(doc, item, "weight",
                                normalize_score(affected_route_count + path_count, 6));
        yyjson_mut_arr_add_val(risk_factors, item);
    }
    if (cochange_total > 0) {
        yyjson_mut_val *item = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, item, "kind", "historical_cochange");
        yyjson_mut_obj_add_str(doc, item, "message", "Files in the blast radius changed together before.");
        yyjson_mut_obj_add_real(doc, item, "weight", normalize_score(cochange_total, 6));
        yyjson_mut_arr_add_val(risk_factors, item);
    }
    if (missing_target_count > 0) {
        yyjson_mut_val *item = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, item, "kind", "partial_graph_coverage");
        yyjson_mut_obj_add_str(doc, item, "message",
                               "Some changed files are outside current graph coverage.");
        yyjson_mut_obj_add_real(doc, item, "weight", normalize_score(missing_target_count, path_count));
        yyjson_mut_arr_add_val(risk_factors, item);
    }
    if (include_tests && test_count == 0) {
        yyjson_mut_val *item = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, item, "kind", "test_gap");
        yyjson_mut_obj_add_str(doc, item, "message", "No direct tests were linked to the changed files.");
        yyjson_mut_obj_add_real(doc, item, "weight", 0.75);
        yyjson_mut_arr_add_val(risk_factors, item);
    }
    yyjson_mut_obj_add_val(doc, data, "risk_factors", risk_factors);

    yyjson_mut_val *safe_steps = yyjson_mut_arr(doc);
    yyjson_mut_arr_add_str(doc, safe_steps, "Inspect the affected symbols before broad refactors.");
    if (include_tests && test_count > 0) {
        yyjson_mut_arr_add_str(doc, safe_steps, "Run the highest-ranked related tests first.");
    }
    if (related_count > 0) {
        yyjson_mut_arr_add_str(doc, safe_steps, "Review the top related files for hidden coupling.");
    }
    if (affected_route_count > 0) {
        yyjson_mut_arr_add_str(doc, safe_steps, "Double-check route and schema compatibility.");
    }
    if (missing_target_count > 0) {
        yyjson_mut_arr_add_str(doc, safe_steps,
                               "Search code for missing targets before relying on graph-only impact analysis.");
    }
    yyjson_mut_obj_add_val(doc, data, "safe_next_steps", safe_steps);

    yyjson_mut_val *pitfalls = yyjson_mut_arr(doc);
    if (missing_target_count > 0) {
        yyjson_mut_arr_add_str(doc, pitfalls,
                               "Graph coverage is partial; blast radius may miss unindexed files.");
    }
    if (affected_route_count > 0 && path_count > 1) {
        yyjson_mut_arr_add_str(doc, pitfalls,
                               "Route-layer and multi-file edits raise contract-regression risk.");
    }
    if (include_tests && test_count == 0) {
        yyjson_mut_arr_add_str(doc, pitfalls,
                               "No direct test edges were found; validate with broader suite coverage.");
    }
    if (cochange_total > 0) {
        yyjson_mut_arr_add_str(doc, pitfalls,
                               "Historical co-change suggests hidden coupling beyond direct imports/calls.");
    }
    yyjson_mut_obj_add_val(doc, data, "pitfalls", pitfalls);

    yyjson_mut_obj_add_real(doc, data, "blast_radius_score", blast_radius_score);
    yyjson_mut_obj_add_val(doc, root, "data", data);

    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);

    free(single_path);
    free(diff_mode);
    free(project);
    free(root_path);
    free(path_indexed);
    free_string_array(paths, path_count);
    free_symbol_matches(affected_symbols, affected_symbol_count);
    free_route_risks(affected_routes, affected_route_count);
    free_related_files(related, related_count);
    free_test_recommendations(tests, test_count);

    char *result = cbm_mcp_text_result(json, false);
    free(json);
    return result;
}

static char *handle_get_edit_plan(cbm_mcp_server_t *srv, const char *args) {
    char *path = cbm_mcp_get_string_arg(args, "path");
    char *project = cbm_mcp_get_string_arg(args, "project");
    char *mode = cbm_mcp_get_string_arg(args, "mode");
    char *task_type = cbm_mcp_get_string_arg(args, "task_type");
    bool include_routes = true;
    bool include_snippets = false;
    int max_related_files = cbm_mcp_get_int_arg(args, "max_related_files", 6);
    int max_tests = cbm_mcp_get_int_arg(args, "max_tests", 5);
    bool compact_mode = false;

    if (strstr(args, "\"include_routes\"")) {
        include_routes = cbm_mcp_get_bool_arg(args, "include_routes");
    }
    if (strstr(args, "\"include_snippets\"")) {
        include_snippets = cbm_mcp_get_bool_arg(args, "include_snippets");
    }
    if (!path || !path[0]) {
        free(path);
        free(project);
        free(mode);
        free(task_type);
        return cbm_mcp_text_result("path is required", true);
    }
    if (!mode || !mode[0]) {
        free(mode);
        mode = heap_strdup("detailed");
    }
    if (strcmp(mode, "compact") == 0) {
        compact_mode = true;
    } else if (strcmp(mode, "detailed") != 0) {
        free(path);
        free(project);
        free(mode);
        free(task_type);
        return cbm_mcp_text_result("mode must be compact or detailed", true);
    }
    if (!task_type || !task_type[0]) {
        free(task_type);
        task_type = heap_strdup("fix");
    } else if (strcmp(task_type, "fix") != 0 && strcmp(task_type, "refactor") != 0 &&
               strcmp(task_type, "investigate") != 0) {
        free(path);
        free(project);
        free(mode);
        free(task_type);
        return cbm_mcp_text_result("task_type must be fix, refactor, or investigate", true);
    }
    if (max_related_files <= 0) {
        max_related_files = 6;
    }
    if (max_tests <= 0) {
        max_tests = 5;
    }

    yyjson_mut_doc *file_args_doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *file_args = yyjson_mut_obj(file_args_doc);
    yyjson_mut_doc_set_root(file_args_doc, file_args);
    yyjson_mut_obj_add_str(file_args_doc, file_args, "path", path);
    if (project && project[0]) {
        yyjson_mut_obj_add_str(file_args_doc, file_args, "project", project);
    }
    yyjson_mut_obj_add_int(file_args_doc, file_args, "max_related_files", max_related_files);
    yyjson_mut_obj_add_int(file_args_doc, file_args, "max_tests", max_tests);
    yyjson_mut_obj_add_bool(file_args_doc, file_args, "include_callers", true);
    yyjson_mut_obj_add_bool(file_args_doc, file_args, "include_snippets", include_snippets);
    char *file_args_json = yy_doc_to_str(file_args_doc);
    yyjson_mut_doc_free(file_args_doc);

    yyjson_mut_doc *risk_args_doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *risk_args = yyjson_mut_obj(risk_args_doc);
    yyjson_mut_doc_set_root(risk_args_doc, risk_args);
    yyjson_mut_obj_add_str(risk_args_doc, risk_args, "path", path);
    if (project && project[0]) {
        yyjson_mut_obj_add_str(risk_args_doc, risk_args, "project", project);
    }
    yyjson_mut_obj_add_bool(risk_args_doc, risk_args, "include_tests", true);
    yyjson_mut_obj_add_bool(risk_args_doc, risk_args, "include_routes", include_routes);
    yyjson_mut_obj_add_int(risk_args_doc, risk_args, "max_related_files", max_related_files);
    yyjson_mut_obj_add_int(risk_args_doc, risk_args, "max_tests", max_tests);
    char *risk_args_json = yy_doc_to_str(risk_args_doc);
    yyjson_mut_doc_free(risk_args_doc);

    char *file_result = handle_get_file_context(srv, file_args_json ? file_args_json : "{}");
    char *risk_result = handle_get_change_risks(srv, risk_args_json ? risk_args_json : "{}");
    free(file_args_json);
    free(risk_args_json);

    yyjson_doc *file_doc = parse_mcp_text_payload_doc(file_result);
    yyjson_doc *risk_doc = parse_mcp_text_payload_doc(risk_result);
    free(file_result);
    free(risk_result);

    if (!file_doc || !risk_doc) {
        free(path);
        free(project);
        if (file_doc) {
            yyjson_doc_free(file_doc);
        }
        if (risk_doc) {
            yyjson_doc_free(risk_doc);
        }
        return cbm_mcp_text_result("{\"error\":\"failed to compose edit plan\"}", true);
    }

    yyjson_val *file_root = yyjson_doc_get_root(file_doc);
    yyjson_val *risk_root = yyjson_doc_get_root(risk_doc);
    yyjson_val *file_data = yyjson_obj_get(file_root, "data");
    yyjson_val *risk_data = yyjson_obj_get(risk_root, "data");
    yyjson_val *file_project = yyjson_obj_get(file_root, "project");
    yyjson_val *file_summary = yyjson_obj_get(file_root, "summary");
    yyjson_val *risk_summary = yyjson_obj_get(risk_root, "summary");
    yyjson_val *risk_level_val = yyjson_obj_get(risk_root, "risk_level");
    yyjson_val *confidence_val = yyjson_obj_get(risk_root, "confidence");

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "tool_version", "1.0");
    if (file_project) {
        yyjson_mut_obj_add_val(doc, root, "project", yyjson_val_mut_copy(doc, file_project));
    }

    yyjson_mut_val *query = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, query, "path", path);
    if (project && project[0]) {
        yyjson_mut_obj_add_str(doc, query, "project", project);
    }
    yyjson_mut_obj_add_str(doc, query, "mode", mode);
    yyjson_mut_obj_add_str(doc, query, "task_type", task_type);
    yyjson_mut_obj_add_bool(doc, query, "include_routes", include_routes);
    yyjson_mut_obj_add_bool(doc, query, "include_snippets", include_snippets);
    yyjson_mut_obj_add_int(doc, query, "max_related_files", max_related_files);
    yyjson_mut_obj_add_int(doc, query, "max_tests", max_tests);
    yyjson_mut_obj_add_val(doc, root, "query", query);

    char summary[768];
    snprintf(summary, sizeof(summary), "Pre-edit plan for %s. %s %s", path,
             file_summary && yyjson_is_str(file_summary) ? yyjson_get_str(file_summary) : "",
             risk_summary && yyjson_is_str(risk_summary) ? yyjson_get_str(risk_summary) : "");
    yyjson_mut_obj_add_strcpy(doc, root, "summary", summary);
    yyjson_mut_obj_add_str(doc, root, "risk_level",
                           risk_level_val && yyjson_is_str(risk_level_val) ? yyjson_get_str(risk_level_val)
                                                                           : "medium");
    yyjson_mut_obj_add_str(doc, root, "confidence",
                           confidence_val && yyjson_is_str(confidence_val) ? yyjson_get_str(confidence_val)
                                                                           : "medium");

    yyjson_mut_val *warnings = yyjson_mut_arr(doc);
    append_string_array_values(doc, warnings, yyjson_obj_get(file_root, "warnings"));
    append_string_array_values(doc, warnings, yyjson_obj_get(risk_root, "warnings"));
    yyjson_mut_obj_add_val(doc, root, "warnings", warnings);

    yyjson_mut_val *data = yyjson_mut_obj(doc);
    yyjson_mut_val *target = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, target, "path", path);
    if (file_data && yyjson_is_obj(file_data)) {
        yyjson_val *file_obj = yyjson_obj_get(file_data, "file");
        if (file_obj && yyjson_is_obj(file_obj)) {
            yyjson_val *lang = yyjson_obj_get(file_obj, "language");
            if (lang && yyjson_is_str(lang)) {
                yyjson_mut_obj_add_str(doc, target, "language", yyjson_get_str(lang));
            }
        }
    }
    yyjson_mut_obj_add_val(doc, data, "target", target);

    yyjson_mut_val *immediate_steps = yyjson_mut_arr(doc);
    append_edit_plan_steps(doc, immediate_steps, task_type, include_routes);
    yyjson_mut_obj_add_val(doc, data, "immediate_steps", immediate_steps);

    yyjson_mut_val *file_context = yyjson_mut_obj(doc);
    if (file_summary && yyjson_is_str(file_summary)) {
        yyjson_mut_obj_add_str(doc, file_context, "summary", yyjson_get_str(file_summary));
    }
    yyjson_val *file_risk_level = yyjson_obj_get(file_root, "risk_level");
    yyjson_val *file_confidence = yyjson_obj_get(file_root, "confidence");
    if (file_risk_level && yyjson_is_str(file_risk_level)) {
        yyjson_mut_obj_add_str(doc, file_context, "risk_level", yyjson_get_str(file_risk_level));
    }
    if (file_confidence && yyjson_is_str(file_confidence)) {
        yyjson_mut_obj_add_str(doc, file_context, "confidence", yyjson_get_str(file_confidence));
    }
    if (!compact_mode && file_data) {
        yyjson_mut_obj_add_val(doc, file_context, "data", yyjson_val_mut_copy(doc, file_data));
    }
    yyjson_mut_obj_add_val(doc, data, "file_context", file_context);

    yyjson_mut_val *change_risks = yyjson_mut_obj(doc);
    if (risk_summary && yyjson_is_str(risk_summary)) {
        yyjson_mut_obj_add_str(doc, change_risks, "summary", yyjson_get_str(risk_summary));
    }
    if (risk_level_val && yyjson_is_str(risk_level_val)) {
        yyjson_mut_obj_add_str(doc, change_risks, "risk_level", yyjson_get_str(risk_level_val));
    }
    if (confidence_val && yyjson_is_str(confidence_val)) {
        yyjson_mut_obj_add_str(doc, change_risks, "confidence", yyjson_get_str(confidence_val));
    }
    if (!compact_mode && risk_data) {
        yyjson_mut_obj_add_val(doc, change_risks, "data", yyjson_val_mut_copy(doc, risk_data));
    }
    yyjson_mut_obj_add_val(doc, data, "change_risks", change_risks);

    if (compact_mode && risk_data && yyjson_is_obj(risk_data)) {
        yyjson_val *top_related = yyjson_obj_get(risk_data, "related_files");
        yyjson_val *top_tests = yyjson_obj_get(risk_data, "related_tests");
        yyjson_val *risk_factors = yyjson_obj_get(risk_data, "risk_factors");
        yyjson_val *safe_steps = yyjson_obj_get(risk_data, "safe_next_steps");
        yyjson_val *pitfalls = yyjson_obj_get(risk_data, "pitfalls");
        yyjson_val *test_hints = yyjson_obj_get(risk_data, "test_command_hints");

        if (top_related && yyjson_is_arr(top_related)) {
            yyjson_mut_obj_add_val(doc, data, "top_related_files", yyjson_val_mut_copy(doc, top_related));
        }
        if (top_tests && yyjson_is_arr(top_tests)) {
            yyjson_mut_obj_add_val(doc, data, "top_tests", yyjson_val_mut_copy(doc, top_tests));
        }
        if (test_hints && yyjson_is_arr(test_hints)) {
            yyjson_mut_obj_add_val(doc, data, "test_command_hints", yyjson_val_mut_copy(doc, test_hints));
        }
        if (risk_factors && yyjson_is_arr(risk_factors)) {
            yyjson_mut_obj_add_val(doc, data, "risk_factors", yyjson_val_mut_copy(doc, risk_factors));
        }
        if (safe_steps && yyjson_is_arr(safe_steps)) {
            yyjson_mut_obj_add_val(doc, data, "safe_next_steps", yyjson_val_mut_copy(doc, safe_steps));
        }
        if (pitfalls && yyjson_is_arr(pitfalls)) {
            yyjson_mut_obj_add_val(doc, data, "pitfalls", yyjson_val_mut_copy(doc, pitfalls));
        }
    }

    yyjson_mut_obj_add_val(doc, root, "data", data);

    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    yyjson_doc_free(file_doc);
    yyjson_doc_free(risk_doc);
    free(path);
    free(project);
    free(mode);
    free(task_type);

    char *result = cbm_mcp_text_result(json, false);
    free(json);
    return result;
}

static char *handle_search_graph(cbm_mcp_server_t *srv, const char *args) {
    char *project = cbm_mcp_get_string_arg(args, "project");
    cbm_store_t *store = resolve_store(srv, project);
    REQUIRE_STORE(store, project);
    char *label = cbm_mcp_get_string_arg(args, "label");
    char *name_pattern = cbm_mcp_get_string_arg(args, "name_pattern");
    char *file_pattern = cbm_mcp_get_string_arg(args, "file_pattern");
    int limit = cbm_mcp_get_int_arg(args, "limit", 500000);
    int offset = cbm_mcp_get_int_arg(args, "offset", 0);
    int min_degree = cbm_mcp_get_int_arg(args, "min_degree", -1);
    int max_degree = cbm_mcp_get_int_arg(args, "max_degree", -1);

    cbm_search_params_t params = {
        .project = project,
        .label = label,
        .name_pattern = name_pattern,
        .file_pattern = file_pattern,
        .limit = limit,
        .offset = offset,
        .min_degree = min_degree,
        .max_degree = max_degree,
    };

    cbm_search_output_t out = {0};
    cbm_store_search(store, &params, &out);

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_int(doc, root, "total", out.total);

    yyjson_mut_val *results = yyjson_mut_arr(doc);
    for (int i = 0; i < out.count; i++) {
        cbm_search_result_t *sr = &out.results[i];
        yyjson_mut_val *item = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, item, "name", sr->node.name ? sr->node.name : "");
        yyjson_mut_obj_add_str(doc, item, "qualified_name",
                               sr->node.qualified_name ? sr->node.qualified_name : "");
        yyjson_mut_obj_add_str(doc, item, "label", sr->node.label ? sr->node.label : "");
        yyjson_mut_obj_add_str(doc, item, "file_path",
                               sr->node.file_path ? sr->node.file_path : "");
        yyjson_mut_obj_add_int(doc, item, "in_degree", sr->in_degree);
        yyjson_mut_obj_add_int(doc, item, "out_degree", sr->out_degree);
        yyjson_mut_arr_add_val(results, item);
    }
    yyjson_mut_obj_add_val(doc, root, "results", results);
    yyjson_mut_obj_add_bool(doc, root, "has_more", out.total > offset + out.count);

    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    cbm_store_search_free(&out);

    free(project);
    free(label);
    free(name_pattern);
    free(file_pattern);

    char *result = cbm_mcp_text_result(json, false);
    free(json);
    return result;
}

static char *handle_query_graph(cbm_mcp_server_t *srv, const char *args) {
    char *query = cbm_mcp_get_string_arg(args, "query");
    char *project = cbm_mcp_get_string_arg(args, "project");
    cbm_store_t *store = resolve_store(srv, project);
    int max_rows = cbm_mcp_get_int_arg(args, "max_rows", 0);

    if (!query) {
        free(project);
        return cbm_mcp_text_result("query is required", true);
    }
    if (!store) {
        free(project);
        free(query);
        return cbm_mcp_text_result("{\"error\":\"no project loaded\"}", true);
    }

    cbm_cypher_result_t result = {0};
    int rc = cbm_cypher_execute(store, query, project, max_rows, &result);

    if (rc < 0) {
        char *err_msg = result.error ? result.error : "query execution failed";
        char *resp = cbm_mcp_text_result(err_msg, true);
        cbm_cypher_result_free(&result);
        free(query);
        free(project);
        return resp;
    }

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    /* columns */
    yyjson_mut_val *cols = yyjson_mut_arr(doc);
    for (int i = 0; i < result.col_count; i++) {
        yyjson_mut_arr_add_str(doc, cols, result.columns[i]);
    }
    yyjson_mut_obj_add_val(doc, root, "columns", cols);

    /* rows */
    yyjson_mut_val *rows = yyjson_mut_arr(doc);
    for (int r = 0; r < result.row_count; r++) {
        yyjson_mut_val *row = yyjson_mut_arr(doc);
        for (int c = 0; c < result.col_count; c++) {
            yyjson_mut_arr_add_str(doc, row, result.rows[r][c]);
        }
        yyjson_mut_arr_add_val(rows, row);
    }
    yyjson_mut_obj_add_val(doc, root, "rows", rows);
    yyjson_mut_obj_add_int(doc, root, "total", result.row_count);

    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    cbm_cypher_result_free(&result);
    free(query);
    free(project);

    char *res = cbm_mcp_text_result(json, false);
    free(json);
    return res;
}

static char *handle_index_status(cbm_mcp_server_t *srv, const char *args) {
    char *project = cbm_mcp_get_string_arg(args, "project");
    cbm_store_t *store = resolve_store(srv, project);
    REQUIRE_STORE(store, project);

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    if (project) {
        int nodes = cbm_store_count_nodes(store, project);
        int edges = cbm_store_count_edges(store, project);
        yyjson_mut_obj_add_str(doc, root, "project", project);
        yyjson_mut_obj_add_int(doc, root, "nodes", nodes);
        yyjson_mut_obj_add_int(doc, root, "edges", edges);
        yyjson_mut_obj_add_str(doc, root, "status", nodes > 0 ? "ready" : "empty");
    } else {
        yyjson_mut_obj_add_str(doc, root, "status", "no_project");
    }

    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    free(project);

    char *result = cbm_mcp_text_result(json, false);
    free(json);
    return result;
}

/* delete_project: just erase the .db file (and WAL/SHM). */
static char *handle_delete_project(cbm_mcp_server_t *srv, const char *args) {
    char *name = cbm_mcp_get_string_arg(args, "project_name");
    if (!name) {
        return cbm_mcp_text_result("project_name is required", true);
    }

    /* Close store if it's the project being deleted */
    if (srv->current_project && strcmp(srv->current_project, name) == 0) {
        if (srv->owns_store && srv->store) {
            cbm_store_close(srv->store);
            srv->store = NULL;
        }
        free(srv->current_project);
        srv->current_project = NULL;
    }

    /* Delete the .db file + WAL/SHM */
    char path[1024];
    project_db_path(name, path, sizeof(path));

    char wal[1024];
    char shm[1024];
    snprintf(wal, sizeof(wal), "%s-wal", path);
    snprintf(shm, sizeof(shm), "%s-shm", path);

    bool exists = (access(path, F_OK) == 0);
    const char *status = "not_found";
    if (exists) {
        (void)cbm_unlink(path);
        (void)cbm_unlink(wal);
        (void)cbm_unlink(shm);
        status = "deleted";
    }

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "project", name);
    yyjson_mut_obj_add_str(doc, root, "status", status);

    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    free(name);

    char *result = cbm_mcp_text_result(json, false);
    free(json);
    return result;
}

static char *handle_get_architecture(cbm_mcp_server_t *srv, const char *args) {
    char *project = cbm_mcp_get_string_arg(args, "project");
    cbm_store_t *store = resolve_store(srv, project);
    REQUIRE_STORE(store, project);

    cbm_schema_info_t schema = {0};
    cbm_store_get_schema(store, project, &schema);

    int node_count = cbm_store_count_nodes(store, project);
    int edge_count = cbm_store_count_edges(store, project);

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    if (project) {
        yyjson_mut_obj_add_str(doc, root, "project", project);
    }
    yyjson_mut_obj_add_int(doc, root, "total_nodes", node_count);
    yyjson_mut_obj_add_int(doc, root, "total_edges", edge_count);

    /* Node label summary */
    yyjson_mut_val *labels = yyjson_mut_arr(doc);
    for (int i = 0; i < schema.node_label_count; i++) {
        yyjson_mut_val *item = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, item, "label", schema.node_labels[i].label);
        yyjson_mut_obj_add_int(doc, item, "count", schema.node_labels[i].count);
        yyjson_mut_arr_add_val(labels, item);
    }
    yyjson_mut_obj_add_val(doc, root, "node_labels", labels);

    /* Edge type summary */
    yyjson_mut_val *types = yyjson_mut_arr(doc);
    for (int i = 0; i < schema.edge_type_count; i++) {
        yyjson_mut_val *item = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, item, "type", schema.edge_types[i].type);
        yyjson_mut_obj_add_int(doc, item, "count", schema.edge_types[i].count);
        yyjson_mut_arr_add_val(types, item);
    }
    yyjson_mut_obj_add_val(doc, root, "edge_types", types);

    /* Relationship patterns */
    if (schema.rel_pattern_count > 0) {
        yyjson_mut_val *pats = yyjson_mut_arr(doc);
        for (int i = 0; i < schema.rel_pattern_count; i++) {
            yyjson_mut_arr_add_str(doc, pats, schema.rel_patterns[i]);
        }
        yyjson_mut_obj_add_val(doc, root, "relationship_patterns", pats);
    }

    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    cbm_store_schema_free(&schema);
    free(project);

    char *result = cbm_mcp_text_result(json, false);
    free(json);
    return result;
}

static char *handle_trace_call_path(cbm_mcp_server_t *srv, const char *args) {
    char *func_name = cbm_mcp_get_string_arg(args, "function_name");
    char *project = cbm_mcp_get_string_arg(args, "project");
    cbm_store_t *store = resolve_store(srv, project);
    char *direction = cbm_mcp_get_string_arg(args, "direction");
    int depth = cbm_mcp_get_int_arg(args, "depth", 3);

    if (!func_name) {
        free(project);
        free(direction);
        return cbm_mcp_text_result("function_name is required", true);
    }
    if (!store) {
        free(func_name);
        free(project);
        free(direction);
        return cbm_mcp_text_result("{\"error\":\"no project loaded\"}", true);
    }
    if (!direction) {
        direction = heap_strdup("both");
    }

    /* Find the node by name */
    cbm_node_t *nodes = NULL;
    int node_count = 0;
    cbm_store_find_nodes_by_name(store, project, func_name, &nodes, &node_count);

    if (node_count == 0) {
        free(func_name);
        free(project);
        free(direction);
        cbm_store_free_nodes(nodes, 0);
        return cbm_mcp_text_result("{\"error\":\"function not found\"}", true);
    }

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_str(doc, root, "function", func_name);
    yyjson_mut_obj_add_str(doc, root, "direction", direction);

    const char *edge_types[] = {"CALLS"};
    int edge_type_count = 1;

    /* Run BFS for each requested direction.
     * IMPORTANT: yyjson_mut_obj_add_str borrows pointers — we must keep
     * traversal results alive until after yy_doc_to_str serialization. */
    // NOLINTNEXTLINE(readability-implicit-bool-conversion)
    bool do_outbound = strcmp(direction, "outbound") == 0 || strcmp(direction, "both") == 0;
    // NOLINTNEXTLINE(readability-implicit-bool-conversion)
    bool do_inbound = strcmp(direction, "inbound") == 0 || strcmp(direction, "both") == 0;

    cbm_traverse_result_t tr_out = {0};
    cbm_traverse_result_t tr_in = {0};

    if (do_outbound) {
        cbm_store_bfs(store, nodes[0].id, "outbound", edge_types, edge_type_count, depth, 100,
                      &tr_out);

        yyjson_mut_val *callees = yyjson_mut_arr(doc);
        for (int i = 0; i < tr_out.visited_count; i++) {
            yyjson_mut_val *item = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, item, "name",
                                   tr_out.visited[i].node.name ? tr_out.visited[i].node.name : "");
            yyjson_mut_obj_add_str(
                doc, item, "qualified_name",
                tr_out.visited[i].node.qualified_name ? tr_out.visited[i].node.qualified_name : "");
            yyjson_mut_obj_add_int(doc, item, "hop", tr_out.visited[i].hop);
            yyjson_mut_arr_add_val(callees, item);
        }
        yyjson_mut_obj_add_val(doc, root, "callees", callees);
    }

    if (do_inbound) {
        cbm_store_bfs(store, nodes[0].id, "inbound", edge_types, edge_type_count, depth, 100,
                      &tr_in);

        yyjson_mut_val *callers = yyjson_mut_arr(doc);
        for (int i = 0; i < tr_in.visited_count; i++) {
            yyjson_mut_val *item = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, item, "name",
                                   tr_in.visited[i].node.name ? tr_in.visited[i].node.name : "");
            yyjson_mut_obj_add_str(
                doc, item, "qualified_name",
                tr_in.visited[i].node.qualified_name ? tr_in.visited[i].node.qualified_name : "");
            yyjson_mut_obj_add_int(doc, item, "hop", tr_in.visited[i].hop);
            yyjson_mut_arr_add_val(callers, item);
        }
        yyjson_mut_obj_add_val(doc, root, "callers", callers);
    }

    /* Serialize BEFORE freeing traversal results (yyjson borrows strings) */
    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);

    /* Now safe to free traversal data */
    if (do_outbound) {
        cbm_store_traverse_free(&tr_out);
    }
    if (do_inbound) {
        cbm_store_traverse_free(&tr_in);
    }

    cbm_store_free_nodes(nodes, node_count);
    free(func_name);
    free(project);
    free(direction);

    char *result = cbm_mcp_text_result(json, false);
    free(json);
    return result;
}

/* ── Helper: free heap fields of a stack-allocated node ────────── */

static void free_node_contents(cbm_node_t *n) {
    free((void *)n->project);
    free((void *)n->label);
    free((void *)n->name);
    free((void *)n->qualified_name);
    free((void *)n->file_path);
    free((void *)n->properties_json);
    memset(n, 0, sizeof(*n));
}

/* ── Helper: read lines [start, end] from a file ─────────────── */

static char *read_file_lines(const char *path, int start, int end) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return NULL;
    }

    size_t cap = 4096;
    char *buf = malloc(cap);
    size_t len = 0;
    buf[0] = '\0';

    char line[2048];
    int lineno = 0;
    while (fgets(line, sizeof(line), fp)) {
        lineno++;
        if (lineno < start) {
            continue;
        }
        if (lineno > end) {
            break;
        }
        size_t ll = strlen(line);
        while (len + ll + 1 > cap) {
            cap *= 2;
            buf = safe_realloc(buf, cap);
        }
        memcpy(buf + len, line, ll);
        len += ll;
        buf[len] = '\0';
    }

    (void)fclose(fp);
    if (len == 0) {
        free(buf);
        return NULL;
    }
    return buf;
}

/* ── Helper: get project root_path from store ─────────────────── */

static char *get_project_root(cbm_mcp_server_t *srv, const char *project) {
    if (!project) {
        return NULL;
    }
    cbm_store_t *store = resolve_store(srv, project);
    if (!store) {
        return NULL;
    }
    cbm_project_t proj = {0};
    if (cbm_store_get_project(store, project, &proj) != CBM_STORE_OK) {
        return NULL;
    }
    char *root = heap_strdup(proj.root_path);
    free((void *)proj.name);
    free((void *)proj.indexed_at);
    free((void *)proj.root_path);
    return root;
}

/* ── index_repository ─────────────────────────────────────────── */

static char *handle_index_repository(cbm_mcp_server_t *srv, const char *args) {
    char *repo_path = cbm_mcp_get_string_arg(args, "repo_path");
    char *mode_str = cbm_mcp_get_string_arg(args, "mode");

    if (!repo_path) {
        free(mode_str);
        return cbm_mcp_text_result("repo_path is required", true);
    }

    cbm_index_mode_t mode = CBM_MODE_FULL;
    if (mode_str && strcmp(mode_str, "fast") == 0) {
        mode = CBM_MODE_FAST;
    }
    free(mode_str);

    cbm_pipeline_t *p = cbm_pipeline_new(repo_path, NULL, mode);
    if (!p) {
        free(repo_path);
        return cbm_mcp_text_result("failed to create pipeline", true);
    }

    char *project_name = heap_strdup(cbm_pipeline_project_name(p));

    /* Pipeline builds everything in-memory, then dumps to file atomically.
     * No need to close srv->store — pipeline doesn't touch the open store. */
    int rc = cbm_pipeline_run(p);
    cbm_pipeline_free(p);
    cbm_mem_collect(); /* return mimalloc pages to OS after large indexing */

    /* Invalidate cached store so next query reopens the fresh database */
    if (srv->owns_store && srv->store) {
        cbm_store_close(srv->store);
        srv->store = NULL;
    }
    free(srv->current_project);
    srv->current_project = NULL;

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_str(doc, root, "project", project_name);
    yyjson_mut_obj_add_str(doc, root, "status", rc == 0 ? "indexed" : "error");

    if (rc == 0) {
        cbm_store_t *store = resolve_store(srv, project_name);
        if (store) {
            int nodes = cbm_store_count_nodes(store, project_name);
            int edges = cbm_store_count_edges(store, project_name);
            yyjson_mut_obj_add_int(doc, root, "nodes", nodes);
            yyjson_mut_obj_add_int(doc, root, "edges", edges);

            /* Check ADR presence and suggest creation if missing */
            char adr_path[4096];
            snprintf(adr_path, sizeof(adr_path), "%s/.codebase-memory/adr.md", repo_path);
            struct stat adr_st;
            // NOLINTNEXTLINE(readability-implicit-bool-conversion)
            bool adr_exists = (stat(adr_path, &adr_st) == 0);
            yyjson_mut_obj_add_bool(doc, root, "adr_present", adr_exists);
            if (!adr_exists) {
                yyjson_mut_obj_add_str(
                    doc, root, "adr_hint",
                    "Project indexed. Consider creating an Architecture Decision Record: "
                    "explore the codebase with get_architecture(aspects=['all']), then use "
                    "manage_adr(mode='store') to persist architectural insights across sessions.");
            }
        }
    }

    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    free(project_name);
    free(repo_path);

    char *result = cbm_mcp_text_result(json, rc != 0);
    free(json);
    return result;
}

/* ── get_code_snippet ─────────────────────────────────────────── */

/* Copy a node from an array into a heap-allocated standalone node. */
static void copy_node(const cbm_node_t *src, cbm_node_t *dst) {
    dst->id = src->id;
    dst->project = heap_strdup(src->project);
    dst->label = heap_strdup(src->label);
    dst->name = heap_strdup(src->name);
    dst->qualified_name = heap_strdup(src->qualified_name);
    dst->file_path = heap_strdup(src->file_path);
    dst->start_line = src->start_line;
    dst->end_line = src->end_line;
    dst->properties_json = src->properties_json ? heap_strdup(src->properties_json) : NULL;
}

/* Build a JSON suggestions response for ambiguous or fuzzy results. */
static char *snippet_suggestions(const char *input, cbm_node_t *nodes, int count) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_str(doc, root, "status", "ambiguous");

    char msg[512];
    snprintf(msg, sizeof(msg),
             "%d matches for \"%s\". Pick a qualified_name from suggestions below, "
             "or use search_graph(name_pattern=\"...\") to narrow results.",
             count, input);
    yyjson_mut_obj_add_str(doc, root, "message", msg);

    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    for (int i = 0; i < count; i++) {
        yyjson_mut_val *s = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, s, "qualified_name",
                               nodes[i].qualified_name ? nodes[i].qualified_name : "");
        yyjson_mut_obj_add_str(doc, s, "name", nodes[i].name ? nodes[i].name : "");
        yyjson_mut_obj_add_str(doc, s, "label", nodes[i].label ? nodes[i].label : "");
        yyjson_mut_obj_add_str(doc, s, "file_path", nodes[i].file_path ? nodes[i].file_path : "");
        yyjson_mut_arr_append(arr, s);
    }
    yyjson_mut_obj_add_val(doc, root, "suggestions", arr);

    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);

    char *result = cbm_mcp_text_result(json, false);
    free(json);
    return result;
}

/* Build an enriched snippet response for a resolved node. */
static char *build_snippet_response(cbm_mcp_server_t *srv, cbm_node_t *node,
                                    const char *match_method, bool include_neighbors,
                                    cbm_node_t *alternatives, int alt_count) {
    char *root_path = get_project_root(srv, node->project);

    int start = node->start_line > 0 ? node->start_line : 1;
    int end = node->end_line > start ? node->end_line : start + SNIPPET_DEFAULT_LINES;
    char *source = NULL;

    /* Build absolute path and verify it's within the project root.
     * Prevents path traversal via crafted file_path (e.g., "../../.ssh/id_rsa"). */
    char *abs_path = NULL;
    if (root_path && node->file_path) {
        size_t apsz = strlen(root_path) + strlen(node->file_path) + 2;
        abs_path = malloc(apsz);
        snprintf(abs_path, apsz, "%s/%s", root_path, node->file_path);

        /* Path containment: resolve symlinks/../ and verify file stays within root */
        char real_root[4096];
        char real_file[4096];
        bool path_ok = false;
#ifdef _WIN32
        if (_fullpath(real_root, root_path, sizeof(real_root)) &&
            _fullpath(real_file, abs_path, sizeof(real_file))) {
#else
        if (realpath(root_path, real_root) && realpath(abs_path, real_file)) {
#endif
            size_t root_len = strlen(real_root);
            if (strncmp(real_file, real_root, root_len) == 0 &&
                (real_file[root_len] == '/' || real_file[root_len] == '\\' ||
                 real_file[root_len] == '\0')) {
                path_ok = true;
            }
        }
        if (path_ok) {
            source = read_file_lines(abs_path, start, end);
        }
    }

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root_obj = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root_obj);

    yyjson_mut_obj_add_str(doc, root_obj, "name", node->name ? node->name : "");
    yyjson_mut_obj_add_str(doc, root_obj, "qualified_name",
                           node->qualified_name ? node->qualified_name : "");
    yyjson_mut_obj_add_str(doc, root_obj, "label", node->label ? node->label : "");

    const char *display_path = "";
    if (abs_path) {
        display_path = abs_path;
    } else if (node->file_path) {
        display_path = node->file_path;
    }
    yyjson_mut_obj_add_str(doc, root_obj, "file_path", display_path);
    yyjson_mut_obj_add_int(doc, root_obj, "start_line", start);
    yyjson_mut_obj_add_int(doc, root_obj, "end_line", end);

    if (source) {
        yyjson_mut_obj_add_str(doc, root_obj, "source", source);
    } else {
        yyjson_mut_obj_add_str(doc, root_obj, "source", "(source not available)");
    }

    /* match_method — omitted for exact matches */
    if (match_method) {
        yyjson_mut_obj_add_str(doc, root_obj, "match_method", match_method);
    }

    /* Enrich with node properties.
     * props_doc is freed AFTER serialization since yyjson_mut_obj_add_str
     * stores pointers into it (zero-copy). */
    yyjson_doc *props_doc = NULL;
    if (node->properties_json && node->properties_json[0] != '\0') {
        props_doc = yyjson_read(node->properties_json, strlen(node->properties_json), 0);
        if (props_doc) {
            yyjson_val *props_root = yyjson_doc_get_root(props_doc);
            if (props_root && yyjson_is_obj(props_root)) {
                yyjson_obj_iter iter;
                yyjson_obj_iter_init(props_root, &iter);
                yyjson_val *key;
                while ((key = yyjson_obj_iter_next(&iter))) {
                    yyjson_val *val = yyjson_obj_iter_get_val(key);
                    const char *k = yyjson_get_str(key);
                    if (!k) {
                        continue;
                    }
                    if (yyjson_is_str(val)) {
                        yyjson_mut_obj_add_str(doc, root_obj, k, yyjson_get_str(val));
                    } else if (yyjson_is_bool(val)) {
                        yyjson_mut_obj_add_bool(doc, root_obj, k, yyjson_get_bool(val));
                    } else if (yyjson_is_int(val)) {
                        yyjson_mut_obj_add_int(doc, root_obj, k, yyjson_get_int(val));
                    } else if (yyjson_is_real(val)) {
                        yyjson_mut_obj_add_real(doc, root_obj, k, yyjson_get_real(val));
                    }
                }
            }
        }
    }

    /* Caller/callee counts — store already resolved by calling handler */
    cbm_store_t *store = srv->store;
    int in_deg = 0;
    int out_deg = 0;
    cbm_store_node_degree(store, node->id, &in_deg, &out_deg);
    yyjson_mut_obj_add_int(doc, root_obj, "callers", in_deg);
    yyjson_mut_obj_add_int(doc, root_obj, "callees", out_deg);

    /* Include neighbor names (opt-in).
     * Strings stored by yyjson reference — freed after serialization. */
    char **nb_callers = NULL;
    int nb_caller_count = 0;
    char **nb_callees = NULL;
    int nb_callee_count = 0;
    if (include_neighbors) {
        cbm_store_node_neighbor_names(store, node->id, 10, &nb_callers, &nb_caller_count,
                                      &nb_callees, &nb_callee_count);
        if (nb_caller_count > 0) {
            yyjson_mut_val *arr = yyjson_mut_arr(doc);
            for (int i = 0; i < nb_caller_count; i++) {
                yyjson_mut_arr_add_str(doc, arr, nb_callers[i]);
            }
            yyjson_mut_obj_add_val(doc, root_obj, "caller_names", arr);
        }
        if (nb_callee_count > 0) {
            yyjson_mut_val *arr = yyjson_mut_arr(doc);
            for (int i = 0; i < nb_callee_count; i++) {
                yyjson_mut_arr_add_str(doc, arr, nb_callees[i]);
            }
            yyjson_mut_obj_add_val(doc, root_obj, "callee_names", arr);
        }
    }

    /* Alternatives (when auto-resolved from ambiguous) */
    if (alternatives && alt_count > 0) {
        yyjson_mut_val *arr = yyjson_mut_arr(doc);
        for (int i = 0; i < alt_count; i++) {
            yyjson_mut_val *a = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, a, "qualified_name",
                                   alternatives[i].qualified_name ? alternatives[i].qualified_name
                                                                  : "");
            yyjson_mut_obj_add_str(doc, a, "file_path",
                                   alternatives[i].file_path ? alternatives[i].file_path : "");
            yyjson_mut_arr_append(arr, a);
        }
        yyjson_mut_obj_add_val(doc, root_obj, "alternatives", arr);
    }

    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    yyjson_doc_free(props_doc); /* safe if NULL */
    for (int i = 0; i < nb_caller_count; i++) {
        free(nb_callers[i]);
    }
    for (int i = 0; i < nb_callee_count; i++) {
        free(nb_callees[i]);
    }
    // NOLINTNEXTLINE(bugprone-multi-level-implicit-pointer-conversion)
    free(nb_callers);
    // NOLINTNEXTLINE(bugprone-multi-level-implicit-pointer-conversion)
    free(nb_callees);
    free(root_path);
    free(abs_path);
    free(source);

    char *result = cbm_mcp_text_result(json, false);
    free(json);
    return result;
}

static char *handle_get_code_snippet(cbm_mcp_server_t *srv, const char *args) {
    char *qn = cbm_mcp_get_string_arg(args, "qualified_name");
    char *project = cbm_mcp_get_string_arg(args, "project");
    bool include_neighbors = cbm_mcp_get_bool_arg(args, "include_neighbors");

    if (!qn) {
        free(project);
        return cbm_mcp_text_result("qualified_name is required", true);
    }

    cbm_store_t *store = resolve_store(srv, project);
    if (!store) {
        free(qn);
        free(project);
        return cbm_mcp_text_result("no project loaded — run index_repository first", true);
    }

    /* Default to current project (same as all other tools) */
    const char *effective_project = project ? project : srv->current_project;

    /* Tier 1: Exact QN match */
    cbm_node_t node = {0};
    int rc = cbm_store_find_node_by_qn(store, effective_project, qn, &node);
    if (rc == CBM_STORE_OK) {
        char *result = build_snippet_response(srv, &node, NULL, include_neighbors, NULL, 0);
        free_node_contents(&node);
        free(qn);
        free(project);
        return result;
    }

    /* Tier 2: Suffix match — handles partial QNs ("main.HandleRequest")
     * and short names ("ProcessOrder") via LIKE '%.X'. */
    cbm_node_t *suffix_nodes = NULL;
    int suffix_count = 0;
    cbm_store_find_nodes_by_qn_suffix(store, effective_project, qn, &suffix_nodes, &suffix_count);

    if (suffix_count == 1) {
        copy_node(&suffix_nodes[0], &node);
        cbm_store_free_nodes(suffix_nodes, suffix_count);
        char *result = build_snippet_response(srv, &node, "suffix", include_neighbors, NULL, 0);
        free_node_contents(&node);
        free(qn);
        free(project);
        return result;
    }

    if (suffix_count > 1) {
        char *result = snippet_suggestions(qn, suffix_nodes, suffix_count);
        cbm_store_free_nodes(suffix_nodes, suffix_count);
        free(qn);
        free(project);
        return result;
    }

    cbm_store_free_nodes(suffix_nodes, suffix_count);
    free(qn);
    free(project);

    /* Nothing found — guide the caller toward search_graph */
    return cbm_mcp_text_result(
        "symbol not found. Use search_graph(name_pattern=\"...\") first to discover "
        "the exact qualified_name, then pass it to get_code_snippet.",
        true);
}

/* ── search_code ──────────────────────────────────────────────── */

static char *handle_search_code(cbm_mcp_server_t *srv, const char *args) {
    char *pattern = cbm_mcp_get_string_arg(args, "pattern");
    char *project = cbm_mcp_get_string_arg(args, "project");
    char *file_pattern = cbm_mcp_get_string_arg(args, "file_pattern");
    int limit = cbm_mcp_get_int_arg(args, "limit", 500000);
    bool use_regex = cbm_mcp_get_bool_arg(args, "regex");

    if (!pattern) {
        free(project);
        free(file_pattern);
        return cbm_mcp_text_result("pattern is required", true);
    }

    char *root_path = get_project_root(srv, project);
    if (!root_path) {
        free(pattern);
        free(project);
        free(file_pattern);
        return cbm_mcp_text_result("project not found or not indexed", true);
    }

    /* Reject shell metacharacters in user-supplied arguments */
    if (!cbm_validate_shell_arg(root_path) ||
        (file_pattern && !cbm_validate_shell_arg(file_pattern))) {
        free(root_path);
        free(pattern);
        free(project);
        free(file_pattern);
        return cbm_mcp_text_result("path or file_pattern contains invalid characters", true);
    }

    /* Write pattern to temp file to avoid shell injection */
    char tmpfile[256];
#ifdef _WIN32
    snprintf(tmpfile, sizeof(tmpfile), "/tmp/cbm_search_%d.pat", (int)_getpid());
#else
    snprintf(tmpfile, sizeof(tmpfile), "/tmp/cbm_search_%d.pat", (int)getpid());
#endif
    FILE *tf = fopen(tmpfile, "w");
    if (!tf) {
        free(root_path);
        free(pattern);
        free(project);
        free(file_pattern);
        return cbm_mcp_text_result("search failed: temp file", true);
    }
    // NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
    (void)fprintf(tf, "%s\n", pattern);
    (void)fclose(tf);

    char cmd[4096];
    // NOLINTNEXTLINE(readability-implicit-bool-conversion)
    const char *flag = use_regex ? "-E" : "-F";
    if (file_pattern) {
        snprintf(cmd, sizeof(cmd), "grep -rn %s --include='%s' -m %d -f '%s' '%s' 2>/dev/null",
                 flag, file_pattern, limit * 3, tmpfile, root_path);
    } else {
        snprintf(cmd, sizeof(cmd), "grep -rn %s -m %d -f '%s' '%s' 2>/dev/null", flag, limit * 3,
                 tmpfile, root_path);
    }

    // NOLINTNEXTLINE(bugprone-command-processor,cert-env33-c)
    FILE *fp = cbm_popen(cmd, "r");
    if (!fp) {
        cbm_unlink(tmpfile);
        free(root_path);
        free(pattern);
        free(project);
        free(file_pattern);
        return cbm_mcp_text_result("search failed", true);
    }

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root_obj = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root_obj);

    yyjson_mut_val *matches = yyjson_mut_arr(doc);
    char line[2048];
    int count = 0;
    size_t root_len = strlen(root_path);

    while (fgets(line, sizeof(line), fp) && count < limit) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0) {
            continue;
        }

        /* grep output: /abs/path/file:lineno:content */
        char *colon1 = strchr(line, ':');
        if (!colon1) {
            continue;
        }
        char *colon2 = strchr(colon1 + 1, ':');
        if (!colon2) {
            continue;
        }

        *colon1 = '\0';
        *colon2 = '\0';

        /* Strip root_path prefix to get relative path */
        const char *file = line;
        if (strncmp(file, root_path, root_len) == 0) {
            file += root_len;
            if (*file == '/') {
                file++;
            }
        }
        int lineno = (int)strtol(colon1 + 1, NULL, 10);
        const char *content = colon2 + 1;

        yyjson_mut_val *item = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, item, "file", file);
        yyjson_mut_obj_add_int(doc, item, "line", lineno);
        yyjson_mut_obj_add_str(doc, item, "content", content);
        yyjson_mut_arr_add_val(matches, item);
        count++;
    }
    cbm_pclose(fp);
    cbm_unlink(tmpfile); /* Clean up pattern file after grep is done */

    yyjson_mut_obj_add_val(doc, root_obj, "matches", matches);
    yyjson_mut_obj_add_int(doc, root_obj, "count", count);

    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    free(root_path);
    free(pattern);
    free(project);
    free(file_pattern);

    char *result = cbm_mcp_text_result(json, false);
    free(json);
    return result;
}

/* ── detect_changes ───────────────────────────────────────────── */

static char *handle_detect_changes(cbm_mcp_server_t *srv, const char *args) {
    char *project = cbm_mcp_get_string_arg(args, "project");
    char *base_branch = cbm_mcp_get_string_arg(args, "base_branch");
    int depth = cbm_mcp_get_int_arg(args, "depth", 2);

    if (!base_branch) {
        base_branch = heap_strdup("main");
    }

    /* Reject shell metacharacters in user-supplied branch name */
    if (!cbm_validate_shell_arg(base_branch)) {
        free(project);
        free(base_branch);
        return cbm_mcp_text_result("base_branch contains invalid characters", true);
    }

    char *root_path = get_project_root(srv, project);
    if (!root_path) {
        free(project);
        free(base_branch);
        return cbm_mcp_text_result("project not found", true);
    }

    if (!cbm_validate_shell_arg(root_path)) {
        free(root_path);
        free(project);
        free(base_branch);
        return cbm_mcp_text_result("project path contains invalid characters", true);
    }

    /* Get changed files via git */
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "cd '%s' && { git diff --name-only '%s'...HEAD 2>/dev/null; "
             "git diff --name-only 2>/dev/null; } | sort -u",
             root_path, base_branch);

    // NOLINTNEXTLINE(bugprone-command-processor,cert-env33-c)
    FILE *fp = cbm_popen(cmd, "r");
    if (!fp) {
        free(root_path);
        free(project);
        free(base_branch);
        return cbm_mcp_text_result("git diff failed", true);
    }

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root_obj = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root_obj);

    yyjson_mut_val *changed = yyjson_mut_arr(doc);
    yyjson_mut_val *impacted = yyjson_mut_arr(doc);

    /* resolve_store already called via get_project_root above */
    cbm_store_t *store = srv->store;

    char line[1024];
    int file_count = 0;

    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0) {
            continue;
        }

        yyjson_mut_arr_add_str(doc, changed, line);
        file_count++;

        /* Find symbols defined in this file */
        cbm_node_t *nodes = NULL;
        int ncount = 0;
        cbm_store_find_nodes_by_file(store, project, line, &nodes, &ncount);

        for (int i = 0; i < ncount; i++) {
            if (nodes[i].label && strcmp(nodes[i].label, "File") != 0 &&
                strcmp(nodes[i].label, "Folder") != 0 && strcmp(nodes[i].label, "Project") != 0) {
                yyjson_mut_val *item = yyjson_mut_obj(doc);
                yyjson_mut_obj_add_str(doc, item, "name", nodes[i].name ? nodes[i].name : "");
                yyjson_mut_obj_add_str(doc, item, "label", nodes[i].label);
                yyjson_mut_obj_add_str(doc, item, "file", line);
                yyjson_mut_arr_add_val(impacted, item);
            }
        }
        cbm_store_free_nodes(nodes, ncount);
    }
    cbm_pclose(fp);

    yyjson_mut_obj_add_val(doc, root_obj, "changed_files", changed);
    yyjson_mut_obj_add_int(doc, root_obj, "changed_count", file_count);
    yyjson_mut_obj_add_val(doc, root_obj, "impacted_symbols", impacted);
    yyjson_mut_obj_add_int(doc, root_obj, "depth", depth);

    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    free(root_path);
    free(project);
    free(base_branch);

    char *result = cbm_mcp_text_result(json, false);
    free(json);
    return result;
}

/* ── manage_adr ───────────────────────────────────────────────── */

static char *handle_manage_adr(cbm_mcp_server_t *srv, const char *args) {
    char *project = cbm_mcp_get_string_arg(args, "project");
    char *mode_str = cbm_mcp_get_string_arg(args, "mode");
    char *content = cbm_mcp_get_string_arg(args, "content");

    if (!mode_str) {
        mode_str = heap_strdup("get");
    }

    char *root_path = get_project_root(srv, project);
    if (!root_path) {
        free(project);
        free(mode_str);
        free(content);
        return cbm_mcp_text_result("project not found", true);
    }

    char adr_dir[4096];
    snprintf(adr_dir, sizeof(adr_dir), "%s/.codebase-memory", root_path);
    char adr_path[4096];
    snprintf(adr_path, sizeof(adr_path), "%s/adr.md", adr_dir);

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root_obj = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root_obj);

    if (strcmp(mode_str, "update") == 0 && content) {
        /* Create dir if needed */
        cbm_mkdir(adr_dir);
        FILE *fp = fopen(adr_path, "w");
        if (fp) {
            (void)fputs(content, fp);
            (void)fclose(fp);
            yyjson_mut_obj_add_str(doc, root_obj, "status", "updated");
        } else {
            yyjson_mut_obj_add_str(doc, root_obj, "status", "write_error");
        }
    } else if (strcmp(mode_str, "sections") == 0) {
        /* List section headers from ADR */
        FILE *fp = fopen(adr_path, "r");
        yyjson_mut_val *sections = yyjson_mut_arr(doc);
        if (fp) {
            char line[1024];
            while (fgets(line, sizeof(line), fp)) {
                if (line[0] == '#') {
                    size_t len = strlen(line);
                    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
                        line[--len] = '\0';
                    }
                    yyjson_mut_arr_add_str(doc, sections, line);
                }
            }
            (void)fclose(fp);
        }
        yyjson_mut_obj_add_val(doc, root_obj, "sections", sections);
    } else {
        /* get: read ADR content */
        FILE *fp = fopen(adr_path, "r");
        if (fp) {
            (void)fseek(fp, 0, SEEK_END);
            long sz = ftell(fp);
            (void)fseek(fp, 0, SEEK_SET);
            char *buf = malloc(sz + 1);
            size_t n = fread(buf, 1, sz, fp);
            buf[n] = '\0';
            (void)fclose(fp);
            yyjson_mut_obj_add_str(doc, root_obj, "content", buf);
            free(buf);
        } else {
            yyjson_mut_obj_add_str(doc, root_obj, "content", "");
            yyjson_mut_obj_add_str(doc, root_obj, "status", "no_adr");
            yyjson_mut_obj_add_str(
                doc, root_obj, "adr_hint",
                "No ADR yet. Create one with manage_adr(mode='update', "
                "content='## PURPOSE\\n...\\n\\n## STACK\\n...\\n\\n## ARCHITECTURE\\n..."
                "\\n\\n## PATTERNS\\n...\\n\\n## TRADEOFFS\\n...\\n\\n## PHILOSOPHY\\n...'). "
                "For guided creation: explore the codebase with get_architecture, "
                "then draft and store. Sections: PURPOSE, STACK, ARCHITECTURE, "
                "PATTERNS, TRADEOFFS, PHILOSOPHY.");
        }
    }

    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    free(root_path);
    free(project);
    free(mode_str);
    free(content);

    char *result = cbm_mcp_text_result(json, false);
    free(json);
    return result;
}

/* ── ingest_traces ────────────────────────────────────────────── */

static char *handle_ingest_traces(cbm_mcp_server_t *srv, const char *args) {
    (void)srv;
    /* Parse traces array from JSON args */
    yyjson_doc *adoc = yyjson_read(args, strlen(args), 0);
    int trace_count = 0;

    if (adoc) {
        yyjson_val *aroot = yyjson_doc_get_root(adoc);
        yyjson_val *traces = yyjson_obj_get(aroot, "traces");
        if (traces && yyjson_is_arr(traces)) {
            trace_count = (int)yyjson_arr_size(traces);
        }
        yyjson_doc_free(adoc);
    }

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_str(doc, root, "status", "accepted");
    yyjson_mut_obj_add_int(doc, root, "traces_received", trace_count);
    yyjson_mut_obj_add_str(doc, root, "note",
                           "Runtime edge creation from traces not yet implemented");

    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);

    char *result = cbm_mcp_text_result(json, false);
    free(json);
    return result;
}

/* ── Tool dispatch ────────────────────────────────────────────── */

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
char *cbm_mcp_handle_tool(cbm_mcp_server_t *srv, const char *tool_name, const char *args_json) {
    if (!tool_name) {
        return cbm_mcp_text_result("missing tool name", true);
    }

    if (strcmp(tool_name, "list_projects") == 0) {
        return handle_list_projects(srv, args_json);
    }
    if (strcmp(tool_name, "get_graph_schema") == 0) {
        return handle_get_graph_schema(srv, args_json);
    }
    if (strcmp(tool_name, "get_file_context") == 0) {
        return handle_get_file_context(srv, args_json);
    }
    if (strcmp(tool_name, "get_related_files") == 0) {
        return handle_get_related_files(srv, args_json);
    }
    if (strcmp(tool_name, "get_tests") == 0) {
        return handle_get_tests(srv, args_json);
    }
    if (strcmp(tool_name, "get_callers") == 0) {
        return handle_get_callers(srv, args_json);
    }
    if (strcmp(tool_name, "get_change_risks") == 0) {
        return handle_get_change_risks(srv, args_json);
    }
    if (strcmp(tool_name, "get_edit_plan") == 0) {
        return handle_get_edit_plan(srv, args_json);
    }
    if (strcmp(tool_name, "search_graph") == 0) {
        return handle_search_graph(srv, args_json);
    }
    if (strcmp(tool_name, "query_graph") == 0) {
        return handle_query_graph(srv, args_json);
    }
    if (strcmp(tool_name, "index_status") == 0) {
        return handle_index_status(srv, args_json);
    }
    if (strcmp(tool_name, "delete_project") == 0) {
        return handle_delete_project(srv, args_json);
    }
    if (strcmp(tool_name, "trace_call_path") == 0) {
        return handle_trace_call_path(srv, args_json);
    }
    if (strcmp(tool_name, "get_architecture") == 0) {
        return handle_get_architecture(srv, args_json);
    }

    /* Pipeline-dependent tools */
    if (strcmp(tool_name, "index_repository") == 0) {
        return handle_index_repository(srv, args_json);
    }
    if (strcmp(tool_name, "get_code_snippet") == 0) {
        return handle_get_code_snippet(srv, args_json);
    }
    if (strcmp(tool_name, "search_code") == 0) {
        return handle_search_code(srv, args_json);
    }
    if (strcmp(tool_name, "detect_changes") == 0) {
        return handle_detect_changes(srv, args_json);
    }
    if (strcmp(tool_name, "manage_adr") == 0) {
        return handle_manage_adr(srv, args_json);
    }
    if (strcmp(tool_name, "ingest_traces") == 0) {
        return handle_ingest_traces(srv, args_json);
    }

    char msg[256];
    snprintf(msg, sizeof(msg), "unknown tool: %s", tool_name);
    return cbm_mcp_text_result(msg, true);
}

/* ── Session detection + auto-index ────────────────────────────── */

/* Detect session root from CWD (fallback: single indexed project from DB). */
static void detect_session(cbm_mcp_server_t *srv) {
    if (srv->session_detected) {
        return;
    }
    srv->session_detected = true;

    /* 1. Try CWD */
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd)) != NULL) {
        // NOLINTNEXTLINE(concurrency-mt-unsafe)
        const char *home = getenv("HOME");
        /* Skip useless roots: / and $HOME */
        if (strcmp(cwd, "/") != 0 && (home == NULL || strcmp(cwd, home) != 0)) {
            snprintf(srv->session_root, sizeof(srv->session_root), "%s", cwd);
            cbm_log_info("session.root.cwd", "path", cwd);
        }
    }

    /* Derive project name from path */
    if (srv->session_root[0]) {
        /* Use last two path components joined by dash, matching Go's ProjectNameFromPath */
        const char *p = srv->session_root;
        const char *last_slash = strrchr(p, '/');
        if (last_slash && last_slash > p) {
            const char *prev = last_slash - 1;
            while (prev > p && *prev != '/') {
                prev--;
            }
            if (*prev == '/') {
                prev++;
            }
            snprintf(srv->session_project, sizeof(srv->session_project), "%.*s",
                     (int)(strlen(p) - (size_t)(prev - p)), prev);
            /* Replace / with - */
            for (char *c = srv->session_project; *c; c++) {
                if (*c == '/') {
                    *c = '-';
                }
            }
        } else {
            snprintf(srv->session_project, sizeof(srv->session_project), "%s",
                     last_slash ? last_slash + 1 : p);
        }
    }
}

/* Background auto-index thread function */
static void *autoindex_thread(void *arg) {
    cbm_mcp_server_t *srv = (cbm_mcp_server_t *)arg;

    cbm_log_info("autoindex.start", "project", srv->session_project, "path", srv->session_root);

    cbm_pipeline_t *p = cbm_pipeline_new(srv->session_root, NULL, CBM_MODE_FULL);
    if (!p) {
        cbm_log_warn("autoindex.err", "msg", "pipeline_create_failed");
        return NULL;
    }

    int rc = cbm_pipeline_run(p);
    cbm_pipeline_free(p);
    cbm_mem_collect(); /* return mimalloc pages to OS after indexing */

    if (rc == 0) {
        cbm_log_info("autoindex.done", "project", srv->session_project);
        /* Register with watcher for ongoing change detection */
        if (srv->watcher) {
            cbm_watcher_watch(srv->watcher, srv->session_project, srv->session_root);
        }
    } else {
        cbm_log_warn("autoindex.err", "msg", "pipeline_run_failed");
    }
    return NULL;
}

/* Start auto-indexing if configured and project not yet indexed. */
static void maybe_auto_index(cbm_mcp_server_t *srv) {
    if (srv->session_root[0] == '\0') {
        return; /* no session root detected */
    }

    /* Check if project already has a DB */
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    const char *home = getenv("HOME");
    if (home) {
        char db_check[1024];
        snprintf(db_check, sizeof(db_check), "%s/.cache/codebase-memory-mcp/%s.db", home,
                 srv->session_project);
        struct stat st;
        if (stat(db_check, &st) == 0) {
            /* Already indexed → register watcher for change detection */
            cbm_log_info("autoindex.skip", "reason", "already_indexed", "project",
                         srv->session_project);
            if (srv->watcher) {
                cbm_watcher_watch(srv->watcher, srv->session_project, srv->session_root);
            }
            return;
        }
    }

/* Default file limit for auto-indexing new projects */
#define DEFAULT_AUTO_INDEX_LIMIT 50000

    /* Check auto_index config */
    bool auto_index = false;
    int file_limit = DEFAULT_AUTO_INDEX_LIMIT;
    if (srv->config) {
        auto_index = cbm_config_get_bool(srv->config, CBM_CONFIG_AUTO_INDEX, false);
        file_limit =
            cbm_config_get_int(srv->config, CBM_CONFIG_AUTO_INDEX_LIMIT, DEFAULT_AUTO_INDEX_LIMIT);
    }

    if (!auto_index) {
        cbm_log_info("autoindex.skip", "reason", "disabled", "hint",
                     "run: codebase-memory-mcp config set auto_index true");
        return;
    }

    /* Quick file count check to avoid OOM on massive repos */
    if (!cbm_validate_shell_arg(srv->session_root)) {
        cbm_log_warn("autoindex.skip", "reason", "path contains shell metacharacters");
        return;
    }
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "git -C '%s' ls-files 2>/dev/null | wc -l", srv->session_root);
    // NOLINTNEXTLINE(bugprone-command-processor,cert-env33-c)
    FILE *fp = cbm_popen(cmd, "r");
    if (fp) {
        char line[64];
        if (fgets(line, sizeof(line), fp)) {
            int count = (int)strtol(line, NULL, 10);
            if (count > file_limit) {
                cbm_log_warn("autoindex.skip", "reason", "too_many_files", "files", line, "limit",
                             CBM_CONFIG_AUTO_INDEX_LIMIT);
                cbm_pclose(fp);
                return;
            }
        }
        cbm_pclose(fp);
    }

    /* Launch auto-index in background */
    if (cbm_thread_create(&srv->autoindex_tid, 0, autoindex_thread, srv) == 0) {
        srv->autoindex_active = true;
    }
}

/* ── Background update check ──────────────────────────────────── */

#define UPDATE_CHECK_URL "https://api.github.com/repos/DeusData/codebase-memory-mcp/releases/latest"

static void *update_check_thread(void *arg) {
    cbm_mcp_server_t *srv = (cbm_mcp_server_t *)arg;

    /* Use curl with 5s timeout to fetch latest release tag */
    FILE *fp = cbm_popen("curl -sf --max-time 5 -H 'Accept: application/vnd.github+json' "
                         "'" UPDATE_CHECK_URL "' 2>/dev/null",
                         "r");
    if (!fp) {
        srv->update_checked = true;
        return NULL;
    }

    char buf[4096];
    size_t total = 0;
    while (total < sizeof(buf) - 1) {
        size_t n = fread(buf + total, 1, sizeof(buf) - 1 - total, fp);
        if (n == 0) {
            break;
        }
        total += n;
    }
    buf[total] = '\0';
    cbm_pclose(fp);

    /* Parse tag_name from JSON response */
    yyjson_doc *doc = yyjson_read(buf, total, 0);
    if (!doc) {
        srv->update_checked = true;
        return NULL;
    }

    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *tag = yyjson_obj_get(root, "tag_name");
    const char *tag_str = yyjson_get_str(tag);

    if (tag_str) {
        const char *current = cbm_cli_get_version();
        if (cbm_compare_versions(tag_str, current) > 0) {
            snprintf(srv->update_notice, sizeof(srv->update_notice),
                     "Update available: %s -> %s -- run: codebase-memory-mcp update", current,
                     tag_str);
            cbm_log_info("update.available", "current", current, "latest", tag_str);
        }
    }

    yyjson_doc_free(doc);
    srv->update_checked = true;
    return NULL;
}

static void start_update_check(cbm_mcp_server_t *srv) {
    if (srv->update_checked) {
        return;
    }
    srv->update_checked = true; /* prevent double-launch */
    if (cbm_thread_create(&srv->update_tid, 0, update_check_thread, srv) == 0) {
        srv->update_thread_active = true;
    }
}

/* Prepend update notice to a tool result, then clear it (one-shot). */
static char *inject_update_notice(cbm_mcp_server_t *srv, char *result_json) {
    if (srv->update_notice[0] == '\0') {
        return result_json;
    }

    /* Parse existing result, prepend notice text, rebuild */
    yyjson_doc *doc = yyjson_read(result_json, strlen(result_json), 0);
    if (!doc) {
        return result_json;
    }

    yyjson_mut_doc *mdoc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_val_mut_copy(mdoc, yyjson_doc_get_root(doc));
    yyjson_doc_free(doc);
    if (!root) {
        yyjson_mut_doc_free(mdoc);
        return result_json;
    }
    yyjson_mut_doc_set_root(mdoc, root);

    /* Find the "content" array */
    yyjson_mut_val *content = yyjson_mut_obj_get(root, "content");
    if (content && yyjson_mut_is_arr(content)) {
        /* Prepend a text content item with the update notice */
        yyjson_mut_val *notice_item = yyjson_mut_obj(mdoc);
        yyjson_mut_obj_add_str(mdoc, notice_item, "type", "text");
        yyjson_mut_obj_add_str(mdoc, notice_item, "text", srv->update_notice);
        yyjson_mut_arr_prepend(content, notice_item);
    }

    size_t len;
    char *new_json = yyjson_mut_write(mdoc, YYJSON_WRITE_ALLOW_INVALID_UNICODE, &len);
    yyjson_mut_doc_free(mdoc);

    if (new_json) {
        free(result_json);
        srv->update_notice[0] = '\0'; /* clear — one-shot */
        return new_json;
    }
    return result_json;
}

/* ── Server request handler ───────────────────────────────────── */

char *cbm_mcp_server_handle(cbm_mcp_server_t *srv, const char *line) {
    cbm_jsonrpc_request_t req = {0};
    if (cbm_jsonrpc_parse(line, &req) < 0) {
        return cbm_jsonrpc_format_error(0, JSONRPC_PARSE_ERROR, "Parse error");
    }

    /* Notifications (no id) → no response */
    if (!req.has_id) {
        cbm_jsonrpc_request_free(&req);
        return NULL;
    }

    char *result_json = NULL;

    if (strcmp(req.method, "initialize") == 0) {
        result_json = cbm_mcp_initialize_response(req.params_raw);
        start_update_check(srv);
        detect_session(srv);
        maybe_auto_index(srv);
    } else if (strcmp(req.method, "tools/list") == 0) {
        result_json = cbm_mcp_tools_list();
    } else if (strcmp(req.method, "tools/call") == 0) {
        char *tool_name = req.params_raw ? cbm_mcp_get_tool_name(req.params_raw) : NULL;
        char *tool_args =
            req.params_raw ? cbm_mcp_get_arguments(req.params_raw) : heap_strdup("{}");

        result_json = cbm_mcp_handle_tool(srv, tool_name, tool_args);
        result_json = inject_update_notice(srv, result_json);
        free(tool_name);
        free(tool_args);
    } else {
        char *err = cbm_jsonrpc_format_error(req.id, JSONRPC_METHOD_NOT_FOUND, "Method not found");
        cbm_jsonrpc_request_free(&req);
        return err;
    }

    cbm_jsonrpc_response_t resp = {
        .id = req.id,
        .result_json = result_json,
    };
    char *out = cbm_jsonrpc_format_response(&resp);
    free(result_json);
    cbm_jsonrpc_request_free(&req);
    return out;
}

/* ── Event loop ───────────────────────────────────────────────── */

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
int cbm_mcp_server_run(cbm_mcp_server_t *srv, FILE *in, FILE *out) {
    char *line = NULL;
    size_t cap = 0;
    int fd = cbm_fileno(in);

    for (;;) {
        /* Poll with idle timeout so we can evict unused stores between requests.
         * MCP is request-response (one line at a time), so mixing poll() on the
         * raw fd with getline() on the buffered FILE* is safe in practice. */
#ifdef _WIN32
        /* Windows: WaitForSingleObject on stdin handle */
        HANDLE hStdin = (HANDLE)_get_osfhandle(fd);
        DWORD wr = WaitForSingleObject(hStdin, STORE_IDLE_TIMEOUT_S * 1000);
        if (wr == WAIT_FAILED) {
            break;
        }
        if (wr == WAIT_TIMEOUT) {
            cbm_mcp_server_evict_idle(srv, STORE_IDLE_TIMEOUT_S);
            continue;
        }
#else
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int pr = poll(&pfd, 1, STORE_IDLE_TIMEOUT_S * 1000);

        if (pr < 0) {
            break; /* error or signal */
        }
        if (pr == 0) {
            /* Timeout — evict idle store to free resources */
            cbm_mcp_server_evict_idle(srv, STORE_IDLE_TIMEOUT_S);
            continue;
        }
#endif

        if (cbm_getline(&line, &cap, in) <= 0) {
            break;
        }

        /* Trim trailing newline/CR */
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0) {
            continue;
        }

        /* Content-Length framing support (LSP-style transport).
         * Some MCP clients (OpenCode, VS Code extensions) send:
         *   Content-Length: <n>\r\n\r\n<json>
         * instead of bare JSONL. Detect the header, read the payload,
         * and respond with the same framing. */
        if (strncmp(line, "Content-Length:", 15) == 0) {
            int content_len = (int)strtol(line + 15, NULL, 10);
            if (content_len <= 0 || content_len > 10 * 1024 * 1024) {
                continue; /* invalid or too large */
            }

            /* Skip blank line(s) between header and body */
            while (cbm_getline(&line, &cap, in) > 0) {
                size_t hlen = strlen(line);
                while (hlen > 0 && (line[hlen - 1] == '\n' || line[hlen - 1] == '\r')) {
                    line[--hlen] = '\0';
                }
                if (hlen == 0) {
                    break; /* found the blank separator */
                }
                /* Skip other headers (e.g. Content-Type) */
            }

            /* Read exact content_len bytes */
            char *body = malloc((size_t)content_len + 1);
            if (!body) {
                continue;
            }
            size_t nread = fread(body, 1, (size_t)content_len, in);
            body[nread] = '\0';

            char *resp = cbm_mcp_server_handle(srv, body);
            free(body);

            if (resp) {
                size_t rlen = strlen(resp);
                (void)fprintf(out, "Content-Length: %zu\r\n\r\n%s", rlen, resp);
                (void)fflush(out);
                free(resp);
            }
            continue;
        }

        char *resp = cbm_mcp_server_handle(srv, line);
        if (resp) {
            // NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
            (void)fprintf(out, "%s\n", resp);
            (void)fflush(out);
            free(resp);
        }
    }

    free(line);
    return 0;
}

/* ── cbm_parse_file_uri ──────────────────────────────────────── */

bool cbm_parse_file_uri(const char *uri, char *out_path, int out_size) {
    if (!uri || !out_path || out_size <= 0) {
        if (out_path && out_size > 0) {
            out_path[0] = '\0';
        }
        return false;
    }

    /* Must start with file:// */
    if (strncmp(uri, "file://", 7) != 0) {
        out_path[0] = '\0';
        return false;
    }

    const char *path = uri + 7;

    /* On Windows, file:///C:/path → /C:/path. Strip leading / before drive letter. */
    if (path[0] == '/' && path[1] &&
        ((path[1] >= 'A' && path[1] <= 'Z') || (path[1] >= 'a' && path[1] <= 'z')) &&
        path[2] == ':') {
        path++; /* skip the leading / */
    }

    snprintf(out_path, out_size, "%s", path);
    return true;
}

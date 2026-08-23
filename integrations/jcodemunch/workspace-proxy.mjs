#!/usr/bin/env node

import { spawn } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import readline from "node:readline";

function fail(message) {
  process.stderr.write(`workspace-proxy: ${message}\n`);
  process.exit(2);
}

function argument(name) {
  const index = process.argv.indexOf(name);
  if (index < 0 || index + 1 >= process.argv.length) fail(`missing ${name}`);
  return process.argv[index + 1];
}

const proxyEntrypoint = path.resolve(argument("--proxy-entrypoint"));
const endpoint = argument("--endpoint");
const workspaceRoot = fs.realpathSync.native(path.resolve(argument("--workspace-root")));
if (!fs.statSync(workspaceRoot).isDirectory()) fail("workspace root is not a directory");
if (!fs.statSync(proxyEntrypoint).isFile()) fail("pinned proxy entrypoint is missing");

const normalizedRoot = process.platform === "win32" ? workspaceRoot.toLowerCase() : workspaceRoot;
const rootPrefix = normalizedRoot.endsWith(path.sep) ? normalizedRoot : `${normalizedRoot}${path.sep}`;
const toolSchemas = new Map();
const listRequests = new Set();

function localError(message, id) {
  process.stdout.write(`${JSON.stringify({ jsonrpc: "2.0", id, error: { code: -32602, message } })}\n`);
}

function insideWorkspace(candidate) {
  const normalized = process.platform === "win32" ? candidate.toLowerCase() : candidate;
  return normalized === normalizedRoot || normalized.startsWith(rootPrefix);
}

function bindPath(value, field) {
  if (typeof value !== "string" || value.trim() === "") {
    throw new Error(`${field} must be a non-empty path`);
  }
  const lexical = path.resolve(workspaceRoot, value);
  if (!insideWorkspace(lexical)) throw new Error(`${field} escapes the bound workspace`);

  // Resolve every existing ancestor, not just the workspace root. This blocks
  // a symlink/junction inside the workspace from redirecting a seemingly safe
  // lexical path into another project. Missing tail components are rebuilt
  // only after their nearest existing ancestor has been canonicalized.
  const missing = [];
  let ancestor = lexical;
  while (!fs.existsSync(ancestor)) {
    const parent = path.dirname(ancestor);
    if (parent === ancestor) throw new Error(`${field} has no resolvable workspace ancestor`);
    missing.unshift(path.basename(ancestor));
    ancestor = parent;
  }
  const canonicalAncestor = fs.realpathSync.native(ancestor);
  const canonical = path.resolve(canonicalAncestor, ...missing);
  if (!insideWorkspace(canonical)) throw new Error(`${field} escapes the bound workspace through a link`);
  return canonical;
}

const rootPathTools = new Set(["index_folder", "resolve_repo"]);
const blockedGlobalTools = new Set([
  "index_repo",
  "list_repos",
  "list_projects",
  "get_session_snapshot",
  "invalidate_cache",
]);
const pathFields = new Set([
  "path",
  "file_path",
  "folder_path",
  "project_path",
  "root_path",
  "workspace_root",
  "cwd",
]);

function bindArguments(toolName, args, schema) {
  if (blockedGlobalTools.has(toolName)) {
    throw new Error(`${toolName} is disabled by workspace isolation; use index_folder/resolve_repo for the current project`);
  }
  const bound = args && typeof args === "object" && !Array.isArray(args) ? structuredClone(args) : {};
  const properties = schema?.properties && typeof schema.properties === "object" ? schema.properties : {};

  if (Object.hasOwn(properties, "repo")) bound.repo = workspaceRoot;
  if (Object.hasOwn(properties, "repository")) bound.repository = workspaceRoot;
  if (rootPathTools.has(toolName) && Object.hasOwn(properties, "path")) bound.path = workspaceRoot;

  for (const field of pathFields) {
    if (!Object.hasOwn(properties, field) || !Object.hasOwn(bound, field)) continue;
    if (field === "path" && rootPathTools.has(toolName)) continue;
    bound[field] = bindPath(bound[field], field);
  }
  if (Object.hasOwn(bound, "storage_path")) delete bound.storage_path;

  // jCodeMunch's compact orchestration tools carry a nested tool call. Bind
  // that nested call as well so order(index_folder, {path: "."}) cannot
  // silently resolve against the singleton server's first working directory.
  if (typeof bound.action === "string" && bound.args && typeof bound.args === "object") {
    const nestedSchema = toolSchemas.get(bound.action);
    if (!nestedSchema) throw new Error(`schema for nested tool ${bound.action} is unavailable`);
    bound.args = bindArguments(bound.action, bound.args, nestedSchema);
  }
  for (const listField of ["calls", "operations", "requests"]) {
    if (!Array.isArray(bound[listField])) continue;
    bound[listField] = bound[listField].map((item) => {
      const nestedName = item?.name ?? item?.tool ?? item?.action;
      const nestedArgs = item?.arguments ?? item?.args ?? item?.params ?? {};
      const nestedSchema = toolSchemas.get(nestedName);
      if (!nestedSchema) throw new Error(`schema for nested tool ${nestedName} is unavailable`);
      const next = structuredClone(item);
      if (Object.hasOwn(next, "arguments")) next.arguments = bindArguments(nestedName, nestedArgs, nestedSchema);
      else if (Object.hasOwn(next, "params")) next.params = bindArguments(nestedName, nestedArgs, nestedSchema);
      else next.args = bindArguments(nestedName, nestedArgs, nestedSchema);
      return next;
    });
  }
  return bound;
}

const childEnvironment = { ...process.env };
delete childEnvironment.NODE_OPTIONS;
delete childEnvironment.NODE_PATH;

const child = spawn(process.execPath, [
  proxyEntrypoint,
  endpoint,
  "--transport",
  "http-only",
  "--header",
  "Authorization: Bearer ${JCODEMUNCH_HTTP_TOKEN}",
], {
  stdio: ["pipe", "pipe", "pipe"],
  windowsHide: true,
  env: childEnvironment,
});

const clientLines = readline.createInterface({ input: process.stdin, crlfDelay: Infinity });
clientLines.on("line", (line) => {
  if (!line.trim()) return;
  let message;
  try { message = JSON.parse(line); }
  catch { localError("invalid JSON-RPC input", null); return; }
  if (message.method === "tools/list" && message.id !== undefined) listRequests.add(String(message.id));
  if (message.method === "tools/call") {
    const toolName = message.params?.name;
    const schema = toolSchemas.get(toolName);
    if (!schema) {
      localError(`workspace schema for ${toolName ?? "unknown tool"} is unavailable; refresh tools/list`, message.id ?? null);
      return;
    }
    try { message.params.arguments = bindArguments(toolName, message.params?.arguments, schema); }
    catch (error) { localError(`workspace isolation rejected ${toolName}: ${error.message}`, message.id ?? null); return; }
  }
  child.stdin.write(`${JSON.stringify(message)}\n`);
});
clientLines.on("close", () => child.stdin.end());

const serverLines = readline.createInterface({ input: child.stdout, crlfDelay: Infinity });
serverLines.on("line", (line) => {
  if (!line.trim()) return;
  try {
    const message = JSON.parse(line);
    if (message.id !== undefined && listRequests.delete(String(message.id)) && Array.isArray(message.result?.tools)) {
      for (const tool of message.result.tools) {
        if (typeof tool?.name === "string") toolSchemas.set(tool.name, tool.inputSchema ?? {});
      }
    }
  } catch {
    // Preserve the upstream proxy's output; the MCP client owns protocol error handling.
  }
  process.stdout.write(`${line}\n`);
});
child.stderr.pipe(process.stderr);

for (const signal of ["SIGINT", "SIGTERM"]) {
  process.on(signal, () => { if (!child.killed) child.kill(signal); });
}
child.on("error", (error) => fail(`unable to start pinned proxy: ${error.message}`));
child.on("exit", (code, signal) => {
  const exitCode = signal ? 1 : code ?? 1;
  serverLines.close();
  clientLines.close();
  process.stdin.pause();
  if (!child.stdin.destroyed) child.stdin.destroy();
  setImmediate(() => process.exit(exitCode));
});

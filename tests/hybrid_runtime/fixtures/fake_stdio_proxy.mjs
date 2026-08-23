import fs from "node:fs";
import http from "node:http";

const [endpoint, transportFlag, transport, headerFlag, header] = process.argv.slice(2);
const token = process.env.JCODEMUNCH_HTTP_TOKEN ?? "";
if (transportFlag !== "--transport" || transport !== "http-only" ||
    headerFlag !== "--header" || header !== "Authorization: Bearer ${JCODEMUNCH_HTTP_TOKEN}" || !token) {
  process.exit(92);
}

if (process.env.JCODEMUNCH_PROXY_OBSERVATION) {
  fs.writeFileSync(process.env.JCODEMUNCH_PROXY_OBSERVATION, JSON.stringify({
    pid: process.pid,
    endpoint,
    transport,
    header_is_placeholder: header.includes("${JCODEMUNCH_HTTP_TOKEN}"),
    token_present_in_environment: Boolean(token),
    argv_contains_token: process.argv.some((value) => value.includes(token)),
  }));
}

if (process.env.JCODEMUNCH_PROXY_EXIT_IMMEDIATELY === "1") process.exit(7);

let buffered = "";
process.stdin.setEncoding("utf8");
process.stdin.on("data", (chunk) => {
  buffered += chunk;
  let newline;
  while ((newline = buffered.indexOf("\n")) >= 0) {
    const payload = buffered.slice(0, newline).trim();
    buffered = buffered.slice(newline + 1);
    if (payload) forward(payload);
  }
});

function forward(payload) {
  const target = new URL(endpoint);
  const request = http.request({
    hostname: target.hostname,
    port: Number(target.port),
    path: target.pathname,
    method: "POST",
    headers: {
      Authorization: `Bearer ${token}`,
      Accept: "application/json, text/event-stream",
      "Content-Type": "application/json",
      "Content-Length": Buffer.byteLength(payload),
    },
  }, (response) => {
    let body = "";
    response.setEncoding("utf8");
    response.on("data", (chunk) => { body += chunk; });
    response.on("end", () => {
      const data = body.split(/\r?\n/).filter((line) => line.startsWith("data: ")).at(-1);
      if (!data) process.exit(93);
      process.stdout.write(`${data.slice(6)}\n`);
    });
  });
  request.on("error", (error) => {
    process.stderr.write(`${error.message}\n`);
    process.exit(94);
  });
  request.end(payload);
}

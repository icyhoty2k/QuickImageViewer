const { createInterface } = require("readline");
const { execFile } = require("child_process");

const CT = "D:\\07_Programs\\JetBrains\\cLion\\CLion2026.1\\bin\\clang\\win\\x64\\bin\\clang-tidy.exe";
const ROOT = "I:\\30_CppSources\\QuickImageViewer";

const rl = createInterface({ input: process.stdin });
let buf = "";

function send(o) { process.stdout.write(JSON.stringify(o) + "\n"); }

rl.on("line", l => {
  buf += l;
  try {
    const msg = JSON.parse(buf);
    buf = "";
    handle(msg);
  } catch {}
});

function handle(msg) {
  const { id, method, params } = msg;
  if (method === "initialize") {
    send({ jsonrpc: "2.0", id, result: { protocolVersion: "2024-11-05", capabilities: { tools: {} }, serverInfo: { name: "clang-tidy-mcp", version: "1.0.0" } } });
  } else if (method === "tools/list") {
    send({ jsonrpc: "2.0", id, result: { tools: [{ name: "run_clang_tidy", description: "Run clang-tidy on a C/C++ file", inputSchema: { type: "object", properties: { file: { type: "string", description: "File path (relative to project root)" }, checks: { type: "string", description: "Checks filter" } }, required: ["file"] } }] } });
  } else if (method === "tools/call") {
    if (params.name === "run_clang_tidy") {
      const fp = params.arguments.file;
      const chk = params.arguments.checks || "-*,bugprone-*,performance-*,modernize-*,readability-*";
      const full = fp.includes(":") ? fp : ROOT + "\\" + fp.replace(/\//g, "\\");
      execFile(CT, ["--checks=" + chk, full, "-p=" + ROOT], { timeout: 30000 }, (err, stdout, stderr) => {
        send({ jsonrpc: "2.0", id, result: { content: [{ type: "text", text: (stdout || stderr || "No issues found.") }] } });
      });
    } else {
      send({ jsonrpc: "2.0", id, error: { code: -32601, message: "Unknown tool" } });
    }
  } else if (method === "shutdown") {
    send({ jsonrpc: "2.0", id, result: null });
    process.exit(0);
  }
}

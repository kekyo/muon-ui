// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { spawn } from "node:child_process";
import { once } from "node:events";

/** Exercises a live Muon MCP server through an installed, authenticated Codex CLI.
 * @param directory - Isolated temporary working directory for this invocation.
 * @param endpoint - Loopback MCP endpoint of the running driver.
 * @param bearerToken - Optional token passed through the child environment.
 * @param value - Text Codex must write through the MCP tool.
 * @returns JSONL events, with any occurrence of the test token redacted.
 * @remarks Uses per-invocation configuration and does not register an MCP server
 * globally. The caller independently checks the visible application state.
 */
export const runCodexMcpClient = async (
  directory: string,
  endpoint: string,
  bearerToken: string | undefined,
  value: string,
): Promise<string> => {
  const args = [
    "exec",
    "--ignore-user-config",
    "--ephemeral",
    "--skip-git-repo-check",
    "--sandbox",
    "read-only",
    "--json",
    "-C",
    directory,
    "-c",
    `mcp_servers.muon.url=${JSON.stringify(endpoint)}`,
    "-c",
    "mcp_servers.muon.required=true",
    "-c",
    'mcp_servers.muon.enabled_tools=["read_state","write_state"]',
    // The integration test explicitly authorizes this one temporary state write.
    // Keep the shell sandbox and every other tool's approval policy intact.
    "-c",
    'mcp_servers.muon.tools.write_state.approval_mode="approve"',
  ];
  if (bearerToken !== undefined)
    args.push(
      "-c",
      'mcp_servers.muon.bearer_token_env_var="MUON_TEST_MCP_TOKEN"',
    );
  args.push(
    `This is an integration test of an already-running local Muon application. ` +
      `Use only the muon MCP tools: first read_state, then write_state exactly once ` +
      `with value ${JSON.stringify(value)}, then read_state to verify it. ` +
      `Do not use shell, files, browsing or other tools. Report the final state.`,
  );
  const child = spawn("codex", args, {
    cwd: directory,
    env: {
      ...process.env,
      ...(bearerToken === undefined
        ? {}
        : { MUON_TEST_MCP_TOKEN: bearerToken }),
    },
    stdio: ["ignore", "pipe", "pipe"],
    timeout: 180000,
  });
  let output = "";
  let errors = "";
  child.stdout.on("data", (data: Buffer) => {
    output += data.toString();
  });
  child.stderr.on("data", (data: Buffer) => {
    errors += data.toString();
  });
  const [code, signal] = await once(child, "close");
  const redact = (text: string): string =>
    bearerToken === undefined
      ? text
      : text.replaceAll(bearerToken, "[redacted]");
  if (code !== 0)
    throw new Error(
      `Codex MCP driver failed (${code ?? signal}): ${redact(errors)}\n${redact(output)}`,
    );
  return redact(output);
};

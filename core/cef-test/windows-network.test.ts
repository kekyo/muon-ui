// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { spawn } from "node:child_process";
import { once } from "node:events";
import { networkInterfaces } from "node:os";

import type {
  RemoteAgent,
  RemoteManagedProcessLaunchOptions,
} from "agent-rover";
import { expect, it } from "vitest";

import { discoverWindowsTestNetwork } from "./plugin-e2e/windows-network.js";

it("discovers the selected machine and the HTTP return route, then releases its probe", async () => {
  let released = false;
  const agent = {
    processes: {
      launchManaged: async (options: RemoteManagedProcessLaunchOptions) => {
        const child = spawn(process.execPath, [...(options.arguments ?? [])]);
        let stdout = "";
        let stderr = "";
        child.stdout.on("data", (chunk) => {
          stdout += String(chunk);
        });
        child.stderr.on("data", (chunk) => {
          stderr += String(chunk);
        });
        return {
          stdoutText: async () => stdout,
          stderrText: async () => stderr,
          rootSnapshot: async () => ({ running: child.exitCode === null }),
          releaseAsync: async () => {
            const closed = once(child, "close");
            child.kill();
            await closed;
            released = true;
          },
        };
      },
    },
  } as unknown as RemoteAgent;
  const result = await discoverWindowsTestNetwork(agent);
  const localAddresses = Object.values(networkInterfaces()).flatMap((entries) =>
    (entries ?? [])
      .filter((entry) => entry.family === "IPv4")
      .map((entry) => entry.address),
  );
  expect(localAddresses).toContain(result.cdpHost);
  expect(localAddresses).toContain(result.httpHost);
  expect(released).toBe(true);
});

it("releases a probe when startup fails", async () => {
  let released = false;
  const agent = {
    processes: {
      launchManaged: async () => ({
        stdoutText: async () => "",
        stderrText: async () => "probe startup failed",
        rootSnapshot: async () => ({ running: false }),
        releaseAsync: async () => {
          released = true;
        },
      }),
    },
  } as unknown as RemoteAgent;
  await expect(discoverWindowsTestNetwork(agent)).rejects.toThrow(
    "probe startup failed",
  );
  expect(released).toBe(true);
});

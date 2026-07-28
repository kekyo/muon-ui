// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { describe, expect, it } from "vitest";

import {
  cdpCommandTimeoutMs,
  connectToMuonCdp,
  MUON_PORT,
  startDebugMuonWithNodeProject,
  stopMuon,
} from "./shared.js";
import { requireWindowsRemoteContext } from "./windows-context.js";

describe("Windows Node sidecar job lifetime", { concurrent: false }, () => {
  it("terminates every Node sidecar when only the Muon parent is killed", async () => {
    const context = requireWindowsRemoteContext();
    const running = await startDebugMuonWithNodeProject({
      nodeProject: context.runtime.nodeProjectDirectory,
    });
    let driver: Awaited<ReturnType<typeof connectToMuonCdp>> | undefined =
      undefined;
    let parentKilled = false;
    try {
      driver = await connectToMuonCdp({
        port: MUON_PORT,
        timeoutMs: cdpCommandTimeoutMs,
      });
      await driver.navigate(
        "data:text/html,<title>muon Windows Node job test</title>",
        cdpCommandTimeoutMs,
      );
      const sidecarProcessIds = await driver.evaluate<readonly number[]>(
        `(async () => {
          const nodes = await Promise.all([
            window.muon.node.createNode(),
            window.muon.node.createNode(),
          ]);
          const modules = await Promise.all(
            nodes.map(async (node) =>
              await node.importModule("./backend.mjs")
            )
          );
          window.__windowsNodeInstances = nodes;
          window.__windowsNodeModules = modules;
          return await Promise.all(
            modules.map(async (module) => await module.processId())
          );
        })()`,
      );

      expect(sidecarProcessIds).toHaveLength(2);
      expect(sidecarProcessIds[0]).not.toBe(sidecarProcessIds[1]);
      for (const sidecarProcessId of sidecarProcessIds) {
        const snapshot =
          await context.agent.processes.snapshot(sidecarProcessId);
        expect(snapshot.running).toBe(true);
      }

      const rootProcessId = running.process.pid;
      if (rootProcessId === undefined) {
        throw new Error("Windows Muon parent process id is unavailable");
      }
      await context.agent.processes.kill(rootProcessId);
      parentKilled = true;
      await context.agent.processes.waitForExit(rootProcessId, {
        intervalMs: 100,
        message: "Muon parent did not exit after a direct process kill",
        timeoutMs: 10000,
      });

      const sidecarSnapshots = await Promise.all(
        sidecarProcessIds.map(
          async (sidecarProcessId) =>
            await context.agent.processes.waitForExit(sidecarProcessId, {
              intervalMs: 100,
              message: `Node sidecar ${String(
                sidecarProcessId,
              )} survived its Muon job owner`,
              timeoutMs: 10000,
            }),
        ),
      );
      expect(sidecarSnapshots.map((snapshot) => snapshot.running)).toEqual([
        false,
        false,
      ]);
    } finally {
      if (parentKilled) {
        driver?.close();
        await stopMuon(running, undefined);
      } else {
        await stopMuon(running, driver);
      }
    }
  });
});

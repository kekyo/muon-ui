// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import type { ChildProcess } from "node:child_process";
import { once } from "node:events";
import { cp } from "node:fs/promises";
import { createConnection } from "node:net";
import { resolve } from "node:path";

import { expect, it } from "vitest";

import {
  MUON_APP_URL,
  MUON_PORT,
  cdpCommandTimeoutMs,
  connectToMuonCdp,
  execFileAsync,
  getBundledNodeExecutable,
  join,
  listProcessGroupCommandLines,
  mkdtemp,
  rm,
  startDebugMuonWithNodeProject,
  stopMuon,
  tmpdir,
} from "./shared.js";
import type { CdpDriver, RunningMuon } from "./shared.js";
import { isWindowsRemoteE2e } from "./windows-context.js";

const linuxIt =
  process.platform === "linux" && !isWindowsRemoteE2e() ? it : it.skip;
const expressFixtureDirectory = resolve("test/fixtures/node-express-project");
const nodeBridgeCommandMarker = "node-bridge.mjs";
const forcedTerminationWarning =
  "Node sidecar did not stop gracefully; terminating it";
const forcedKillWarning = "Node sidecar did not terminate; killing it";

interface ExpressProbeResult {
  readonly contentType: string | null;
  readonly descriptorProcessId: number;
  readonly executablePath: string;
  readonly framework: string;
  readonly listeningAfterStop: boolean;
  readonly port: number;
  readonly releasedValueType: string;
  readonly responseProcessId: number;
  readonly status: number;
  readonly value: string;
}

const requireProcessGroupId = (running: RunningMuon): number => {
  const processGroupId = running.process.pid;
  if (processGroupId === undefined) {
    throw new Error("Muon process group id is unavailable");
  }
  return processGroupId;
};

const requireLocalMuonProcess = (running: RunningMuon): ChildProcess => {
  if (running.remoteWindows !== undefined || !("stderr" in running.process)) {
    throw new Error("Expected a local Muon child process");
  }
  return running.process;
};

const expectLoopbackPortUnavailable = async (port: number): Promise<void> => {
  // stopMuon waits for process exit, so one connection attempt is conclusive.
  await new Promise<void>((resolvePromise, rejectPromise) => {
    const socket = createConnection({ host: "127.0.0.1", port });
    let settled = false;
    const settle = (operation: () => void): void => {
      if (settled) {
        return;
      }
      settled = true;
      socket.destroy();
      operation();
    };

    socket.once("connect", () => {
      settle(() => {
        rejectPromise(
          new Error(`Express listener remained available on port ${port}`),
        );
      });
    });
    socket.once("error", (error) => {
      settle(() => {
        if ("code" in error && error.code === "ECONNREFUSED") {
          resolvePromise();
          return;
        }
        rejectPromise(error);
      });
    });
    socket.setTimeout(cdpCommandTimeoutMs, () => {
      settle(() => {
        rejectPromise(
          new Error(`Timed out checking Express listener on port ${port}`),
        );
      });
    });
  });
};

linuxIt(
  "serves a Muon asset request from an independent Express project",
  async () => {
    const temporaryDirectory = await mkdtemp(
      join(tmpdir(), "muon-node-express-"),
    );
    const nodeProject = join(temporaryDirectory, "project");
    try {
      await cp(expressFixtureDirectory, nodeProject, { recursive: true });
      await execFileAsync(
        "npm",
        ["ci", "--offline", "--ignore-scripts", "--omit=dev"],
        { cwd: nodeProject },
      );

      const running = await startDebugMuonWithNodeProject({
        nodeProject,
        environment: {
          MUON_NODE_EXECUTABLE: join(temporaryDirectory, "unavailable-node"),
          PATH: join(temporaryDirectory, "unavailable-path"),
        },
        browserPluginAllowPatterns: ["asset://main/**"],
        networkAllowPatterns: [
          "asset://main/**",
          "http://127.0.0.1:*/muon-node-e2e/**",
        ],
        networkLocalAccess: {
          loopbackOrigins: [{ scheme: "asset", domain: "main" }],
          localNetworkOrigins: [{ scheme: "asset", domain: "main" }],
        },
      });
      let driver: CdpDriver | undefined = undefined;
      try {
        driver = await connectToMuonCdp({
          port: MUON_PORT,
          timeoutMs: cdpCommandTimeoutMs,
        });
        await driver.navigate(MUON_APP_URL, cdpCommandTimeoutMs);

        const expectedValue = "muon express sidecar";
        const result = await driver.evaluate<ExpressProbeResult>(`(async () => {
          const backend = await window.muon.node.importModule(".");
          const result = {};
          try {
            const port = await backend.startServer();
            const descriptorProcessId = await backend.processId();
            const response = await fetch(
              "http://127.0.0.1:" + port +
                "/muon-node-e2e/status/" +
                encodeURIComponent(${JSON.stringify(expectedValue)})
            );
            const body = await response.json();
            Object.assign(result, {
              contentType: response.headers.get("content-type"),
              descriptorProcessId,
              executablePath: await backend.executablePath(),
              framework: body.framework,
              port,
              responseProcessId: body.processId,
              status: response.status,
              value: body.value,
            });
          } finally {
            try {
              await backend.stopServer();
              result.listeningAfterStop = await backend.isListening();
            } finally {
              result.releasedValueType = typeof (await backend.$release());
            }
          }
          return result;
        })()`);

        expect(result.status).toBe(200);
        expect(result.contentType).toMatch(/^application\/json(?:;|$)/);
        expect(result.executablePath).toBe(getBundledNodeExecutable(running));
        expect(result.framework).toBe("express");
        expect(result.value).toBe(expectedValue);
        expect(result.port).toBeGreaterThan(0);
        expect(result.port).toBeLessThanOrEqual(65535);
        expect(result.descriptorProcessId).toBeGreaterThan(0);
        expect(result.responseProcessId).toBe(result.descriptorProcessId);
        expect(result.listeningAfterStop).toBe(false);
        expect(result.releasedValueType).toBe("undefined");
      } catch (error) {
        throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
      } finally {
        await stopMuon(running, driver);
      }
    } finally {
      await rm(temporaryDirectory, { recursive: true, force: true });
    }
  },
);

linuxIt(
  "terminates the Node sidecar when an Express listener remains open",
  async () => {
    const temporaryDirectory = await mkdtemp(
      join(tmpdir(), "muon-node-express-lifecycle-"),
    );
    const nodeProject = join(temporaryDirectory, "project");
    try {
      await cp(expressFixtureDirectory, nodeProject, { recursive: true });
      await execFileAsync(
        "npm",
        ["ci", "--offline", "--ignore-scripts", "--omit=dev"],
        { cwd: nodeProject },
      );

      const running = await startDebugMuonWithNodeProject({
        nodeProject,
        browserPluginAllowPatterns: ["asset://main/**"],
      });
      let driver: CdpDriver | undefined = undefined;
      let stopped = false;
      try {
        const processGroupId = requireProcessGroupId(running);
        driver = await connectToMuonCdp({
          port: MUON_PORT,
          timeoutMs: cdpCommandTimeoutMs,
        });
        await driver.navigate(MUON_APP_URL, cdpCommandTimeoutMs);

        const port = await driver.evaluate<number>(`(async () => {
          const backend = await window.muon.node.importModule(".");
          return await backend.startServer();
        })()`);
        expect(port).toBeGreaterThan(0);
        expect(
          (await listProcessGroupCommandLines(processGroupId)).some((line) =>
            line.includes(nodeBridgeCommandMarker),
          ),
        ).toBe(true);

        const processCloseOperation = once(
          requireLocalMuonProcess(running),
          "close",
        );
        await Promise.all([stopMuon(running, driver), processCloseOperation]);
        stopped = true;
        driver = undefined;

        expect(running.stderr).not.toContain(forcedTerminationWarning);
        expect(running.stderr).not.toContain(forcedKillWarning);
        expect(
          (await listProcessGroupCommandLines(processGroupId)).some((line) =>
            line.includes(nodeBridgeCommandMarker),
          ),
        ).toBe(false);
        await expectLoopbackPortUnavailable(port);
      } catch (error) {
        throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
      } finally {
        if (!stopped) {
          await stopMuon(running, driver);
        }
      }
    } finally {
      await rm(temporaryDirectory, { recursive: true, force: true });
    }
  },
);

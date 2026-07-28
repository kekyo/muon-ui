// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { cp } from "node:fs/promises";
import { createConnection } from "node:net";

import { expect, it } from "vitest";

import {
  MUON_APP_URL,
  MUON_PORT,
  cdpCommandTimeoutMs,
  connectToMuonCdp,
  execFileAsync,
  getBundledNodeExecutable,
  getNodeExpressProjectFixtureDirectory,
  getNodeSidecarCommandMarker,
  expectProcessExitCode,
  join,
  listMuonProcessCommandLines,
  mkdtemp,
  processExitTimeoutMs,
  rm,
  startDebugMuonWithNodeProject,
  stopMuon,
  tmpdir,
  waitForProcessExit,
} from "./shared.js";
import type { CdpDriver } from "./shared.js";
import {
  isWindowsRemoteE2e,
  requireWindowsRemoteContext,
} from "./windows-context.js";

const nodeIt =
  process.platform === "linux" || isWindowsRemoteE2e() ? it : it.skip;
const expressFixtureDirectory = getNodeExpressProjectFixtureDirectory();
const nodeSidecarCommandMarker = getNodeSidecarCommandMarker();
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

const expectLoopbackPortUnavailable = async (port: number): Promise<void> => {
  // stopMuon waits for process exit, so one connection attempt is conclusive.
  await new Promise<void>((resolvePromise, rejectPromise) => {
    const host = isWindowsRemoteE2e()
      ? requireWindowsRemoteContext().httpHost
      : "127.0.0.1";
    const socket = createConnection({ host, port });
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

nodeIt(
  "serves a Muon asset request from an independent Express project",
  async () => {
    const temporaryDirectory = await mkdtemp(
      join(tmpdir(), "muon-node-express-"),
    );
    const nodeProject = isWindowsRemoteE2e()
      ? expressFixtureDirectory
      : join(temporaryDirectory, "project");
    try {
      if (!isWindowsRemoteE2e()) {
        await cp(expressFixtureDirectory, nodeProject, { recursive: true });
        await execFileAsync(
          "npm",
          ["ci", "--offline", "--ignore-scripts", "--omit=dev"],
          { cwd: nodeProject },
        );
      }

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
          const node = await window.muon.node.createNode();
          const backend = await node.importModule(".");
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
              result.releasedValueType = typeof (await node.release());
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

nodeIt(
  "terminates the Node sidecar when an Express listener remains open",
  async () => {
    const temporaryDirectory = await mkdtemp(
      join(tmpdir(), "muon-node-express-lifecycle-"),
    );
    const nodeProject = isWindowsRemoteE2e()
      ? expressFixtureDirectory
      : join(temporaryDirectory, "project");
    try {
      if (!isWindowsRemoteE2e()) {
        await cp(expressFixtureDirectory, nodeProject, { recursive: true });
        await execFileAsync(
          "npm",
          ["ci", "--offline", "--ignore-scripts", "--omit=dev"],
          { cwd: nodeProject },
        );
      }

      const running = await startDebugMuonWithNodeProject({
        nodeProject,
        browserPluginAllowPatterns: ["asset://main/**"],
      });
      let driver: CdpDriver | undefined = undefined;
      let stopped = false;
      try {
        driver = await connectToMuonCdp({
          port: MUON_PORT,
          timeoutMs: cdpCommandTimeoutMs,
        });
        await driver.navigate(MUON_APP_URL, cdpCommandTimeoutMs);

        const sidecar = await driver.evaluate<{
          readonly port: number;
          readonly processId: number;
        }>(`(async () => {
          window.__expressNode = await window.muon.node.createNode();
          const backend = await window.__expressNode.importModule(".");
          return {
            port: await backend.startServer(),
            processId: await backend.processId(),
          };
        })()`);
        expect(sidecar.port).toBeGreaterThan(0);
        expect(
          (await listMuonProcessCommandLines(running)).some((line) =>
            line.includes(nodeSidecarCommandMarker),
          ),
        ).toBe(true);

        const processCloseOperation = waitForProcessExit(
          running,
          processExitTimeoutMs,
        );
        await Promise.all([stopMuon(running, driver), processCloseOperation]);
        stopped = true;
        driver = undefined;

        await expectProcessExitCode(running, 0);
        expect(running.stderr).not.toContain(forcedTerminationWarning);
        expect(running.stderr).not.toContain(forcedKillWarning);
        if (isWindowsRemoteE2e()) {
          await requireWindowsRemoteContext().agent.processes.waitForExit(
            sidecar.processId,
            {
              intervalMs: 100,
              message: `Node sidecar ${String(
                sidecar.processId,
              )} survived Muon shutdown`,
              timeoutMs: processExitTimeoutMs,
            },
          );
        } else {
          expect(
            (await listMuonProcessCommandLines(running)).some((line) =>
              line.includes(nodeSidecarCommandMarker),
            ),
          ).toBe(false);
        }
        await expectLoopbackPortUnavailable(sidecar.port);
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

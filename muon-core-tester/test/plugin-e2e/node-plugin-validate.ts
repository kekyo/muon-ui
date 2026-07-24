// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { resolve } from "node:path";

import { expect, it } from "vitest";

import {
  MUON_APP_URL,
  MUON_PORT,
  access,
  cdpCommandTimeoutMs,
  connectToMuonCdp,
  constants,
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

const localIt = isWindowsRemoteE2e() ? it.skip : it;
const nodeProjectDirectory = resolve("test/fixtures/node-project");
const nodeBridgeCommandMarker = "node-bridge.mjs";
const nodeImportCapabilityId = "node-import-validate-e2e";
const nodeImportFunctionPath = "muon.node.importModule";

const requireProcessGroupId = (running: RunningMuon): number => {
  const processGroupId = running.process.pid;
  if (processGroupId === undefined) {
    throw new Error("Muon process group id is unavailable");
  }
  return processGroupId;
};

const connectToValidateTestPage = async (): Promise<CdpDriver> => {
  const driver = await connectToMuonCdp({
    port: MUON_PORT,
    timeoutMs: cdpCommandTimeoutMs,
  });
  await driver.navigate(MUON_APP_URL, cdpCommandTimeoutMs);
  return driver;
};

localIt(
  "does not allow a validate-mode capability call to start the Node sidecar",
  async () => {
    const markerDirectory = await mkdtemp(
      join(tmpdir(), "muon-node-validate-"),
    );
    const markerPath = join(markerDirectory, "starts.txt");
    try {
      const running = await startDebugMuonWithNodeProject({
        nodeProject: nodeProjectDirectory,
        environment: {
          MUON_NODE_TEST_START_MARKER: markerPath,
        },
        browserPluginAllowPatterns: null,
        pluginCapabilities: [
          {
            id: nodeImportCapabilityId,
            allow: [nodeImportFunctionPath],
          },
        ],
      });
      let driver: CdpDriver | undefined = undefined;
      try {
        driver = await connectToValidateTestPage();
        await expect(driver.evaluate("typeof window.muon")).resolves.toBe(
          "undefined",
        );
        await expect(
          driver.evaluate("typeof globalThis.__muon_plugin_call"),
        ).resolves.toBe("function");
        await expect(access(markerPath, constants.F_OK)).rejects.toThrow();

        const processGroupId = requireProcessGroupId(running);
        const commandLinesBeforeCall =
          await listProcessGroupCommandLines(processGroupId);
        expect(
          commandLinesBeforeCall.some((line) =>
            line.includes(nodeBridgeCommandMarker),
          ),
        ).toBe(false);

        const outcome = await driver.evaluate<{
          readonly error: string;
          readonly ok: boolean;
        }>(`(async () => {
          try {
            await globalThis.__muon_plugin_call(
              ${JSON.stringify(nodeImportCapabilityId)},
              ${JSON.stringify(nodeImportFunctionPath)},
              ["./backend.mjs"]
            );
            return { ok: true, error: "" };
          } catch (error) {
            return {
              ok: false,
              error: String(error && error.message ? error.message : error),
            };
          }
        })()`);

        const commandLinesAfterCall =
          await listProcessGroupCommandLines(processGroupId);
        expect(
          commandLinesAfterCall.some((line) =>
            line.includes(nodeBridgeCommandMarker),
          ),
        ).toBe(false);
        await expect(access(markerPath, constants.F_OK)).rejects.toThrow();
        expect(outcome.ok).toBe(false);
        expect(outcome.error).toBe(
          "Node runtime is unavailable in validate mode",
        );
      } catch (error) {
        if (error instanceof Error) {
          error.message = `${error.message}\nMuon stderr:\n${running.stderr}`;
          throw error;
        }
        throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
      } finally {
        await stopMuon(running, driver);
      }
    } finally {
      await rm(markerDirectory, { recursive: true, force: true });
    }
  },
);

// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

import { cp } from "node:fs/promises";
import { resolve } from "node:path";

import { expect, it } from "vitest";

import {
  MUON_APP_URL,
  MUON_PORT,
  cdpCommandTimeoutMs,
  connectToMuonCdp,
  execFileAsync,
  join,
  mkdtemp,
  rm,
  startDebugMuonWithNodeProject,
  stopMuon,
  tmpdir,
} from "./shared.js";
import type { CdpDriver } from "./shared.js";
import { isWindowsRemoteE2e } from "./windows-context.js";

const linuxIt =
  process.platform === "linux" && !isWindowsRemoteE2e() ? it : it.skip;
const expressFixtureDirectory = resolve("test/fixtures/node-express-project");

interface ExpressProbeResult {
  readonly contentType: string | null;
  readonly descriptorProcessId: number;
  readonly framework: string;
  readonly listeningAfterStop: boolean;
  readonly port: number;
  readonly releasedValueType: string;
  readonly responseProcessId: number;
  readonly status: number;
  readonly value: string;
}

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
        environment: { MUON_NODE_EXECUTABLE: process.execPath },
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

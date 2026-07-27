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
  readFile,
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

const requireProcessGroupId = (running: RunningMuon): number => {
  const processGroupId = running.process.pid;
  if (processGroupId === undefined) {
    throw new Error("Muon process group id is unavailable");
  }
  return processGroupId;
};

const readMarkerLines = async (path: string): Promise<string[]> =>
  (await readFile(path, "utf8")).split("\n").filter((line) => line !== "");

const connectToValidateTestPage = async (): Promise<CdpDriver> => {
  const driver = await connectToMuonCdp({
    port: MUON_PORT,
    timeoutMs: cdpCommandTimeoutMs,
  });
  await driver.navigate(MUON_APP_URL, cdpCommandTimeoutMs);
  return driver;
};

localIt(
  "runs a Node instance through the validate-mode virtual module facade",
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
        const nodeApiDescriptor = await driver.evaluate<{
          readonly configurable: boolean;
          readonly enumerable: boolean;
          readonly frozen: boolean;
          readonly writable: boolean;
        }>(`(() => {
          const descriptor = Object.getOwnPropertyDescriptor(
            globalThis,
            "__muon_node_api"
          );
          return {
            configurable: descriptor.configurable,
            enumerable: descriptor.enumerable,
            frozen: Object.isFrozen(descriptor.value),
            writable: descriptor.writable,
          };
        })()`);
        expect(nodeApiDescriptor).toEqual({
          configurable: false,
          enumerable: false,
          frozen: true,
          writable: false,
        });
        const internalCapabilityError = await driver.evaluate<string>(
          `(async () => {
            try {
              await globalThis.__muon_plugin_call(
                "__muon_node_internal",
                "muon.node.createNode",
                [""]
              );
              return "";
            } catch (error) {
              return String(
                error instanceof Error ? error.message : error
              );
            }
          })()`,
        );
        expect(internalCapabilityError).toContain("cannot be called directly");
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
          readonly callbackJson: {
            readonly copied: boolean;
            readonly original: unknown;
            readonly returned: unknown;
          };
          readonly hasUnsupportedExport: boolean;
          readonly invalidJsonError: string;
          readonly jsonValue: {
            readonly copied: boolean;
            readonly original: unknown;
            readonly returned: unknown;
          };
          readonly processId: number;
          readonly reservedTags: readonly unknown[];
          readonly state: readonly number[];
        }>(`(async () => {
          const node = await globalThis.__muon_node_api.createNode();
          try {
            const backend = await node.importModule("./backend.mjs");

            const jsonArgument = {
              nested: {
                items: ["validate", { source: "renderer" }],
              },
            };
            const jsonResult = await backend.mutateJsonValue(jsonArgument);

            const callbackArgument = {
              nested: {
                items: [
                  { source: "node" },
                  ["validate", 42, false, null],
                ],
              },
            };
            const callbackResult = await backend.invokeCallback(
              async (value) => {
                value.nested.items[0].source = "validate callback";
                return {
                  received: value,
                  response: ["callback", { accepted: true }],
                };
              },
              callbackArgument
            );

            const reservedTags = await Promise.all([
              { kind: "i64", value: "123" },
              { kind: "buffer", data: "not-a-buffer" },
              { kind: "function", handle: 123 },
              { kind: "json", value: { nested: true } },
            ].map(async (value) => await backend.echo(value)));

            let invalidJsonError = "";
            try {
              await backend.echo({ nested: 1n });
            } catch (error) {
              invalidJsonError = String(
                error instanceof Error ? error.message : error
              );
            }

            return {
              callbackJson: {
                copied:
                  callbackArgument !== callbackResult.received &&
                  callbackArgument.nested !== callbackResult.received.nested,
                original: callbackArgument,
                returned: callbackResult,
              },
              hasUnsupportedExport:
                Object.hasOwn(backend, "unsupportedExport"),
              invalidJsonError,
              jsonValue: {
                copied:
                  jsonArgument !== jsonResult &&
                  jsonArgument.nested !== jsonResult.nested,
                original: jsonArgument,
                returned: jsonResult,
              },
              processId: await backend.processId(),
              reservedTags,
              state: [
                await backend.incrementState(),
                await backend.incrementState(),
              ],
            };
          } finally {
            await node.release();
          }
        })()`);

        const commandLinesAfterCall =
          await listProcessGroupCommandLines(processGroupId);
        expect(
          commandLinesAfterCall.filter((line) =>
            line.includes(nodeBridgeCommandMarker),
          ),
        ).toHaveLength(0);
        expect(await readMarkerLines(markerPath)).toEqual([
          String(outcome.processId),
        ]);
        expect(outcome.callbackJson).toEqual({
          copied: true,
          original: {
            nested: {
              items: [{ source: "node" }, ["validate", 42, false, null]],
            },
          },
          returned: {
            received: {
              nested: {
                items: [
                  { source: "validate callback" },
                  ["validate", 42, false, null],
                ],
              },
            },
            response: ["callback", { accepted: true }],
          },
        });
        expect(outcome.hasUnsupportedExport).toBe(false);
        expect(outcome.invalidJsonError).toMatch(/bigint|JSON|unsupported/iu);
        expect(outcome.jsonValue).toEqual({
          copied: true,
          original: {
            nested: {
              items: ["validate", { source: "renderer" }],
            },
          },
          returned: {
            nested: {
              items: ["validate", { source: "node" }],
            },
            nodeOnly: ["added", { accepted: true }],
          },
        });
        expect(outcome.processId).toBeGreaterThan(0);
        expect(outcome.reservedTags).toEqual([
          { kind: "i64", value: "123" },
          { kind: "buffer", data: "not-a-buffer" },
          { kind: "function", handle: 123 },
          { kind: "json", value: { nested: true } },
        ]);
        expect(outcome.state).toEqual([1, 2]);
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

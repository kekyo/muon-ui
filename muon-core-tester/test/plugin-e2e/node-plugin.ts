// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { expect, it } from "vitest";

import {
  MUON_PORT,
  access,
  cdpCommandTimeoutMs,
  connectToMuonCdp,
  constants,
  evaluateRejection,
  getBundledNodeExecutable,
  getNodeProjectFixtureDirectory,
  getNodeSidecarCommandMarker,
  join,
  listMuonProcessCommandLines,
  mkdtemp,
  openPopupTarget,
  readFile,
  realpath,
  rename,
  rm,
  startDebugMuonWithNodeProject,
  stopMuon,
  tmpdir,
  withMuon,
} from "./shared.js";
import type { CdpDriver, RunningMuon } from "./shared.js";

const nodeProjectDirectory = getNodeProjectFixtureDirectory();
const nodeSidecarCommandMarker = getNodeSidecarCommandMarker();
const nodeIt = it;

const connectToNodeTestPage = async (): Promise<CdpDriver> => {
  const driver = await connectToMuonCdp({
    port: MUON_PORT,
    timeoutMs: cdpCommandTimeoutMs,
  });
  await driver.navigate(
    "data:text/html,<title>muon node test</title>",
    cdpCommandTimeoutMs,
  );
  return driver;
};

const readMarkerLines = async (path: string): Promise<string[]> =>
  (await readFile(path, "utf8")).split("\n").filter((line) => line !== "");

const withNodeRuntime = async (
  environment: NodeJS.ProcessEnv,
  run: (driver: CdpDriver, running: RunningMuon) => Promise<void>,
  browserPluginAllowPatterns: string[] | null = ["asset://main/**", "data:**"],
): Promise<void> => {
  const running = await startDebugMuonWithNodeProject({
    nodeProject: nodeProjectDirectory,
    environment,
    browserPluginAllowPatterns,
  });
  let driver: CdpDriver | undefined = undefined;
  try {
    driver = await connectToNodeTestPage();
    await run(driver, running);
  } catch (error) {
    if (error instanceof Error) {
      error.message = `${error.message}\nMuon stderr:\n${running.stderr}`;
      throw error;
    }
    throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
  } finally {
    await stopMuon(running, driver);
  }
};

nodeIt(
  "does not expose muon.node without a top-level node config",
  async () => {
    await withMuon([], async (driver) => {
      await expect(driver.evaluate("typeof window.muon.node")).resolves.toBe(
        "undefined",
      );
    });
  },
);

nodeIt(
  "loads metadata in validate mode without starting the Node sidecar",
  async () => {
    await withNodeRuntime(
      {},
      async (driver, running) => {
        await expect(driver.evaluate("typeof window.muon")).resolves.toBe(
          "undefined",
        );
        const commandLines = await listMuonProcessCommandLines(running);
        expect(
          commandLines.some((line) => line.includes(nodeSidecarCommandMarker)),
        ).toBe(false);
      },
      null,
    );
  },
);

nodeIt(
  "creates isolated Node sidecars and releases each instance independently",
  async () => {
    const markerDirectory = await mkdtemp(join(tmpdir(), "muon-node-start-"));
    const markerPath = join(markerDirectory, "starts.txt");
    const firstExitMarkerPath = join(markerDirectory, "first-exit.txt");
    try {
      await withNodeRuntime(
        {
          MUON_NODE_TEST_START_MARKER: markerPath,
        },
        async (driver, running) => {
          await expect(access(markerPath, constants.F_OK)).rejects.toThrow();
          const commandLinesBeforeImport =
            await listMuonProcessCommandLines(running);
          expect(
            commandLinesBeforeImport.some((line) =>
              line.includes(nodeSidecarCommandMarker),
            ),
          ).toBe(false);
          await expect(
            driver.evaluate("typeof window.muon.node.createNode"),
          ).resolves.toBe("function");

          const result = await driver.evaluate<{
            readonly asyncDisposeExposed: boolean;
            readonly concurrentReleaseCompleted: boolean;
            readonly firstProcessId: number;
            readonly firstReleasedError: string;
            readonly firstState: readonly number[];
            readonly pendingError: string;
            readonly secondProcessId: number;
            readonly secondState: readonly number[];
          }>(`(async () => {
            const [firstNode, secondNode] = await Promise.all([
              window.muon.node.createNode(),
              window.muon.node.createNode(),
            ]);
            const [firstBackend, secondBackend] = await Promise.all([
              firstNode.importModule("./backend.mjs"),
              secondNode.importModule("./backend.mjs"),
            ]);
            let notifyPendingStarted = () => undefined;
            const pendingStarted = new Promise((resolve) => {
              notifyPendingStarted = resolve;
            });
            const pendingOutcome = (async () => {
              try {
                await firstBackend.remainPending(async () => {
                  notifyPendingStarted();
                });
                return "";
              } catch (error) {
                return String(error instanceof Error ? error.message : error);
              }
            })();
            try {
              const firstProcessId = await firstBackend.processId();
              const secondProcessId = await secondBackend.processId();
              const firstState = [
                await firstBackend.incrementState(),
                await firstBackend.incrementState(),
              ];
              const secondState = [await secondBackend.incrementState()];
              const asyncDisposeExposed =
                typeof Symbol.asyncDispose !== "symbol" ||
                typeof firstNode[Symbol.asyncDispose] === "function";
              await firstBackend.installExitMarker(
                ${JSON.stringify(firstExitMarkerPath)}
              );
              await pendingStarted;

              let firstReleaseCompleted = false;
              const firstRelease = (async () => {
                await firstNode.release();
                firstReleaseCompleted = true;
              })();
              await firstNode.release();
              const concurrentReleaseCompleted = firstReleaseCompleted;
              await firstRelease;

              let firstReleasedError = "";
              try {
                await firstBackend.incrementState();
              } catch (error) {
                firstReleasedError = String(
                  error instanceof Error ? error.message : error
                );
              }
              secondState.push(await secondBackend.incrementState());
              return {
                asyncDisposeExposed,
                concurrentReleaseCompleted,
                firstProcessId,
                firstReleasedError,
                firstState,
                pendingError: await pendingOutcome,
                secondProcessId,
                secondState,
              };
            } finally {
              if (typeof Symbol.asyncDispose === "symbol") {
                await secondNode[Symbol.asyncDispose]();
              } else {
                await secondNode.release();
              }
            }
          })()`);
          expect(result.asyncDisposeExposed).toBe(true);
          expect(result.firstProcessId).toBeGreaterThan(0);
          expect(result.secondProcessId).toBeGreaterThan(0);
          expect(result.firstProcessId).not.toBe(result.secondProcessId);
          expect(result.firstState).toEqual([1, 2]);
          expect(result.secondState).toEqual([1, 2]);
          expect(result.concurrentReleaseCompleted).toBe(true);
          expect(result.pendingError).toBe(
            "Node instance was released before the request completed",
          );
          expect(result.firstReleasedError).toMatch(/released/iu);
          expect((await readMarkerLines(markerPath)).sort()).toEqual(
            [result.firstProcessId, result.secondProcessId].map(String).sort(),
          );
          await expect(readFile(firstExitMarkerPath, "utf8")).resolves.toBe(
            "exited\n",
          );

          const commandLinesAfterImport =
            await listMuonProcessCommandLines(running);
          expect(
            commandLinesAfterImport.filter((line) =>
              line.includes(nodeSidecarCommandMarker),
            ),
          ).toHaveLength(0);
        },
      );
    } finally {
      await rm(markerDirectory, { recursive: true, force: true });
    }
  },
);

nodeIt(
  "imports built-in modules through the bundled Node runtime",
  async () => {
    await withNodeRuntime({}, async (driver) => {
      const packageJsonPath = join(nodeProjectDirectory, "package.json");
      const packageJson = await driver.evaluate<string>(`(async () => {
        const node = await window.muon.node.createNode();
        try {
          const fs = await node.importModule("node:fs/promises");
          return await fs.readFile(
            ${JSON.stringify(packageJsonPath)},
            "utf8"
          );
        } finally {
          await node.release();
        }
      })()`);
      expect(JSON.parse(packageJson)).toMatchObject({
        name: "muon-node-e2e-project",
        type: "module",
      });
    });
  },
);

nodeIt(
  "creates a descriptor facade and marshals supported scalar and JSON values",
  async () => {
    await withNodeRuntime({}, async (driver) => {
      const values = await driver.evaluate<{
        readonly accessorError: string;
        readonly accessorGetterCalls: number;
        readonly arrayPropertyError: string;
        readonly callback: string;
        readonly callbackAfterInvalidJson: string;
        readonly callbackError: string;
        readonly callbackInvalidJsonError: string;
        readonly callbackJson: {
          readonly copied: boolean;
          readonly original: unknown;
          readonly returned: unknown;
        };
        readonly circularArgumentError: string;
        readonly circularFunctionAvailable: boolean;
        readonly circularResultError: string;
        readonly concurrentReleaseWaited: boolean;
        readonly copied: readonly number[];
        readonly dateArgumentError: string;
        readonly descriptorValue: string;
        readonly fakeArrayBufferError: string;
        readonly fakeArrayBufferRecovery: string;
        readonly frozen: boolean;
        readonly hasUnsupportedExport: boolean;
        readonly internalFunctionsHidden: boolean;
        readonly jsonValue: {
          readonly copied: boolean;
          readonly nestedCopied: boolean;
          readonly original: unknown;
          readonly returned: unknown;
        };
        readonly negativeZeroError: string;
        readonly nestedBigIntError: string;
        readonly nestedBufferError: string;
        readonly prototypeIsNull: boolean;
        readonly prototypeProperty: unknown;
        readonly releasedError: string;
        readonly reservedTags: readonly unknown[];
        readonly returnedObject: unknown;
        readonly signed: string;
        readonly sparseArrayError: string;
        readonly topLevelArray: unknown;
        readonly unsigned: string;
        readonly undefinedDescriptor: boolean;
        readonly undefinedRoundTrip: string;
      }>(`(async () => {
          const node = await window.muon.node.createNode();
          try {
          const backend =
            await node.importModule("./backend.mjs");

          const jsonArgument = {
            nested: {
              items: [
                "unchanged",
                { source: "renderer" },
              ],
            },
          };
          const jsonResult = await backend.mutateJsonValue(jsonArgument);

          const topLevelArrayArgument = [
            "root",
            { nested: [1, true, null] },
          ];
          const topLevelArrayResult =
            await backend.echo(topLevelArrayArgument);

          const callbackArgument = {
            nested: {
              items: [
                { source: "node" },
                ["preserved", 42, true, null],
              ],
            },
          };
          const callbackResult = await backend.invokeCallback(
            async (value) => {
              value.nested.items[0].source = "renderer callback";
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

          const prototypeArgument = JSON.parse(
            '{"__proto__":{"muonNodeE2ePolluted":true},"safe":true}'
          );
          const nodePrototypeInspection =
            await backend.inspectPrototypeProperty(prototypeArgument);
          const prototypeResult = await backend.echo(prototypeArgument);
          const prototypeProperty = {
            node: nodePrototypeInspection,
            rendererHasOwnPrototypeProperty:
              Object.hasOwn(prototypeResult, "__proto__"),
            rendererObjectPrototypePolluted:
              Object.prototype.muonNodeE2ePolluted === true,
            rendererPrototypeIsObjectPrototype:
              Object.getPrototypeOf(prototypeResult) === Object.prototype,
            rendererPrototypeProperty: prototypeResult["__proto__"],
          };

          let circularArgumentError = "";
          const circularArgument = {};
          circularArgument.self = circularArgument;
          try {
            await backend.echo(circularArgument);
          } catch (error) {
            circularArgumentError = String(
              error instanceof Error ? error.message : error
            );
          }

          const circularFunctionAvailable =
            typeof backend.returnCircularObject === "function";
          let circularResultError = "";
          try {
            await backend.returnCircularObject();
          } catch (error) {
            circularResultError = String(
              error instanceof Error ? error.message : error
            );
          }

          let sparseArrayError = "";
          const sparseArray = [];
          sparseArray.length = 2;
          sparseArray[1] = "present";
          try {
            await backend.echo(sparseArray);
          } catch (error) {
            sparseArrayError = String(
              error instanceof Error ? error.message : error
            );
          }

          let arrayPropertyError = "";
          const arrayWithProperty = ["value"];
          arrayWithProperty.extra = true;
          try {
            await backend.echo(arrayWithProperty);
          } catch (error) {
            arrayPropertyError = String(
              error instanceof Error ? error.message : error
            );
          }

          let nestedBigIntError = "";
          try {
            await backend.echo({ nested: 1n });
          } catch (error) {
            nestedBigIntError = String(
              error instanceof Error ? error.message : error
            );
          }

          let nestedBufferError = "";
          try {
            await backend.echo({
              nested: Uint8Array.from([1, 2, 3]),
            });
          } catch (error) {
            nestedBufferError = String(
              error instanceof Error ? error.message : error
            );
          }

          let dateArgumentError = "";
          try {
            await backend.echo(new Date(0));
          } catch (error) {
            dateArgumentError = String(
              error instanceof Error ? error.message : error
            );
          }

          let accessorGetterCalls = 0;
          let accessorError = "";
          const accessorValue = {};
          Object.defineProperty(accessorValue, "value", {
            enumerable: true,
            get: () => {
              accessorGetterCalls += 1;
              return "unexpected";
            },
          });
          try {
            await backend.echo(accessorValue);
          } catch (error) {
            accessorError = String(
              error instanceof Error ? error.message : error
            );
          }

          let fakeArrayBufferError = "";
          try {
            await backend.echo(Object.create(ArrayBuffer.prototype));
          } catch (error) {
            fakeArrayBufferError = String(
              error instanceof Error ? error.message : error
            );
          }
          const fakeArrayBufferRecovery =
            await backend.echo("after fake ArrayBuffer");

          let callbackInvalidJsonError = "";
          try {
            await backend.invokeCallback(
              async () => ({ nested: 1n }),
              "invalid JSON callback result"
            );
          } catch (error) {
            callbackInvalidJsonError = String(
              error instanceof Error ? error.message : error
            );
          }
          const callbackAfterInvalidJson =
            await backend.invokeCallback(
              async (value) => "after:" + value,
              "invalid JSON"
            );

          let negativeZeroError = "";
          try {
            await backend.echo(-0);
          } catch (error) {
            negativeZeroError = String(
              error instanceof Error ? error.message : error
            );
          }
          const result = {
            accessorError,
            accessorGetterCalls,
            arrayPropertyError,
            callback: await backend.invokeCallback(
              async (value) => "renderer:" + value,
              "callback"
            ),
            callbackAfterInvalidJson,
            callbackError: "",
            callbackInvalidJsonError,
            callbackJson: {
              copied:
                callbackArgument !== callbackResult.received &&
                callbackArgument.nested !== callbackResult.received.nested,
              original: callbackArgument,
              returned: callbackResult,
            },
            circularArgumentError,
            circularFunctionAvailable,
            circularResultError,
            concurrentReleaseWaited: false,
            copied: Array.from(
              await backend.copyBuffer(Uint8Array.from([0, 1, 127, 255]))
            ),
            dateArgumentError,
            descriptorValue: backend.descriptorValue,
            fakeArrayBufferError,
            fakeArrayBufferRecovery,
            frozen: Object.isFrozen(backend),
            hasUnsupportedExport:
              Object.hasOwn(backend, "unsupportedExport"),
            internalFunctionsHidden: [
              "__createNode",
              "__importModule",
              "__call",
              "__releaseModule",
              "__releaseNode",
              "__completeCallback",
            ].every((name) => !Object.hasOwn(window.muon.node, name)),
            jsonValue: {
              copied:
                jsonArgument !== jsonResult,
              nestedCopied:
                jsonArgument.nested !== jsonResult.nested &&
                jsonArgument.nested.items !== jsonResult.nested.items,
              original: jsonArgument,
              returned: jsonResult,
            },
            negativeZeroError,
            nestedBigIntError,
            nestedBufferError,
            prototypeIsNull: Object.getPrototypeOf(backend) === null,
            prototypeProperty,
            releasedError: "",
            reservedTags,
            returnedObject: await backend.returnObject(),
            signed: (await backend.echo(-9223372036854775808n)).toString(),
            sparseArrayError,
            topLevelArray: {
              copied:
                topLevelArrayArgument !== topLevelArrayResult &&
                topLevelArrayArgument[1] !== topLevelArrayResult[1],
              original: topLevelArrayArgument,
              returned: topLevelArrayResult,
            },
            unsigned:
              (await backend.echo(18446744073709551615n)).toString(),
            undefinedDescriptor:
              Object.hasOwn(backend, "undefinedExport") &&
              backend.undefinedExport === undefined,
            undefinedRoundTrip: typeof (
              await backend.invokeCallback((value) => value, undefined)
            ),
          };
          try {
            await backend.invokeCallback(async () => {
              throw new Error("renderer callback failure");
            });
          } catch (error) {
            result.callbackError = String(
              error instanceof Error ? error.message : error
            );
          }
          let firstReleaseSettled = false;
          const firstRelease = backend.$release();
          const observeFirstRelease = (async () => {
            try {
              await firstRelease;
            } finally {
              firstReleaseSettled = true;
            }
          })();
          await backend.$release();
          result.concurrentReleaseWaited = firstReleaseSettled;
          await observeFirstRelease;
          try {
            await backend.echo("released");
          } catch (error) {
            result.releasedError = String(
              error instanceof Error ? error.message : error
            );
          }
          return result;
          } finally {
            await node.release();
          }
        })()`);

      expect(values).toEqual({
        accessorError: expect.stringMatching(
          /JSON|accessor|data property|unsupported/iu,
        ),
        accessorGetterCalls: 0,
        arrayPropertyError: expect.stringMatching(
          /JSON|array|property|unsupported/iu,
        ),
        callback: "renderer:callback",
        callbackAfterInvalidJson: "after:invalid JSON",
        callbackError: "renderer callback failure",
        callbackInvalidJsonError: expect.stringMatching(
          /bigint|JSON|callback|unsupported/iu,
        ),
        callbackJson: {
          copied: true,
          original: {
            nested: {
              items: [{ source: "node" }, ["preserved", 42, true, null]],
            },
          },
          returned: {
            received: {
              nested: {
                items: [
                  { source: "renderer callback" },
                  ["preserved", 42, true, null],
                ],
              },
            },
            response: ["callback", { accepted: true }],
          },
        },
        circularArgumentError: expect.stringMatching(
          /circular|cycle|cyclic|JSON|unsupported/iu,
        ),
        circularFunctionAvailable: true,
        circularResultError: expect.stringMatching(
          /circular|cycle|cyclic|JSON|unsupported/iu,
        ),
        concurrentReleaseWaited: true,
        copied: [0, 1, 127, 255],
        dateArgumentError: expect.stringMatching(
          /JSON|object|plain|unsupported/iu,
        ),
        descriptorValue: "descriptor",
        fakeArrayBufferError: expect.stringMatching(
          /ArrayBuffer|binary|inspect|JSON|unsupported/iu,
        ),
        fakeArrayBufferRecovery: "after fake ArrayBuffer",
        frozen: true,
        hasUnsupportedExport: false,
        internalFunctionsHidden: true,
        jsonValue: {
          copied: true,
          nestedCopied: true,
          original: {
            nested: {
              items: ["unchanged", { source: "renderer" }],
            },
          },
          returned: {
            nested: {
              items: ["unchanged", { source: "node" }],
            },
            nodeOnly: ["added", { accepted: true }],
          },
        },
        negativeZeroError: expect.stringMatching(/finite|negative zero/iu),
        nestedBigIntError: expect.stringMatching(/bigint|JSON|unsupported/iu),
        nestedBufferError: expect.stringMatching(
          /buffer|binary|JSON|unsupported/iu,
        ),
        prototypeIsNull: true,
        prototypeProperty: {
          node: {
            hasOwnPrototypeProperty: true,
            objectPrototypePolluted: false,
            prototypeProperty: {
              muonNodeE2ePolluted: true,
            },
          },
          rendererHasOwnPrototypeProperty: true,
          rendererObjectPrototypePolluted: false,
          rendererPrototypeIsObjectPrototype: true,
          rendererPrototypeProperty: {
            muonNodeE2ePolluted: true,
          },
        },
        releasedError: expect.stringMatching(/released/iu),
        reservedTags: [
          { kind: "i64", value: "123" },
          { kind: "buffer", data: "not-a-buffer" },
          { kind: "function", handle: 123 },
          { kind: "json", value: { nested: true } },
        ],
        returnedObject: { unsupported: true },
        signed: "-9223372036854775808",
        sparseArrayError: expect.stringMatching(
          /array|hole|JSON|unsupported/iu,
        ),
        topLevelArray: {
          copied: true,
          original: ["root", { nested: [1, true, null] }],
          returned: ["root", { nested: [1, true, null] }],
        },
        unsigned: "18446744073709551615",
        undefinedDescriptor: true,
        undefinedRoundTrip: "undefined",
      });
      await expect(
        driver.evaluate(`(async () => {
            const node = await window.muon.node.createNode();
            try {
            const backend =
              await node.importModule("./backend.mjs");
            return await backend.currentWorkingDirectory();
            } finally {
              await node.release();
            }
          })()`),
      ).resolves.toBe(await realpath(nodeProjectDirectory));
    });
  },
);

nodeIt(
  "rejects an oversized renderer callback result without leaving the Node call pending",
  async () => {
    await withNodeRuntime({}, async (driver) => {
      const error = await evaluateRejection(
        driver,
        `(async () => {
            const node = await window.muon.node.createNode();
            try {
            const backend =
              await node.importModule("./backend.mjs");
            return await backend.invokeCallback(
              () => "x".repeat(16 * 1024 * 1024),
              "oversized"
            );
            } finally {
              await node.release();
            }
          })()`,
      );
      expect(error).toMatch(/callback|frame|large|size/iu);
    });
  },
);

nodeIt(
  "keeps renderer callback handles distinct across browser contexts",
  async () => {
    await withNodeRuntime({}, async (driver) => {
      let popupDriver: CdpDriver | undefined = undefined;
      try {
        await driver.evaluate(`(() => {
            let releaseMainCallback = () => undefined;
            let markMainCallbackStarted = () => undefined;
            window.__releaseMainCallback = (value) =>
              releaseMainCallback(value);
            window.__mainCallbackStarted = new Promise((resolve) => {
              markMainCallbackStarted = resolve;
            });
            window.__mainNodeCall = (async () => {
              window.__mainNode = await window.muon.node.createNode();
              const backend =
                await window.__mainNode.importModule("./backend.mjs");
              return await backend.invokeCallback(
                (value) => {
                  markMainCallbackStarted();
                  return new Promise((resolve) => {
                    releaseMainCallback = resolve;
                  });
                },
                "main"
              );
            })();
            return "scheduled";
          })()`);
        await driver.evaluate("window.__mainCallbackStarted");

        const popupTarget = await openPopupTarget(
          driver,
          "data:text/html,muon-node-callback-popup",
        );
        popupDriver = await connectToMuonCdp({
          port: MUON_PORT,
          targetId: popupTarget.id,
          timeoutMs: cdpCommandTimeoutMs,
        });
        await expect(
          popupDriver.evaluate(`(async () => {
              const node = await window.muon.node.createNode();
              try {
              const backend =
                await node.importModule("./backend.mjs");
              return await backend.invokeCallback(
                async (value) => "popup:" + value,
                "popup"
              );
              } finally {
                await node.release();
              }
            })()`),
        ).resolves.toBe("popup:popup");

        await driver.evaluate('window.__releaseMainCallback("main:main")');
        await expect(driver.evaluate("window.__mainNodeCall")).resolves.toBe(
          "main:main",
        );
      } finally {
        await driver.evaluate(`(() => {
            if (typeof window.__releaseMainCallback === "function") {
              window.__releaseMainCallback("main:cleanup");
            }
          })()`);
        await driver.evaluate(`(async () => {
            if (window.__mainNode !== undefined) {
              await window.__mainNode.release();
            }
          })()`);
        popupDriver?.close();
      }
    });
  },
);

nodeIt(
  "releases a context-owned Node instance without affecting another context",
  async () => {
    const markerDirectory = await mkdtemp(
      join(tmpdir(), "muon-node-context-release-"),
    );
    const exitMarkerPath = join(markerDirectory, "popup-exit.txt");
    try {
      await withNodeRuntime({}, async (driver, running) => {
        let popupDriver: CdpDriver | undefined = undefined;
        try {
          const mainProcessId = await driver.evaluate<number>(`(async () => {
              window.__mainNode = await window.muon.node.createNode();
              window.__mainBackend =
                await window.__mainNode.importModule("./backend.mjs");
              window.__popupExitObserved =
                window.__mainBackend.waitForFile(
                  ${JSON.stringify(exitMarkerPath)}
                );
              return await window.__mainBackend.processId();
            })()`);
          const popupTarget = await openPopupTarget(
            driver,
            "data:text/html,muon-node-callback-release-popup",
          );
          popupDriver = await connectToMuonCdp({
            port: MUON_PORT,
            targetId: popupTarget.id,
            timeoutMs: cdpCommandTimeoutMs,
          });
          const popupProcessId = await popupDriver.evaluate<number>(
            `(async () => {
              window.__popupNode = await window.muon.node.createNode();
              const backend =
                await window.__popupNode.importModule("./backend.mjs");
              await backend.installExitMarker(
                ${JSON.stringify(exitMarkerPath)}
              );
              let markPendingStarted = () => undefined;
              const pendingStarted = new Promise((resolve) => {
                markPendingStarted = resolve;
              });
              window.__pendingNodeCall = (async () => {
                try {
                  await backend.remainPending(async () => {
                    markPendingStarted();
                  });
                  return "";
                } catch (error) {
                  return String(
                    error instanceof Error ? error.message : error
                  );
                }
              })();
              await pendingStarted;
              return await backend.processId();
            })()`,
          );
          expect(popupProcessId).not.toBe(mainProcessId);

          await popupDriver.send("Page.close");
          popupDriver.close();
          popupDriver = undefined;

          await expect(
            driver.evaluate("window.__popupExitObserved"),
          ).resolves.toBe(true);
          await expect(readFile(exitMarkerPath, "utf8")).resolves.toBe(
            "exited\n",
          );
          await expect(
            driver.evaluate("window.__mainBackend.incrementState()"),
          ).resolves.toBe(1);
          expect(
            (await listMuonProcessCommandLines(running)).filter((line) =>
              line.includes(nodeSidecarCommandMarker),
            ),
          ).toHaveLength(1);
          await driver.evaluate("window.__mainNode.release()");
          expect(
            (await listMuonProcessCommandLines(running)).filter((line) =>
              line.includes(nodeSidecarCommandMarker),
            ),
          ).toHaveLength(0);
        } finally {
          await driver.evaluate(`(async () => {
              if (window.__mainNode !== undefined) {
                await window.__mainNode.release();
              }
            })()`);
          popupDriver?.close();
        }
      });
    } finally {
      await rm(markerDirectory, { recursive: true, force: true });
    }
  },
);

nodeIt(
  "does not fall back when the bundled Node executable is missing",
  async () => {
    await withNodeRuntime(
      { MUON_NODE_EXECUTABLE: process.execPath },
      async (driver, running) => {
        const executable = getBundledNodeExecutable(running);
        const unavailableExecutable = `${executable}.unavailable-${String(
          process.pid,
        )}`;
        await rename(executable, unavailableExecutable);
        try {
          await expect(
            driver.evaluate("typeof window.muon.node.createNode"),
          ).resolves.toBe("function");
          const error = await evaluateRejection(
            driver,
            "window.muon.node.createNode()",
          );
          expect(error).toContain(executable);
        } finally {
          await rename(unavailableExecutable, executable);
        }
      },
    );
  },
);

nodeIt(
  "isolates a crashed Node sidecar from subsequently created instances",
  async () => {
    const markerDirectory = await mkdtemp(join(tmpdir(), "muon-node-crash-"));
    const markerPath = join(markerDirectory, "starts.txt");
    try {
      await withNodeRuntime(
        {
          MUON_NODE_TEST_START_MARKER: markerPath,
        },
        async (driver) => {
          const result = await driver.evaluate<{
            readonly crashError: string;
            readonly replacementProcessId: number;
            readonly retryError: string;
          }>(`(async () => {
              const crashedNode = await window.muon.node.createNode();
              let replacementNode;
              try {
                const backend =
                  await crashedNode.importModule("./backend.mjs");
                let crashError = "";
                try {
                  await backend.crash();
                } catch (error) {
                  crashError = String(
                    error instanceof Error ? error.message : error
                  );
                }
                let retryError = "";
                try {
                  await crashedNode.importModule("./secondary.mjs");
                } catch (error) {
                  retryError = String(
                    error instanceof Error ? error.message : error
                  );
                }
                replacementNode = await window.muon.node.createNode();
                const replacementBackend =
                  await replacementNode.importModule("./backend.mjs");
                return {
                  crashError,
                  replacementProcessId:
                    await replacementBackend.processId(),
                  retryError,
                };
              } finally {
                await crashedNode.release();
                if (replacementNode !== undefined) {
                  await replacementNode.release();
                }
              }
            })()`);
          expect(result.crashError).not.toBe("");
          expect(result.retryError).not.toBe("");
          expect(result.replacementProcessId).toBeGreaterThan(0);
          expect(await readMarkerLines(markerPath)).toHaveLength(2);
        },
      );
    } finally {
      await rm(markerDirectory, { recursive: true, force: true });
    }
  },
);

nodeIt("stops the Node child before the plugin library unloads", async () => {
  const markerDirectory = await mkdtemp(join(tmpdir(), "muon-node-stop-"));
  const exitMarkerPath = join(markerDirectory, "exit.txt");
  const running = await startDebugMuonWithNodeProject({
    nodeProject: nodeProjectDirectory,
  });
  let driver: CdpDriver | undefined = undefined;
  try {
    driver = await connectToNodeTestPage();
    await expect(
      driver.evaluate<{
        readonly installed: boolean;
        readonly pendingError: string;
      }>(`(async () => {
        window.__node = await window.muon.node.createNode();
        const backend =
          await window.__node.importModule("./backend.mjs");
        const installed = await backend.installExitMarker(
          ${JSON.stringify(exitMarkerPath)}
        );
        let notifyCallbackStarted = () => undefined;
        const callbackStarted = new Promise((resolve) => {
          notifyCallbackStarted = resolve;
        });
        const pendingOutcome = (async () => {
          try {
            await backend.invokeCallback(async () => {
              notifyCallbackStarted();
              await new Promise(() => undefined);
            }, "pending-at-context-release");
            return "";
          } catch (error) {
            return String(error instanceof Error ? error.message : error);
          }
        })();
        await callbackStarted;
        await window.__node.release();
        return {
          installed,
          pendingError: await pendingOutcome,
        };
      })()`),
    ).resolves.toEqual({
      installed: true,
      pendingError: "Node instance was released before the request completed",
    });
  } finally {
    await stopMuon(running, driver);
  }
  try {
    await expect(readFile(exitMarkerPath, "utf8")).resolves.toBe("exited\n");
  } finally {
    await rm(markerDirectory, { recursive: true, force: true });
  }
});

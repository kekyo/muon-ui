// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { realpath } from "node:fs/promises";
import { resolve } from "node:path";

import { expect, it } from "vitest";

import {
  MUON_PORT,
  access,
  cdpCommandTimeoutMs,
  connectToMuonCdp,
  constants,
  evaluateRejection,
  join,
  listProcessGroupCommandLines,
  mkdtemp,
  openPopupTarget,
  readFile,
  rm,
  startDebugMuonWithNodeProject,
  stopMuon,
  tmpdir,
  withMuon,
} from "./shared.js";
import type { CdpDriver, RunningMuon } from "./shared.js";
import { isWindowsRemoteE2e } from "./windows-context.js";

const localIt = isWindowsRemoteE2e() ? it.skip : it;
const nodeProjectDirectory = resolve("test/fixtures/node-project");
const nodeBridgeCommandMarker = "node-bridge.mjs";

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

const requireProcessGroupId = (running: RunningMuon): number => {
  const processGroupId = running.process.pid;
  if (processGroupId === undefined) {
    throw new Error("Muon process group id is unavailable");
  }
  return processGroupId;
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

localIt(
  "does not expose muon.node without a top-level node config",
  async () => {
    await withMuon([], async (driver) => {
      await expect(driver.evaluate("typeof window.muon.node")).resolves.toBe(
        "undefined",
      );
    });
  },
);

localIt(
  "loads metadata in validate mode without starting the Node sidecar",
  async () => {
    await withNodeRuntime(
      { MUON_NODE_EXECUTABLE: process.execPath },
      async (driver, running) => {
        await expect(driver.evaluate("typeof window.muon")).resolves.toBe(
          "undefined",
        );
        const commandLines = await listProcessGroupCommandLines(
          requireProcessGroupId(running),
        );
        expect(
          commandLines.some((line) => line.includes(nodeBridgeCommandMarker)),
        ).toBe(false);
      },
      null,
    );
  },
);

localIt(
  "starts one Node sidecar lazily for concurrent module imports",
  async () => {
    const markerDirectory = await mkdtemp(join(tmpdir(), "muon-node-start-"));
    const markerPath = join(markerDirectory, "starts.txt");
    try {
      await withNodeRuntime(
        {
          MUON_NODE_EXECUTABLE: process.execPath,
          MUON_NODE_TEST_START_MARKER: markerPath,
        },
        async (driver, running) => {
          await expect(access(markerPath, constants.F_OK)).rejects.toThrow();
          const processGroupId = requireProcessGroupId(running);
          const commandLinesBeforeImport =
            await listProcessGroupCommandLines(processGroupId);
          expect(
            commandLinesBeforeImport.some((line) =>
              line.includes(nodeBridgeCommandMarker),
            ),
          ).toBe(false);
          await expect(
            driver.evaluate("typeof window.muon.node.importModule"),
          ).resolves.toBe("function");

          const processIds = await driver.evaluate<number[]>(`(async () => {
            const [backend, secondary] = await Promise.all([
              window.muon.node.importModule("./backend.mjs"),
              window.muon.node.importModule("./secondary.mjs"),
            ]);
            return await Promise.all([
              backend.processId(),
              secondary.processId(),
            ]);
          })()`);
          expect(processIds).toHaveLength(2);
          expect(processIds[0]).toBe(processIds[1]);
          expect(await readMarkerLines(markerPath)).toEqual([
            String(processIds[0]),
          ]);

          const commandLinesAfterImport =
            await listProcessGroupCommandLines(processGroupId);
          expect(
            commandLinesAfterImport.filter((line) =>
              line.includes(nodeBridgeCommandMarker),
            ),
          ).toHaveLength(1);
        },
      );
    } finally {
      await rm(markerDirectory, { recursive: true, force: true });
    }
  },
);

localIt(
  "resolves the Node executable from PATH when no override exists",
  async () => {
    await withNodeRuntime(
      { MUON_NODE_EXECUTABLE: undefined },
      async (driver) => {
        const packageJsonPath = join(nodeProjectDirectory, "package.json");
        const packageJson = await driver.evaluate<string>(`(async () => {
        const fs = await window.muon.node.importModule("node:fs/promises");
        return await fs.readFile(${JSON.stringify(packageJsonPath)}, "utf8");
      })()`);
        expect(JSON.parse(packageJson)).toMatchObject({
          name: "muon-node-e2e-project",
          type: "module",
        });
      },
    );
  },
);

localIt(
  "creates a descriptor facade and marshals supported values and callbacks",
  async () => {
    await withNodeRuntime(
      { MUON_NODE_EXECUTABLE: process.execPath },
      async (driver) => {
        const values = await driver.evaluate<{
          readonly callback: string;
          readonly callbackError: string;
          readonly concurrentReleaseWaited: boolean;
          readonly copied: readonly number[];
          readonly descriptorValue: string;
          readonly frozen: boolean;
          readonly hasUnsupportedExport: boolean;
          readonly internalFunctionsHidden: boolean;
          readonly negativeZeroError: string;
          readonly prototypeIsNull: boolean;
          readonly releasedError: string;
          readonly resultObjectError: string;
          readonly signed: string;
          readonly unsigned: string;
          readonly undefinedDescriptor: boolean;
          readonly undefinedRoundTrip: string;
          readonly unsupportedArgumentError: string;
        }>(`(async () => {
          const backend =
            await window.muon.node.importModule("./backend.mjs");
          let unsupportedArgumentError = "";
          try {
            await backend.echo({ unsupported: true });
          } catch (error) {
            unsupportedArgumentError = String(
              error instanceof Error ? error.message : error
            );
          }
          let resultObjectError = "";
          try {
            await backend.returnObject();
          } catch (error) {
            resultObjectError = String(
              error instanceof Error ? error.message : error
            );
          }
          let negativeZeroError = "";
          try {
            await backend.echo(-0);
          } catch (error) {
            negativeZeroError = String(
              error instanceof Error ? error.message : error
            );
          }
          const result = {
            callback: await backend.invokeCallback(
              async (value) => "renderer:" + value,
              "callback"
            ),
            callbackError: "",
            concurrentReleaseWaited: false,
            copied: Array.from(
              await backend.copyBuffer(Uint8Array.from([0, 1, 127, 255]))
            ),
            descriptorValue: backend.descriptorValue,
            frozen: Object.isFrozen(backend),
            hasUnsupportedExport:
              Object.hasOwn(backend, "unsupportedExport"),
            internalFunctionsHidden: [
              "__importModule",
              "__call",
              "__release",
              "__completeCallback",
            ].every((name) => !Object.hasOwn(window.muon.node, name)),
            negativeZeroError,
            prototypeIsNull: Object.getPrototypeOf(backend) === null,
            releasedError: "",
            resultObjectError,
            signed: (await backend.echo(-9223372036854775808n)).toString(),
            unsigned:
              (await backend.echo(18446744073709551615n)).toString(),
            undefinedDescriptor:
              Object.hasOwn(backend, "undefinedExport") &&
              backend.undefinedExport === undefined,
            undefinedRoundTrip: typeof (
              await backend.invokeCallback((value) => value, undefined)
            ),
            unsupportedArgumentError,
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
        })()`);

        expect(values).toEqual({
          callback: "renderer:callback",
          callbackError: "renderer callback failure",
          concurrentReleaseWaited: true,
          copied: [0, 1, 127, 255],
          descriptorValue: "descriptor",
          frozen: true,
          hasUnsupportedExport: false,
          internalFunctionsHidden: true,
          negativeZeroError: expect.stringMatching(/finite|negative zero/iu),
          prototypeIsNull: true,
          releasedError: expect.stringMatching(/released/iu),
          resultObjectError: expect.stringMatching(/primitive|buffer/iu),
          signed: "-9223372036854775808",
          unsigned: "18446744073709551615",
          undefinedDescriptor: true,
          undefinedRoundTrip: "undefined",
          unsupportedArgumentError: expect.stringMatching(/primitive|buffer/iu),
        });
        await expect(
          driver.evaluate(`(async () => {
            const backend =
              await window.muon.node.importModule("./backend.mjs");
            return await backend.currentWorkingDirectory();
          })()`),
        ).resolves.toBe(await realpath(nodeProjectDirectory));
      },
    );
  },
);

localIt(
  "rejects a relative MUON_NODE_EXECUTABLE without PATH fallback",
  async () => {
    await withNodeRuntime({ MUON_NODE_EXECUTABLE: "node" }, async (driver) => {
      const error = await evaluateRejection(
        driver,
        'window.muon.node.importModule("./backend.mjs")',
      );
      expect(error).toMatch(/MUON_NODE_EXECUTABLE.*absolute/iu);
    });
  },
);

localIt(
  "rejects an oversized renderer callback result without leaving the Node call pending",
  async () => {
    await withNodeRuntime(
      { MUON_NODE_EXECUTABLE: process.execPath },
      async (driver) => {
        const error = await evaluateRejection(
          driver,
          `(async () => {
            const backend =
              await window.muon.node.importModule("./backend.mjs");
            return await backend.invokeCallback(
              () => "x".repeat(16 * 1024 * 1024),
              "oversized"
            );
          })()`,
        );
        expect(error).toMatch(/callback|frame|large|size/iu);
      },
    );
  },
);

localIt(
  "keeps renderer callback handles distinct across browser contexts",
  async () => {
    await withNodeRuntime(
      { MUON_NODE_EXECUTABLE: process.execPath },
      async (driver) => {
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
              const backend =
                await window.muon.node.importModule("./backend.mjs");
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
              const backend =
                await window.muon.node.importModule("./backend.mjs");
              return await backend.invokeCallback(
                async (value) => "popup:" + value,
                "popup"
              );
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
          popupDriver?.close();
        }
      },
    );
  },
);

localIt(
  "rejects a pending Node callback when its renderer context is released",
  async () => {
    await withNodeRuntime(
      { MUON_NODE_EXECUTABLE: process.execPath },
      async (driver) => {
        let popupDriver: CdpDriver | undefined = undefined;
        try {
          const popupTarget = await openPopupTarget(
            driver,
            "data:text/html,muon-node-callback-release-popup",
          );
          popupDriver = await connectToMuonCdp({
            port: MUON_PORT,
            targetId: popupTarget.id,
            timeoutMs: cdpCommandTimeoutMs,
          });
          await popupDriver.evaluate(`(async () => {
            const backend =
              await window.muon.node.importModule("./backend.mjs");
            let markCallbackStarted = () => undefined;
            const callbackStarted = new Promise((resolve) => {
              markCallbackStarted = resolve;
            });
            window.__pendingNodeCallback =
              backend.observeCallbackFailure(async () => {
                markCallbackStarted();
                await new Promise(() => undefined);
              });
            await callbackStarted;
            return "started";
          })()`);

          await popupDriver.send("Page.close");
          popupDriver.close();
          popupDriver = undefined;

          const message = await driver.evaluate<string>(`(async () => {
            const backend =
              await window.muon.node.importModule("./backend.mjs");
            return await backend.waitForObservedCallbackFailure();
          })()`);
          expect(message).toMatch(/context.*released/iu);
        } finally {
          popupDriver?.close();
        }
      },
    );
  },
);

localIt("reports a missing Node executable on the first import", async () => {
  const missingExecutable = join(
    tmpdir(),
    `missing-muon-node-${String(process.pid)}`,
  );
  await withNodeRuntime(
    { MUON_NODE_EXECUTABLE: missingExecutable },
    async (driver) => {
      await expect(
        driver.evaluate("typeof window.muon.node.importModule"),
      ).resolves.toBe("function");
      const error = await evaluateRejection(
        driver,
        'window.muon.node.importModule("./backend.mjs")',
      );
      expect(error).toContain(missingExecutable);
    },
  );
});

localIt(
  "keeps a crashed Node sidecar failure sticky without restart",
  async () => {
    const markerDirectory = await mkdtemp(join(tmpdir(), "muon-node-crash-"));
    const markerPath = join(markerDirectory, "starts.txt");
    try {
      await withNodeRuntime(
        {
          MUON_NODE_EXECUTABLE: process.execPath,
          MUON_NODE_TEST_START_MARKER: markerPath,
        },
        async (driver) => {
          const crashError = await evaluateRejection(
            driver,
            `(async () => {
            const backend =
              await window.muon.node.importModule("./backend.mjs");
            return await backend.crash();
          })()`,
          );
          expect(crashError).not.toBe("");

          const retryError = await evaluateRejection(
            driver,
            'window.muon.node.importModule("./secondary.mjs")',
          );
          expect(retryError).not.toBe("");
          expect(await readMarkerLines(markerPath)).toHaveLength(1);
        },
      );
    } finally {
      await rm(markerDirectory, { recursive: true, force: true });
    }
  },
);

localIt("stops the Node child before the plugin library unloads", async () => {
  const markerDirectory = await mkdtemp(join(tmpdir(), "muon-node-stop-"));
  const exitMarkerPath = join(markerDirectory, "exit.txt");
  const running = await startDebugMuonWithNodeProject({
    nodeProject: nodeProjectDirectory,
    environment: {
      MUON_NODE_EXECUTABLE: process.execPath,
    },
  });
  let driver: CdpDriver | undefined = undefined;
  try {
    driver = await connectToNodeTestPage();
    await expect(
      driver.evaluate(`(async () => {
        const backend =
          await window.muon.node.importModule("./backend.mjs");
        return await backend.installExitMarker(
          ${JSON.stringify(exitMarkerPath)}
        );
      })()`),
    ).resolves.toBe(true);
  } finally {
    await stopMuon(running, driver);
  }
  try {
    await expect(readFile(exitMarkerPath, "utf8")).resolves.toBe("exited\n");
  } finally {
    await rm(markerDirectory, { recursive: true, force: true });
  }
});

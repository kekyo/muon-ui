// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

import { Buffer } from "node:buffer";
import { createHash } from "node:crypto";
import { homedir } from "node:os";

import { expect, it } from "vitest";

import {
  DEBUG_MUON_DIRECTORY,
  MUON_APP_URL,
  MUON_PORT,
  TEST_BROWSER_PLUGIN_ALLOW_PATTERNS,
  TEST_NETWORK_ALLOW_PATTERNS,
  TEST_PLUGIN_ALLOW_PATTERNS,
  access,
  addPopupSizeFeatures,
  appendFile,
  cdpCommandTimeoutMs,
  cleanupWindowsRemoteTestProcesses,
  connectToMuonCdp,
  describeMuonPluginBridge,
  getCurrentTargetIds,
  join,
  mkdir,
  mkdtemp,
  openPopupTarget,
  readFile,
  rm,
  startDebugMuon,
  startHttpServer,
  stopHttpServer,
  stopMuon,
  targetTimeoutMs,
  tmpdir,
  wait,
  waitForDocumentTitle,
  waitForMuonStderr,
  waitForNetworkResponse,
  waitForNewPageTarget,
  waitForPageTargetUrl,
  waitForProcessExit,
  withMuon,
  writeFile,
} from "./shared.js";
import { isWindowsRemoteE2e } from "./windows-context.js";
import type { CdpDriver, RuntimeEvaluateResponse } from "./shared.js";

interface StoredZipEntry {
  name: string;
  content: string;
}

interface RuntimeConsoleAPICalledParams {
  type?: string;
  args?: Array<{
    value?: unknown;
  }>;
}

const getLocalBootstrapCefLogPath = (
  localStateHome: string | undefined,
): string => {
  const stateHome =
    localStateHome ??
    (process.env.XDG_STATE_HOME?.length
      ? process.env.XDG_STATE_HOME
      : join(homedir(), ".local", "state"));
  return join(stateHome, "muon-bootstrap", "profile", "muon-cef.log");
};

const waitForConsoleMessage = async (
  driver: CdpDriver,
  type: string,
  messageFragment: string,
  timeoutMs: number,
): Promise<RuntimeConsoleAPICalledParams> =>
  await new Promise<RuntimeConsoleAPICalledParams>((resolve, reject) => {
    let settled = false;
    let unsubscribe: (() => void) | undefined = undefined;
    const timer = setTimeout(() => {
      if (settled) {
        return;
      }
      settled = true;
      unsubscribe?.();
      reject(
        new Error(
          `Timed out waiting for ${type} console message containing ${messageFragment}`,
        ),
      );
    }, timeoutMs);

    unsubscribe = driver.on<RuntimeConsoleAPICalledParams>(
      "Runtime.consoleAPICalled",
      (params) => {
        if (params.type !== type) {
          return;
        }
        const matches = params.args?.some(
          (argument) =>
            typeof argument.value === "string" &&
            argument.value.includes(messageFragment),
        );
        if (matches !== true || settled) {
          return;
        }
        settled = true;
        clearTimeout(timer);
        unsubscribe?.();
        resolve(params);
      },
    );
  });

const waitForDocumentLocation = async (
  driver: CdpDriver,
  expectedHref: string,
  timeoutMs: number,
): Promise<{ href: string; origin: string }> => {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    try {
      const location = await driver.evaluate<{ href: string; origin: string }>(
        `({
          href: document.location.href,
          origin: document.location.origin,
        })`,
      );
      if (location.href === expectedHref) {
        return location;
      }
    } catch (error) {
      if (!String(error).includes("Cannot find default execution context")) {
        throw error;
      }
    }
    await wait(100);
  }
  throw new Error(`Timed out waiting for document location: ${expectedHref}`);
};

const createCrc32Table = (): Uint32Array => {
  const table = new Uint32Array(256);
  for (let index = 0; index < table.length; index += 1) {
    let value = index;
    for (let bit = 0; bit < 8; bit += 1) {
      value = (value & 1) !== 0 ? 0xedb88320 ^ (value >>> 1) : value >>> 1;
    }
    table[index] = value >>> 0;
  }
  return table;
};

const crc32Table = createCrc32Table();

const calculateCrc32 = (data: Buffer): number => {
  let crc = 0xffffffff;
  for (const byte of data) {
    crc = (crc32Table[(crc ^ byte) & 0xff] ?? 0) ^ (crc >>> 8);
  }
  return (crc ^ 0xffffffff) >>> 0;
};

const createStoredZipArchive = (entries: StoredZipEntry[]): Buffer => {
  const localParts: Buffer[] = [];
  const centralParts: Buffer[] = [];
  let offset = 0;

  for (const entry of entries) {
    const name = Buffer.from(entry.name, "utf8");
    const data = Buffer.from(entry.content, "utf8");
    const crc = calculateCrc32(data);

    const localHeader = Buffer.alloc(30);
    localHeader.writeUInt32LE(0x04034b50, 0);
    localHeader.writeUInt16LE(20, 4);
    localHeader.writeUInt16LE(0, 6);
    localHeader.writeUInt16LE(0, 8);
    localHeader.writeUInt32LE(crc, 14);
    localHeader.writeUInt32LE(data.length, 18);
    localHeader.writeUInt32LE(data.length, 22);
    localHeader.writeUInt16LE(name.length, 26);
    localParts.push(localHeader, name, data);

    const centralHeader = Buffer.alloc(46);
    centralHeader.writeUInt32LE(0x02014b50, 0);
    centralHeader.writeUInt16LE(20, 4);
    centralHeader.writeUInt16LE(20, 6);
    centralHeader.writeUInt16LE(0, 8);
    centralHeader.writeUInt16LE(0, 10);
    centralHeader.writeUInt32LE(crc, 16);
    centralHeader.writeUInt32LE(data.length, 20);
    centralHeader.writeUInt32LE(data.length, 24);
    centralHeader.writeUInt16LE(name.length, 28);
    centralHeader.writeUInt32LE(offset, 42);
    centralParts.push(centralHeader, name);

    offset += localHeader.length + name.length + data.length;
  }

  const centralOffset = offset;
  const centralSize = centralParts.reduce(
    (size, part) => size + part.length,
    0,
  );
  const endOfCentralDirectory = Buffer.alloc(22);
  endOfCentralDirectory.writeUInt32LE(0x06054b50, 0);
  endOfCentralDirectory.writeUInt16LE(entries.length, 8);
  endOfCentralDirectory.writeUInt16LE(entries.length, 10);
  endOfCentralDirectory.writeUInt32LE(centralSize, 12);
  endOfCentralDirectory.writeUInt32LE(centralOffset, 16);

  return Buffer.concat([...localParts, ...centralParts, endOfCentralDirectory]);
};

const writeStoredZipArchive = async (
  path: string,
  entries: StoredZipEntry[],
  signatureSalt: string,
): Promise<string> => {
  const archive = createStoredZipArchive(entries);
  await writeFile(path, archive);
  return createHash("sha1")
    .update(archive)
    .update(Buffer.from(signatureSalt, "hex"))
    .digest("hex");
};

describeMuonPluginBridge("muon plugin bridge - app and network", () => {
  it("loads the initial app page from the asset scheme", async () => {
    const running = await startDebugMuon([], ["asset://main/**"]);
    let driver: CdpDriver | undefined = undefined;
    try {
      const appTarget = await waitForPageTargetUrl(
        MUON_APP_URL,
        targetTimeoutMs,
      );
      expect(appTarget).toMatchObject({
        type: "page",
        url: MUON_APP_URL,
      });
      driver = await connectToMuonCdp({
        port: MUON_PORT,
        targetId: appTarget.id,
        timeoutMs: cdpCommandTimeoutMs,
      });

      const location = await waitForDocumentLocation(
        driver,
        MUON_APP_URL,
        targetTimeoutMs,
      );
      expect(location).toEqual({
        href: MUON_APP_URL,
        origin: "asset://main",
      });
      await expect(driver.evaluate("window.isSecureContext")).resolves.toBe(
        true,
      );

      const fetchResult = await driver.evaluate(`(async () => {
        const existing = await fetch("asset://main/index.html");
        const missing = await fetch("asset://main/inde.html");
        const rejected = await fetch("asset://main/safe%2fsecret.txt");
        return {
          existingStatus: existing.status,
          existingText: await existing.text(),
          missingStatus: missing.status,
          rejectedStatus: rejected.status,
        };
      })()`);
      expect(fetchResult).toMatchObject({
        existingStatus: 200,
        missingStatus: 404,
        rejectedStatus: 403,
      });
      expect(fetchResult).toMatchObject({
        existingText: expect.stringContaining("muon app"),
      });
    } catch (error) {
      throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
    } finally {
      await stopMuon(running, driver);
    }
  });

  it("loads app assets from asset.sourcePath ZIP storage", async () => {
    const configDirectory = join(DEBUG_MUON_DIRECTORY, ".muon-test-config");
    const archiveDirectory = isWindowsRemoteE2e()
      ? await mkdtemp(join(tmpdir(), "muon-assets-"))
      : configDirectory;
    const archivePath = join(archiveDirectory, "assets.zip");
    const assetSourcePath = isWindowsRemoteE2e() ? archivePath : "./assets.zip";
    const assetSalt = "deadbeef";
    await mkdir(archiveDirectory, { recursive: true });
    const assetSignature = await writeStoredZipArchive(
      archivePath,
      [
        {
          name: "main/index.html",
          content:
            "<!doctype html><title>zip asset app</title><body>zip asset e2e</body>",
        },
      ],
      assetSalt,
    );

    const running = await startDebugMuon(
      [],
      ["asset://main/**"],
      {},
      undefined,
      TEST_PLUGIN_ALLOW_PATTERNS,
      [],
      TEST_BROWSER_PLUGIN_ALLOW_PATTERNS,
      [],
      null,
      true,
      undefined,
      assetSourcePath,
      assetSignature,
      assetSalt,
    );
    let driver: CdpDriver | undefined = undefined;
    try {
      await expect(
        waitForPageTargetUrl(MUON_APP_URL, targetTimeoutMs),
      ).resolves.toMatchObject({
        type: "page",
        url: MUON_APP_URL,
      });
      driver = await connectToMuonCdp({
        port: MUON_PORT,
        timeoutMs: cdpCommandTimeoutMs,
      });
      await waitForDocumentTitle(driver, "zip asset app", cdpCommandTimeoutMs);

      const fetchResult = await driver.evaluate(`(async () => {
        const response = await fetch("asset://main/index.html");
        return {
          status: response.status,
          text: await response.text(),
        };
      })()`);
      expect(fetchResult).toEqual({
        status: 200,
        text: "<!doctype html><title>zip asset app</title><body>zip asset e2e</body>",
      });
    } catch (error) {
      throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
    } finally {
      await stopMuon(running, driver);
      await rm(isWindowsRemoteE2e() ? archiveDirectory : archivePath, {
        force: true,
        recursive: isWindowsRemoteE2e(),
      });
    }
  });

  it("routes console, plugin, and CEF logs through the unified logger", async () => {
    const emitLogs = async (driver: CdpDriver): Promise<void> => {
      await driver.evaluate<void>(`
        (async () => {
          await new Promise((resolve) => {
            const script = document.createElement("script");
            script.textContent = ${JSON.stringify(`
              console.debug("muon e2e console debug");
              console.info("muon e2e console info");
              console.warn("muon e2e console warning");
              console.error("muon e2e console error");
              window.dispatchEvent(new Event("muon-e2e-console-script-complete"));
            `)};
            window.addEventListener(
              "muon-e2e-console-script-complete",
              () => resolve(undefined),
              { once: true },
            );
            document.documentElement.appendChild(script);
            script.remove();
          });
        })()
      `);
      await driver.evaluate<void>(
        `globalThis.muon.test.helpers.helperLogInfo("muon e2e plugin helper")`,
      );
    };

    const assertLogs = async (
      waitForLog: (expected: string) => Promise<void>,
      injectCefLog: boolean,
      localStateHome: string | undefined = undefined,
    ): Promise<void> => {
      if (injectCefLog) {
        await appendFile(
          getLocalBootstrapCefLogPath(localStateHome),
          "[0529/123456.789:ERROR:muon-e2e.cc(1)] muon e2e cef forward\n",
        );
      }

      if (!isWindowsRemoteE2e()) {
        await waitForLog("muon e2e console debug");
      }
      await waitForLog("muon e2e console info");
      await waitForLog("muon e2e console warning");
      await waitForLog("muon e2e console error");
      await waitForLog("][info][plugin] muon e2e plugin helper");
      if (injectCefLog) {
        await waitForLog(
          "][error][cef] [0529/123456.789:ERROR:muon-e2e.cc(1)] muon e2e cef forward",
        );
      }
    };

    if (isWindowsRemoteE2e()) {
      const logPath = join(
        DEBUG_MUON_DIRECTORY,
        ".muon-test-config",
        "muon-e2e.log",
      );
      const readLogContent = async (): Promise<string> => {
        const deadline = Date.now() + targetTimeoutMs;
        let lastError: unknown = undefined;
        while (Date.now() < deadline) {
          try {
            return (await readFile(logPath, "utf8")) as string;
          } catch (error) {
            lastError = error;
          }
          await wait(100);
        }
        throw lastError;
      };
      const configPath = join(
        DEBUG_MUON_DIRECTORY,
        ".muon-test-config",
        "muon.json",
      );
      const running = await startDebugMuon(
        ["muon_test_plugin_helpers"],
        TEST_NETWORK_ALLOW_PATTERNS,
        {},
        undefined,
        TEST_PLUGIN_ALLOW_PATTERNS,
        ["muon_test_plugin_helpers"],
        TEST_BROWSER_PLUGIN_ALLOW_PATTERNS,
        [],
        null,
        true,
        undefined,
        undefined,
        undefined,
        undefined,
        undefined,
        undefined,
        undefined,
        undefined,
        {
          output: { path: "muon-e2e.log", type: "file" },
          sources: {
            console: "debug",
            plugin: "info",
          },
        },
      );
      let driver: CdpDriver | undefined = undefined;
      try {
        driver = await connectToMuonCdp({
          port: MUON_PORT,
          timeoutMs: cdpCommandTimeoutMs,
        });
        await driver.navigate(
          "data:text/html,<title>muon test</title>",
          cdpCommandTimeoutMs,
        );
        await emitLogs(driver);
        await driver.send("Browser.close", undefined);
        driver.close();
        driver = undefined;
        await waitForProcessExit(running, targetTimeoutMs);
        await cleanupWindowsRemoteTestProcesses();
        const logContent = await readLogContent();
        await assertLogs(async (expected) => {
          expect(logContent).toContain(expected);
        }, false);
      } catch (error) {
        const configContent = await readFile(configPath, "utf8").catch(
          (readError) => String(readError),
        );
        const logContent = await readFile(logPath, "utf8").catch((readError) =>
          String(readError),
        );
        throw new Error(
          `${String(error)}\nMuon config:\n${String(
            configContent,
          )}\nMuon log:\n${String(logContent)}\nMuon stderr:\n${
            running.stderr
          }`,
        );
      } finally {
        await stopMuon(running, driver);
      }
      return;
    }

    await withMuon(["muon_test_plugin_helpers"], async (driver, running) => {
      await emitLogs(driver);
      await assertLogs(
        async (expected) =>
          await waitForMuonStderr(running, expected, targetTimeoutMs),
        true,
        running.stateDirectory,
      );
    });
  });

  it("returns forbidden for asset requests when network.allow is empty", async () => {
    const running = await startDebugMuon([], []);
    let driver: CdpDriver | undefined = undefined;
    try {
      driver = await connectToMuonCdp({
        port: MUON_PORT,
        timeoutMs: cdpCommandTimeoutMs,
      });
      await expect(driver.evaluate("document.location.href")).resolves.toBe(
        MUON_APP_URL,
      );
      await expect(
        driver.evaluate("document.body.innerText"),
      ).resolves.toContain("Forbidden");
      await expect(
        driver.evaluate("document.body.innerText"),
      ).resolves.toContain(MUON_APP_URL);
    } catch (error) {
      throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
    } finally {
      await stopMuon(running, driver);
    }
  });

  it("reads and writes files from the default app page", async () => {
    const directory = await mkdtemp(join(tmpdir(), "muon-app-fs-"));
    const demoPath = join(directory, "demo.txt");
    const demoText = "default app filesystem demo";
    const running = await startDebugMuon([], ["asset://main/**"]);
    let driver: CdpDriver | undefined = undefined;
    try {
      driver = await connectToMuonCdp({
        port: MUON_PORT,
        timeoutMs: cdpCommandTimeoutMs,
      });
      const result = await driver.evaluate<{
        content: string;
        status: string;
      }>(`(async () => {
        const waitForControls = async () => {
          const deadline = Date.now() + 3000;
          while (Date.now() < deadline) {
            const pathInput = document.querySelector("#path");
            const contentInput = document.querySelector("#content");
            const writeButton = document.querySelector("#write");
            const readButton = document.querySelector("#read");
            const statusOutput = document.querySelector("#status");
            if (
              pathInput instanceof HTMLInputElement &&
              contentInput instanceof HTMLTextAreaElement &&
              writeButton instanceof HTMLButtonElement &&
              readButton instanceof HTMLButtonElement &&
              statusOutput instanceof HTMLOutputElement
            ) {
              return {
                pathInput,
                contentInput,
                writeButton,
                readButton,
                statusOutput,
              };
            }
            await new Promise((resolve) => setTimeout(resolve, 25));
          }
          throw new Error("Default filesystem demo controls are missing");
        };

        const {
          pathInput,
          contentInput,
          writeButton,
          readButton,
          statusOutput,
        } = await waitForControls();

        const waitForStatus = async (expected) => {
          const deadline = Date.now() + 3000;
          while (Date.now() < deadline) {
            if (statusOutput.textContent === expected) {
              return;
            }
            await new Promise((resolve) => setTimeout(resolve, 25));
          }
          throw new Error("Timed out waiting for status: " + expected);
        };

        pathInput.value = ${JSON.stringify(demoPath)};
        contentInput.value = ${JSON.stringify(demoText)};
        writeButton.click();
        await waitForStatus("Wrote file");

        contentInput.value = "";
        readButton.click();
        await waitForStatus("Read file");

        return {
          content: contentInput.value,
          status: statusOutput.textContent ?? "",
        };
      })()`);

      expect(result).toEqual({
        content: demoText,
        status: "Read file",
      });
      await expect(readFile(demoPath, "utf8")).resolves.toBe(demoText);
    } catch (error) {
      throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
    } finally {
      await stopMuon(running, driver);
      await rm(directory, { recursive: true, force: true });
    }
  });

  it("runs popup and browser controls from the default app page", async () => {
    const running = await startDebugMuon(
      [],
      ["asset://main/**"],
      {},
      undefined,
      TEST_PLUGIN_ALLOW_PATTERNS,
      [],
      TEST_BROWSER_PLUGIN_ALLOW_PATTERNS,
      [],
      ["asset://main/**"],
    );
    let driver: CdpDriver | undefined = undefined;
    const popupDrivers: CdpDriver[] = [];
    try {
      driver = await connectToMuonCdp({
        port: MUON_PORT,
        timeoutMs: cdpCommandTimeoutMs,
      });

      const browserDemo = await driver.evaluate<{
        status: string;
        state: string;
        widthChanged: boolean;
      }>(`(async () => {
        const waitForControls = async () => {
          const deadline = Date.now() + 3000;
          while (Date.now() < deadline) {
            const button = document.querySelector("[data-browser-action='zoomIn']");
            const status = document.querySelector("#browser-status");
            if (
              button instanceof HTMLButtonElement &&
              status instanceof HTMLOutputElement
            ) {
              return { button, status };
            }
            await new Promise((resolve) => setTimeout(resolve, 25));
          }
          throw new Error("Default browser demo controls are missing");
        };

        const { button, status } = await waitForControls();
        const initialWidth = window.innerWidth;
        button.click();
        const deadline = Date.now() + 3000;
        while (Date.now() < deadline) {
          if (
            status.textContent === "zoomIn completed" &&
            window.innerWidth < initialWidth
          ) {
            return {
              status: status.textContent,
              state: status.dataset.state ?? "",
              widthChanged: true,
            };
          }
          await new Promise((resolve) => setTimeout(resolve, 25));
        }
        return {
          status: status.textContent ?? "",
          state: status.dataset.state ?? "",
          widthChanged: window.innerWidth < initialWidth,
        };
      })()`);
      expect(browserDemo).toEqual({
        status: "zoomIn completed",
        state: "ok",
        widthChanged: true,
      });

      const previousTargetIds = await getCurrentTargetIds();
      await driver.send<RuntimeEvaluateResponse>("Runtime.evaluate", {
        expression: `document.querySelector("#open-connected-popup").click(); "clicked"`,
        returnByValue: true,
        userGesture: true,
      });
      const popupTarget = await waitForNewPageTarget(
        previousTargetIds,
        targetTimeoutMs,
        (target) => target.url === `${MUON_APP_URL}#popup=connected`,
      );
      const popupDriver = await connectToMuonCdp({
        port: MUON_PORT,
        targetId: popupTarget.id,
        timeoutMs: cdpCommandTimeoutMs,
      });
      popupDrivers.push(popupDriver);

      await expect(
        popupDriver.evaluate("document.location.href"),
      ).resolves.toBe(`${MUON_APP_URL}#popup=connected`);
      const popupDemo = await popupDriver.evaluate<{
        status: string;
        state: string;
      }>(`(async () => {
        const status = document.querySelector("#popup-status");
        if (!(status instanceof HTMLOutputElement)) {
          throw new Error("Default popup demo status is missing");
        }
        const deadline = Date.now() + 3000;
        while (Date.now() < deadline) {
          if (status.textContent === "Popup has opener access") {
            return {
              status: status.textContent,
              state: status.dataset.state ?? "",
            };
          }
          await new Promise((resolve) => setTimeout(resolve, 25));
        }
        return {
          status: status.textContent ?? "",
          state: status.dataset.state ?? "",
        };
      })()`);
      expect(popupDemo).toEqual({
        status: "Popup has opener access",
        state: "ok",
      });
    } catch (error) {
      throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
    } finally {
      for (const popupDriver of popupDrivers) {
        popupDriver.close();
      }
      await stopMuon(running, driver);
    }
  });

  it("opens default page popups without opener access by default", async () => {
    const running = await startDebugMuon(
      [],
      ["asset://main/**"],
      {},
      undefined,
      TEST_PLUGIN_ALLOW_PATTERNS,
      [],
      TEST_BROWSER_PLUGIN_ALLOW_PATTERNS,
    );
    let driver: CdpDriver | undefined = undefined;
    let popupDriver: CdpDriver | undefined = undefined;
    try {
      driver = await connectToMuonCdp({
        port: MUON_PORT,
        timeoutMs: cdpCommandTimeoutMs,
      });

      const previousTargetIds = await getCurrentTargetIds();
      const response = await driver.send<RuntimeEvaluateResponse>(
        "Runtime.evaluate",
        {
          expression: `(async () => {
            const deadline = Date.now() + 3000;
            while (Date.now() < deadline) {
              const button = document.querySelector("#open-connected-popup");
              if (button instanceof HTMLButtonElement) {
                button.click();
                return "clicked";
              }
              await new Promise((resolve) => setTimeout(resolve, 25));
            }
            throw new Error("Default popup demo button is missing");
          })()`,
          returnByValue: true,
          userGesture: true,
          awaitPromise: true,
        },
      );
      if (response.exceptionDetails !== undefined) {
        throw new Error("default popup click script failed");
      }
      const popupTarget = await waitForNewPageTarget(
        previousTargetIds,
        targetTimeoutMs,
        (target) => target.url === `${MUON_APP_URL}#popup=connected`,
      );
      popupDriver = await connectToMuonCdp({
        port: MUON_PORT,
        targetId: popupTarget.id,
        timeoutMs: cdpCommandTimeoutMs,
      });

      await expect(
        popupDriver.evaluate("document.location.href"),
      ).resolves.toBe(`${MUON_APP_URL}#popup=connected`);
      await expect(
        popupDriver.evaluate("window.opener === null"),
      ).resolves.toBe(true);
      await expect(
        driver.evaluate("document.querySelector('#popup-status')?.textContent"),
      ).resolves.toBe("Popup requested without opener access");
      expect(running.process.exitCode).toBeNull();
      expect(running.process.signalCode).toBeNull();
    } catch (error) {
      throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
    } finally {
      if (popupDriver !== undefined) {
        popupDriver.close();
      }
      await stopMuon(running, driver);
    }
  });

  it("returns forbidden for non-allowlisted browser requests", async () => {
    const blockedUrl = "https://blocked.example/";
    const running = await startDebugMuon([], []);
    let driver: CdpDriver | undefined = undefined;
    try {
      driver = await connectToMuonCdp({
        port: MUON_PORT,
        timeoutMs: cdpCommandTimeoutMs,
      });
      await driver.send("Network.enable", undefined);
      const response = waitForNetworkResponse(
        driver,
        blockedUrl,
        cdpCommandTimeoutMs,
      );
      await driver.navigate(blockedUrl, cdpCommandTimeoutMs);
      await expect(response).resolves.toMatchObject({
        response: {
          url: blockedUrl,
          status: 403,
        },
      });
      await expect(
        driver.evaluate("document.body.innerText"),
      ).resolves.toContain("Forbidden");
      await expect(
        driver.evaluate("document.body.innerText"),
      ).resolves.toContain(blockedUrl);
    } catch (error) {
      throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
    } finally {
      await stopMuon(running, driver);
    }
  });

  it("warns the initiating page console for non-allowlisted fetch requests", async () => {
    let allowedHits = 0;
    let blockedHits = 0;
    const server = await startHttpServer((request, response) => {
      if (request.url === "/allowed.html") {
        allowedHits += 1;
        response.setHeader("Content-Type", "text/html");
        response.end("<!doctype html><title>allowed fetch initiator</title>");
        return;
      }
      if (request.url?.startsWith("/blocked-fetch.json") === true) {
        blockedHits += 1;
        response.statusCode = 500;
        response.end("blocked fetch target should not reach the server");
        return;
      }
      response.statusCode = 404;
      response.end("missing");
    });
    const allowedUrl = `${server.origin}/allowed.html`;
    const blockedUrl = `${server.origin}/blocked-fetch.json?quote=%22&tag=%3Cscript%3E`;
    const running = await startDebugMuon([], ["asset://main/**", allowedUrl]);
    let driver: CdpDriver | undefined = undefined;
    try {
      driver = await connectToMuonCdp({
        port: MUON_PORT,
        timeoutMs: cdpCommandTimeoutMs,
      });
      await driver.navigate(allowedUrl, cdpCommandTimeoutMs);
      expect(allowedHits).toBe(1);
      await driver.send("Runtime.enable");
      const consoleEvent = waitForConsoleMessage(
        driver,
        "warning",
        blockedUrl,
        cdpCommandTimeoutMs,
      );
      const fetchResult = await driver.evaluate(`(async () => {
        const response = await fetch(${JSON.stringify(blockedUrl)});
        return {
          status: response.status,
          text: await response.text(),
        };
      })()`);
      expect(fetchResult).toEqual({
        status: 403,
        text: "Forbidden",
      });
      const consoleMessage = await consoleEvent;
      expect(consoleMessage.type).toBe("warning");
      expect(consoleMessage.args?.[0]?.value).toContain("Forbidden");
      expect(consoleMessage.args?.[0]?.value).toContain(blockedUrl);
      expect(blockedHits).toBe(0);
    } catch (error) {
      throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
    } finally {
      await stopMuon(running, driver);
      await stopHttpServer(server.server);
    }
  });

  it("allows non-asset URLs matching network.allow", async () => {
    const running = await startDebugMuon([], TEST_NETWORK_ALLOW_PATTERNS);
    let driver: CdpDriver | undefined = undefined;
    try {
      driver = await connectToMuonCdp({
        port: MUON_PORT,
        timeoutMs: cdpCommandTimeoutMs,
      });
      await driver.navigate(
        "data:text/html,<title>allowed network</title>",
        cdpCommandTimeoutMs,
      );
      await expect(driver.evaluate("document.title")).resolves.toBe(
        "allowed network",
      );
    } catch (error) {
      throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
    } finally {
      await stopMuon(running, driver);
    }
  });

  it("opens popup windows as separate page targets", async () => {
    const running = await startDebugMuon(
      [],
      TEST_NETWORK_ALLOW_PATTERNS,
      {},
      undefined,
      TEST_PLUGIN_ALLOW_PATTERNS,
      [],
      TEST_BROWSER_PLUGIN_ALLOW_PATTERNS,
      [],
      ["asset://main/**"],
    );
    let driver: CdpDriver | undefined = undefined;
    const popupDrivers: CdpDriver[] = [];
    try {
      driver = await connectToMuonCdp({
        port: MUON_PORT,
        timeoutMs: cdpCommandTimeoutMs,
      });
      const popupTarget = await openPopupTarget(driver, MUON_APP_URL);
      const popupDriver = await connectToMuonCdp({
        port: MUON_PORT,
        targetId: popupTarget.id,
        timeoutMs: cdpCommandTimeoutMs,
      });
      popupDrivers.push(popupDriver);

      await expect(
        popupDriver.evaluate("document.location.href"),
      ).resolves.toBe(MUON_APP_URL);
    } catch (error) {
      throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
    } finally {
      for (const popupDriver of popupDrivers) {
        popupDriver.close();
      }
      await stopMuon(running, driver);
    }
  });

  it("disconnects popup opener access for URLs outside browser.allowUnsafeJavaScriptParentAccess", async () => {
    let popupHits = 0;
    let modifiedHits = 0;
    const server = await startHttpServer((request, response) => {
      response.setHeader("Content-Type", "text/html");
      if (request.url === "/modified") {
        modifiedHits += 1;
        response.end("modified");
        return;
      }
      if (request.url === "/opener.html") {
        response.end(`<!doctype html>
<title>opener safe</title>
<a id="popup" href="/popup.html" target="_blank"
   style="display:block;width:120px;height:80px">open</a>`);
        return;
      }
      if (request.url === "/popup.html") {
        popupHits += 1;
        response.end(`<!doctype html>
<title>popup loaded</title>
<script>
try {
  window.opener.document.title = "opener modified";
  fetch("/modified");
} catch {
}
</script>`);
        return;
      }
      response.statusCode = 404;
      response.end("missing");
    });
    const openerUrl = `${server.origin}/opener.html`;
    const running = await startDebugMuon(
      [],
      ["asset://main/**", `${server.origin}/**`],
      {},
      undefined,
      TEST_PLUGIN_ALLOW_PATTERNS,
      [],
      TEST_BROWSER_PLUGIN_ALLOW_PATTERNS,
      [],
      [],
    );
    let driver: CdpDriver | undefined = undefined;
    try {
      driver = await connectToMuonCdp({
        port: MUON_PORT,
        timeoutMs: cdpCommandTimeoutMs,
      });
      await driver.navigate(openerUrl, cdpCommandTimeoutMs);
      await expect(driver.evaluate("document.title")).resolves.toBe(
        "opener safe",
      );
      const previousTargetIds = await getCurrentTargetIds();
      const response = await driver.send<RuntimeEvaluateResponse>(
        "Runtime.evaluate",
        {
          expression: `document.querySelector("#popup").click(); "clicked"`,
          returnByValue: true,
          userGesture: true,
        },
      );
      if (response.exceptionDetails !== undefined) {
        throw new Error("popup click script failed");
      }
      const popupTarget = await waitForNewPageTarget(
        previousTargetIds,
        targetTimeoutMs,
        (target) => target.url === `${server.origin}/popup.html`,
      );
      expect(popupTarget.type).toBe("page");
      const deadline = Date.now() + targetTimeoutMs;
      while (popupHits === 0 && Date.now() < deadline) {
        await wait(100);
      }
      expect(popupHits).toBe(1);
      expect(modifiedHits).toBe(0);
    } catch (error) {
      throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
    } finally {
      await stopMuon(running, driver);
      await stopHttpServer(server.server);
    }
  });

  it("disconnects popup opener access by default", async () => {
    let popupHits = 0;
    let modifiedHits = 0;
    const server = await startHttpServer((request, response) => {
      response.setHeader("Content-Type", "text/html");
      if (request.url === "/modified") {
        modifiedHits += 1;
        response.end("modified");
        return;
      }
      if (request.url === "/opener.html") {
        response.end(`<!doctype html>
<title>opener safe</title>
<a id="popup" href="/popup.html" target="_blank"
   style="display:block;width:120px;height:80px">open</a>`);
        return;
      }
      if (request.url === "/popup.html") {
        popupHits += 1;
        response.end(`<!doctype html>
<title>popup loaded</title>
<script>
try {
  window.opener.document.title = "opener modified";
  fetch("/modified");
} catch {
}
</script>`);
        return;
      }
      response.statusCode = 404;
      response.end("missing");
    });
    const openerUrl = `${server.origin}/opener.html`;
    const running = await startDebugMuon(
      [],
      ["asset://main/**", `${server.origin}/**`],
      {},
      undefined,
      TEST_PLUGIN_ALLOW_PATTERNS,
      [],
      TEST_BROWSER_PLUGIN_ALLOW_PATTERNS,
    );
    let driver: CdpDriver | undefined = undefined;
    try {
      driver = await connectToMuonCdp({
        port: MUON_PORT,
        timeoutMs: cdpCommandTimeoutMs,
      });
      await driver.navigate(openerUrl, cdpCommandTimeoutMs);
      await expect(driver.evaluate("document.title")).resolves.toBe(
        "opener safe",
      );
      const previousTargetIds = await getCurrentTargetIds();
      const response = await driver.send<RuntimeEvaluateResponse>(
        "Runtime.evaluate",
        {
          expression: `document.querySelector("#popup").click(); "clicked"`,
          returnByValue: true,
          userGesture: true,
        },
      );
      if (response.exceptionDetails !== undefined) {
        throw new Error("popup click script failed");
      }
      await waitForNewPageTarget(
        previousTargetIds,
        targetTimeoutMs,
        (target) => target.url === `${server.origin}/popup.html`,
      );
      const deadline = Date.now() + targetTimeoutMs;
      while (popupHits === 0 && Date.now() < deadline) {
        await wait(100);
      }
      expect(popupHits).toBe(1);
      expect(modifiedHits).toBe(0);
      await expect(driver.evaluate("document.title")).resolves.toBe(
        "opener safe",
      );
    } catch (error) {
      throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
    } finally {
      await stopMuon(running, driver);
      await stopHttpServer(server.server);
    }
  });

  it("keeps popup opener access for URLs matching browser.allowUnsafeJavaScriptParentAccess", async () => {
    const running = await startDebugMuon(
      [],
      TEST_NETWORK_ALLOW_PATTERNS,
      {},
      undefined,
      TEST_PLUGIN_ALLOW_PATTERNS,
      [],
      TEST_BROWSER_PLUGIN_ALLOW_PATTERNS,
      [],
      ["asset://main/**"],
    );
    let driver: CdpDriver | undefined = undefined;
    const popupDrivers: CdpDriver[] = [];
    try {
      driver = await connectToMuonCdp({
        port: MUON_PORT,
        timeoutMs: cdpCommandTimeoutMs,
      });
      await driver.evaluate(`document.title = "muon opener page";`);
      const popupTarget = await openPopupTarget(
        driver,
        MUON_APP_URL,
        "",
        "muonAllowedPopup",
      );
      const popupDriver = await connectToMuonCdp({
        port: MUON_PORT,
        targetId: popupTarget.id,
        timeoutMs: cdpCommandTimeoutMs,
      });
      popupDrivers.push(popupDriver);

      await expect(
        popupDriver.evaluate(`(() => {
          if (window.opener === null) {
            return { hasOpener: false };
          }
          window.opener.__muonPopupAccess = 42;
          return {
            hasOpener: true,
            openerValue: window.opener.__muonPopupAccess,
          };
        })()`),
      ).resolves.toEqual({
        hasOpener: true,
        openerValue: 42,
      });
      await expect(driver.evaluate("window.__muonPopupAccess")).resolves.toBe(
        42,
      );
    } catch (error) {
      throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
    } finally {
      for (const popupDriver of popupDrivers) {
        popupDriver.close();
      }
      await stopMuon(running, driver);
    }
  });

  it("keeps no-opener popup access disconnected even for unsafe parent access matches", async () => {
    const running = await startDebugMuon(
      [],
      TEST_NETWORK_ALLOW_PATTERNS,
      {},
      undefined,
      TEST_PLUGIN_ALLOW_PATTERNS,
      [],
      TEST_BROWSER_PLUGIN_ALLOW_PATTERNS,
      [],
      ["asset://main/**"],
    );
    let driver: CdpDriver | undefined = undefined;
    const popupDrivers: CdpDriver[] = [];
    try {
      driver = await connectToMuonCdp({
        port: MUON_PORT,
        timeoutMs: cdpCommandTimeoutMs,
      });

      for (const baseFeatures of ["noopener", "noreferrer"]) {
        const features = addPopupSizeFeatures(baseFeatures);
        const previousTargetIds = await getCurrentTargetIds();
        const openResponse = await driver.send<RuntimeEvaluateResponse>(
          "Runtime.evaluate",
          {
            expression: `(() => {
              const popup = window.open(
                ${JSON.stringify(MUON_APP_URL)},
                ${JSON.stringify(`muonNoOpenerPopup-${baseFeatures}`)},
                ${JSON.stringify(features)}
              );
              return {
                returnedNull: popup === null,
                returnedHasOpener:
                  popup !== null && popup.opener !== null,
              };
            })()`,
            returnByValue: true,
            userGesture: true,
          },
        );
        if (openResponse.exceptionDetails !== undefined) {
          throw new Error(`${baseFeatures} popup open script failed`);
        }
        expect(openResponse.result?.value).toEqual({
          returnedNull: true,
          returnedHasOpener: false,
        });
        const popupTarget = await waitForNewPageTarget(
          previousTargetIds,
          targetTimeoutMs,
          (target) => target.url === MUON_APP_URL,
        );
        const popupDriver = await connectToMuonCdp({
          port: MUON_PORT,
          targetId: popupTarget.id,
          timeoutMs: cdpCommandTimeoutMs,
        });
        popupDrivers.push(popupDriver);

        await expect(
          popupDriver.evaluate("window.opener === null"),
        ).resolves.toBe(true);
      }
    } catch (error) {
      throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
    } finally {
      for (const popupDriver of popupDrivers) {
        popupDriver.close();
      }
      await stopMuon(running, driver);
    }
  });

  it("opens blocked popup targets with a visible console-observable error page", async () => {
    const server = await startHttpServer((request, response) => {
      response.statusCode = 500;
      response.end("blocked popup target should not reach the server");
    });
    const popupUrl = `${server.origin}/blocked-popup.html`;
    const running = await startDebugMuon([], ["asset://main/**"]);
    let driver: CdpDriver | undefined = undefined;
    let popupDriver: CdpDriver | undefined = undefined;
    try {
      driver = await connectToMuonCdp({
        port: MUON_PORT,
        timeoutMs: cdpCommandTimeoutMs,
      });
      const previousTargetIds = await getCurrentTargetIds();
      const popupFeatures = addPopupSizeFeatures("");
      await driver.send<RuntimeEvaluateResponse>("Runtime.evaluate", {
        expression: `window.open(${JSON.stringify(
          popupUrl,
        )}, "_blank", ${JSON.stringify(popupFeatures)}); "opened"`,
        returnByValue: true,
        userGesture: true,
      });
      const popupTarget = await waitForNewPageTarget(
        previousTargetIds,
        targetTimeoutMs,
        (target) => target.url === popupUrl,
      );
      popupDriver = await connectToMuonCdp({
        port: MUON_PORT,
        timeoutMs: cdpCommandTimeoutMs,
        targetId: popupTarget.id,
      });
      await popupDriver.send("Runtime.enable");
      await popupDriver.send("Network.enable");
      const consoleEvent = waitForConsoleMessage(
        popupDriver,
        "error",
        popupUrl,
        cdpCommandTimeoutMs,
      );
      const responseEvent = waitForNetworkResponse(
        popupDriver,
        popupUrl,
        cdpCommandTimeoutMs,
      );
      await popupDriver.send("Page.reload", { ignoreCache: true });
      const response = await responseEvent;
      expect(response.response?.url).toBe(popupUrl);
      expect(response.response?.status).toBe(403);
      const consoleMessage = await consoleEvent;
      expect(consoleMessage.type).toBe("error");
      expect(consoleMessage.args?.[0]?.value).toContain("Forbidden");
      expect(consoleMessage.args?.[0]?.value).toContain(popupUrl);
      await expect(
        popupDriver.evaluate("document.body.textContent"),
      ).resolves.toContain("Forbidden");
      await expect(
        popupDriver.evaluate("document.body.textContent"),
      ).resolves.toContain(popupUrl);
      await expect(
        popupDriver.evaluate(`(() => {
          const style = getComputedStyle(document.body);
          return {
            backgroundColor: style.backgroundColor,
            color: style.color,
          };
        })()`),
      ).resolves.toEqual({
        backgroundColor: "rgb(255, 255, 255)",
        color: "rgb(17, 17, 17)",
      });
      await expect(
        popupDriver.evaluate("window.opener === null"),
      ).resolves.toBe(true);
    } catch (error) {
      throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
    } finally {
      popupDriver?.close();
      await stopMuon(running, driver);
      await stopHttpServer(server.server);
    }
  });

  it("exposes plugin APIs in popup pages matching plugin.pages", async () => {
    const running = await startDebugMuon(
      [],
      TEST_NETWORK_ALLOW_PATTERNS,
      {},
      undefined,
      TEST_PLUGIN_ALLOW_PATTERNS,
      [],
      ["asset://main/**"],
      [],
      ["asset://main/**"],
    );
    let driver: CdpDriver | undefined = undefined;
    const popupDrivers: CdpDriver[] = [];
    try {
      driver = await connectToMuonCdp({
        port: MUON_PORT,
        timeoutMs: cdpCommandTimeoutMs,
      });
      const popupTarget = await openPopupTarget(
        driver,
        MUON_APP_URL,
        "",
        "muonPluginPopup",
      );
      const popupDriver = await connectToMuonCdp({
        port: MUON_PORT,
        targetId: popupTarget.id,
        timeoutMs: cdpCommandTimeoutMs,
      });
      popupDrivers.push(popupDriver);
      await waitForDocumentTitle(popupDriver, "muon", cdpCommandTimeoutMs);

      await expect(
        popupDriver.evaluate("typeof window.muon?.fs?.readFile"),
      ).resolves.toBe("function");
    } catch (error) {
      throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
    } finally {
      for (const popupDriver of popupDrivers) {
        popupDriver.close();
      }
      await stopMuon(running, driver);
    }
  });

  it("does not expose plugin APIs in popup pages outside plugin.pages", async () => {
    const server = await startHttpServer((request, response) => {
      response.setHeader("Content-Type", "text/html");
      if (request.url === "/popup.html") {
        response.end("<title>muon plugin popup deny</title>");
        return;
      }
      response.statusCode = 404;
      response.end("missing");
    });
    const popupUrl = `${server.origin}/popup.html`;
    const running = await startDebugMuon(
      [],
      ["asset://main/**", `${server.origin}/**`],
      {},
      undefined,
      TEST_PLUGIN_ALLOW_PATTERNS,
      [],
      ["asset://main/**"],
      [],
      [`${server.origin}/**`],
    );
    let driver: CdpDriver | undefined = undefined;
    const popupDrivers: CdpDriver[] = [];
    try {
      driver = await connectToMuonCdp({
        port: MUON_PORT,
        timeoutMs: cdpCommandTimeoutMs,
      });
      const popupTarget = await openPopupTarget(
        driver,
        popupUrl,
        "",
        "muonPluginDenyPopup",
      );
      const popupDriver = await connectToMuonCdp({
        port: MUON_PORT,
        targetId: popupTarget.id,
        timeoutMs: cdpCommandTimeoutMs,
      });
      popupDrivers.push(popupDriver);
      await waitForDocumentTitle(
        popupDriver,
        "muon plugin popup deny",
        cdpCommandTimeoutMs,
      );

      await expect(popupDriver.evaluate("typeof window.muon")).resolves.toBe(
        "undefined",
      );
    } catch (error) {
      throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
    } finally {
      for (const popupDriver of popupDrivers) {
        popupDriver.close();
      }
      await stopMuon(running, driver);
      await stopHttpServer(server.server);
    }
  });

  it("allows requests initiated by network.authorizedOrigin", async () => {
    const pixel = Buffer.from(
      "R0lGODlhAQABAIAAAAAAAP///ywAAAAAAQABAAACAUwAOw==",
      "base64",
    );
    const resourceHits: string[] = [];
    const resourceServer = await startHttpServer((request, response) => {
      resourceHits.push(request.url ?? "");
      if (request.url === "/allowed.js") {
        response.setHeader("Content-Type", "application/javascript");
        response.end("window.authorizedScriptLoaded = true;");
        return;
      }
      if (request.url === "/data.json") {
        response.setHeader("Access-Control-Allow-Origin", "*");
        response.setHeader("Content-Type", "application/json");
        response.end(JSON.stringify({ ok: true }));
        return;
      }
      if (request.url === "/pixel.gif") {
        response.setHeader("Content-Type", "image/gif");
        response.end(pixel);
        return;
      }
      response.statusCode = 404;
      response.end("missing");
    });
    const pageServer = await startHttpServer((request, response) => {
      if (request.url !== "/authorized.html") {
        response.statusCode = 404;
        response.end("missing");
        return;
      }
      response.setHeader("Content-Type", "text/html");
      response.end(`<!doctype html>
<title>authorized pending</title>
<script src="${resourceServer.origin}/allowed.js"></script>
<img id="pixel" src="${resourceServer.origin}/pixel.gif">
<script>
const waitForImage = (image) => new Promise((resolve, reject) => {
  if (image.complete) {
    resolve();
    return;
  }
  image.addEventListener("load", resolve, { once: true });
  image.addEventListener("error", () => reject(new Error("image failed")), {
    once: true,
  });
});
(async () => {
  try {
    const response = await fetch("${resourceServer.origin}/data.json");
    const data = await response.json();
    const image = document.getElementById("pixel");
    await waitForImage(image);
    document.title =
      data.ok && window.authorizedScriptLoaded === true &&
      image.naturalWidth === 1
        ? "authorized loaded"
        : "authorized failed";
  } catch (error) {
    document.body.dataset.error = String(error);
    document.title = "authorized failed";
  }
})();
</script>`);
    });
    const running = await startDebugMuon(
      [],
      TEST_NETWORK_ALLOW_PATTERNS,
      {},
      undefined,
      TEST_PLUGIN_ALLOW_PATTERNS,
      [],
      TEST_BROWSER_PLUGIN_ALLOW_PATTERNS,
      [
        {
          scheme: "http",
          domain: new URL(pageServer.origin).hostname,
          port: pageServer.port,
        },
      ],
    );
    let driver: CdpDriver | undefined = undefined;
    try {
      driver = await connectToMuonCdp({
        port: MUON_PORT,
        timeoutMs: cdpCommandTimeoutMs,
      });
      await driver.navigate(
        `${pageServer.origin}/authorized.html`,
        cdpCommandTimeoutMs,
      );
      await waitForDocumentTitle(
        driver,
        "authorized loaded",
        cdpCommandTimeoutMs,
      );
      expect(resourceHits.sort()).toEqual([
        "/allowed.js",
        "/data.json",
        "/pixel.gif",
      ]);
    } catch (error) {
      throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
    } finally {
      await stopMuon(running, driver);
      await stopHttpServer(pageServer.server);
      await stopHttpServer(resourceServer.server);
    }
  });

  it("blocks top-level redirects from authorized origins to unauthorized origins", async () => {
    let blockedHits = 0;
    const blockedServer = await startHttpServer((request, response) => {
      if (request.url === "/blocked.html") {
        blockedHits += 1;
      }
      response.setHeader("Content-Type", "text/html");
      response.end("<title>blocked target</title>blocked target");
    });
    const redirectServer = await startHttpServer((request, response) => {
      if (request.url !== "/redirect") {
        response.statusCode = 404;
        response.end("missing");
        return;
      }
      response.statusCode = 302;
      response.setHeader("Location", `${blockedServer.origin}/blocked.html`);
      response.end();
    });
    const running = await startDebugMuon(
      [],
      TEST_NETWORK_ALLOW_PATTERNS,
      {},
      undefined,
      TEST_PLUGIN_ALLOW_PATTERNS,
      [],
      TEST_BROWSER_PLUGIN_ALLOW_PATTERNS,
      [
        {
          scheme: "http",
          domain: new URL(redirectServer.origin).hostname,
          port: redirectServer.port,
        },
      ],
    );
    let driver: CdpDriver | undefined = undefined;
    try {
      driver = await connectToMuonCdp({
        port: MUON_PORT,
        timeoutMs: cdpCommandTimeoutMs,
      });
      await driver.send("Network.enable", undefined);
      const blockedUrl = `${blockedServer.origin}/blocked.html`;
      const response = waitForNetworkResponse(
        driver,
        blockedUrl,
        cdpCommandTimeoutMs,
      );
      await driver.navigate(
        `${redirectServer.origin}/redirect`,
        cdpCommandTimeoutMs,
      );
      await expect(response).resolves.toMatchObject({
        response: {
          url: blockedUrl,
          status: 403,
        },
      });
      await expect(
        driver.evaluate("document.body.innerText"),
      ).resolves.toContain("Forbidden");
      await expect(
        driver.evaluate("document.body.innerText"),
      ).resolves.toContain(blockedUrl);
      expect(blockedHits).toBe(0);
    } catch (error) {
      throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
    } finally {
      await stopMuon(running, driver);
      await stopHttpServer(redirectServer.server);
      await stopHttpServer(blockedServer.server);
    }
  });

  it("does not expose plugin APIs outside the default plugin.pages", async () => {
    const running = await startDebugMuon(
      [],
      TEST_NETWORK_ALLOW_PATTERNS,
      {},
      undefined,
      TEST_PLUGIN_ALLOW_PATTERNS,
      [],
      ["asset://main/**"],
    );
    let driver: CdpDriver | undefined = undefined;
    try {
      driver = await connectToMuonCdp({
        port: MUON_PORT,
        timeoutMs: cdpCommandTimeoutMs,
      });
      await expect(
        driver.evaluate("typeof window.muon.fs.readFile"),
      ).resolves.toBe("function");
      await driver.navigate(
        "data:text/html,<title>muon plugin page deny</title>",
        cdpCommandTimeoutMs,
      );
      await expect(driver.evaluate("document.title")).resolves.toBe(
        "muon plugin page deny",
      );
      await expect(driver.evaluate("typeof window.muon")).resolves.toBe(
        "undefined",
      );
    } catch (error) {
      throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
    } finally {
      await stopMuon(running, driver);
    }
  });

  it("exposes plugin APIs on pages matching plugin.pages", async () => {
    const running = await startDebugMuon(
      [],
      TEST_NETWORK_ALLOW_PATTERNS,
      {},
      undefined,
      TEST_PLUGIN_ALLOW_PATTERNS,
      [],
      ["asset://main/**", "data:**"],
    );
    let driver: CdpDriver | undefined = undefined;
    try {
      driver = await connectToMuonCdp({
        port: MUON_PORT,
        timeoutMs: cdpCommandTimeoutMs,
      });
      await driver.navigate(
        "data:text/html,<title>muon plugin page allow</title>",
        cdpCommandTimeoutMs,
      );
      await expect(
        driver.evaluate("typeof window.muon.fs.readFile"),
      ).resolves.toBe("function");
    } catch (error) {
      throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
    } finally {
      await stopMuon(running, driver);
    }
  });
});

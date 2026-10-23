// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFile, spawn, type ChildProcess } from "node:child_process";
import { once } from "node:events";
import {
  cp,
  mkdir,
  mkdtemp,
  readFile,
  readlink,
  readdir,
  rename,
  rm,
  writeFile,
} from "node:fs/promises";
import { createServer } from "node:net";
import { tmpdir } from "node:os";
import { basename, join, resolve } from "node:path";
import { setTimeout as delay } from "node:timers/promises";
import { promisify } from "node:util";

import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { InMemoryTransport } from "@modelcontextprotocol/sdk/inMemory.js";
import { createConnection } from "@playwright/mcp";
import { PNG } from "pngjs";
import { expect, it } from "vitest";

const execute = promisify(execFile);
const archive = process.env.MUON_PACKAGED_LINUX_TARBALL;
const oldPrepared = process.env.MUON_PACKAGED_LINUX_OLD_PREPARED;
type Backend = "wayland" | "x11";
type Lifecycle = "close" | "recycle" | undefined;
type RuntimeProcess = {
  readonly pid: number;
  readonly command: string;
  readonly executable: string;
  readonly maps: string;
};
const backend: Backend =
  process.env.MUON_PACKAGED_LINUX_BACKEND === "x11" ? "x11" : "wayland";

const reservePort = async (): Promise<number> => {
  const listener = createServer();
  listener.listen(0, "127.0.0.1");
  await once(listener, "listening");
  const address = listener.address();
  if (address === null || typeof address === "string")
    throw new Error("Cannot allocate a CDP port");
  listener.close();
  await once(listener, "close");
  return address.port;
};

const readDiagnostics = (child: ChildProcess): (() => string) => {
  const chunks: Buffer[] = [];
  const append = (chunk: Buffer): void => {
    chunks.push(chunk);
    while (Buffer.concat(chunks).length > 64 * 1024) chunks.shift();
  };
  child.stdout?.on("data", append);
  child.stderr?.on("data", append);
  return (): string => Buffer.concat(chunks).toString();
};

const waitForCdp = async (
  child: ChildProcess,
  port: number,
  diagnostics: () => string,
): Promise<void> => {
  // First consumer startup extracts CEF into .muon and can take several minutes.
  const deadline = Date.now() + 600_000;
  while (Date.now() < deadline) {
    if (child.exitCode !== null || child.signalCode !== null)
      throw new Error(`Muon exited before readiness:\n${diagnostics()}`);
    try {
      const response = await fetch(`http://127.0.0.1:${port}/json/version`);
      if (response.ok) return;
    } catch {
      // CDP is not yet listening.
    }
    await delay(100);
  }
  throw new Error(`Timed out waiting for CDP:\n${diagnostics()}`);
};

const waitForProcessGroupExit = async (pid: number): Promise<void> => {
  const deadline = Date.now() + 20_000;
  while (Date.now() < deadline) {
    try {
      process.kill(-pid, 0);
    } catch (error) {
      if ((error as NodeJS.ErrnoException).code === "ESRCH") return;
      throw error;
    }
    await delay(100);
  }
  throw new Error(`Timed out waiting for process group ${pid} to exit`);
};

const signalProcessGroup = (
  child: ChildProcess,
  signal: NodeJS.Signals,
): boolean => {
  if (child.pid === undefined) return child.kill(signal);
  try {
    process.kill(-child.pid, signal);
    return true;
  } catch (error) {
    if ((error as NodeJS.ErrnoException).code === "ESRCH") return false;
    throw error;
  }
};

const stop = async (child: ChildProcess): Promise<void> => {
  const closed =
    child.exitCode === null && child.signalCode === null
      ? once(child, "close")
      : undefined;
  if (!signalProcessGroup(child, "SIGTERM")) return;
  let timeout: NodeJS.Timeout | undefined = undefined;
  try {
    const timedOut = new Promise<never>((_, reject) => {
      timeout = setTimeout(
        () =>
          reject(
            new Error("Timed out stopping the packaged consumer process group"),
          ),
        20_000,
      );
    });
    const stopped = async (): Promise<void> => {
      if (closed !== undefined) await closed;
      if (child.pid !== undefined) await waitForProcessGroupExit(child.pid);
    };
    await Promise.race([stopped(), timedOut]);
  } catch (error) {
    signalProcessGroup(child, "SIGKILL");
    if (closed !== undefined) await closed;
    if (child.pid !== undefined) await waitForProcessGroupExit(child.pid);
    throw error;
  } finally {
    if (timeout !== undefined) clearTimeout(timeout);
  }
};

const waitForProcessExit = async (pid: number): Promise<void> => {
  const deadline = Date.now() + 20_000;
  while (Date.now() < deadline) {
    try {
      process.kill(pid, 0);
    } catch (error) {
      if ((error as NodeJS.ErrnoException).code === "ESRCH") return;
      throw error;
    }
    await delay(100);
  }
  throw new Error(`Timed out waiting for process ${pid} to exit`);
};

const waitForChildClose = async (
  child: ChildProcess,
): Promise<[number | null, NodeJS.Signals | null]> => {
  if (child.exitCode !== null || child.signalCode !== null)
    return [child.exitCode, child.signalCode];
  const timeout = AbortSignal.timeout(20_000);
  return (await once(child, "close", { signal: timeout })) as [
    number | null,
    NodeJS.Signals | null,
  ];
};

const runtimeProcesses = async (
  child: ChildProcess,
): Promise<RuntimeProcess[]> => {
  if (child.pid === undefined) throw new Error("Packaged consumer has no PID");
  const processList = await execute("ps", ["-eo", "pid=,pgid=,args="]);
  const members = processList.stdout
    .split("\n")
    .map((line) => line.trim().match(/^(\d+)\s+(\d+)\s+(.+)$/u))
    .filter((fields): fields is RegExpMatchArray => fields !== null)
    .filter((fields) => Number(fields[2]) === child.pid)
    .map((fields) => ({ pid: Number(fields[1]), command: fields[3] ?? "" }));
  return await Promise.all(
    members.map(async ({ pid, command }) => {
      try {
        const [executable, maps] = await Promise.all([
          readlink(`/proc/${pid}/exe`),
          readFile(`/proc/${pid}/maps`, "utf8"),
        ]);
        return { pid, command, executable, maps };
      } catch (error) {
        if ((error as NodeJS.ErrnoException).code === "ENOENT")
          return { pid, command, executable: "<exited>", maps: "" };
        throw error;
      }
    }),
  );
};

const assertRuntimeEvidence = async (
  child: ChildProcess,
  backend: Backend,
  diagnostics: () => string,
): Promise<number> => {
  let last = "";
  for (let attempt = 0; attempt < 300; attempt += 1) {
    const processes = await runtimeProcesses(child);
    const muon = processes.find(
      (process) => basename(process.executable) === "muon-core",
    );
    const cef = processes.filter((process) =>
      /--type=(?:renderer|gpu-process)/u.test(process.command),
    );
    const ozone = `--ozone-platform=${backend}`;
    const gdkLibrary = backend === "wayland" ? "libwayland-client" : "libX11";
    const gtkLibraries = (maps: string): string[] =>
      maps
        .split("\n")
        .flatMap(
          (line) =>
            line.match(
              /\/[^\s]*\/lib(?:gtk-3|gdk-3|glib-2\.0|gio-2\.0|gobject-2\.0)\.so[^\s]*/,
            )?.[0] ?? [],
        );
    const systemGtkLoaded = (maps: string): boolean => {
      const paths = gtkLibraries(maps);
      return (
        [
          "libgtk-3.so",
          "libgdk-3.so",
          "libglib-2.0.so",
          "libgio-2.0.so",
          "libgobject-2.0.so",
        ].every((name) => paths.some((path) => path.includes(name))) &&
        paths.every((path) => /^\/(?:usr\/)?lib\//.test(path))
      );
    };
    last = JSON.stringify(
      processes.map(({ pid, command, executable, maps }) => ({
        pid,
        command,
        executable,
        // GTK links both backend libraries. This confirms the runtime loader
        // source, but is not by itself a backend selection assertion.
        backendLibraryMapped: maps.includes(gdkLibrary),
        systemGtkLoaded: systemGtkLoaded(maps),
        gtkLibraries: [...new Set(gtkLibraries(maps))],
      })),
    );
    if (
      muon !== undefined &&
      systemGtkLoaded(muon.maps) &&
      cef.some((process) => process.command.includes(ozone))
    ) {
      console.info(
        `Packaged ${backend} runtime evidence (CEF cmdline and system GTK maps): ${last}`,
      );
      console.info(
        `GDK ${backend} is selected by the mutually exclusive display environment and successful GTK startup; linux-gtk-runtime.test.ts verifies the GdkDisplay type with the system GTK separately.`,
      );
      return muon.pid;
    }
    await delay(100);
  }
  throw new Error(
    `Packaged runtime did not load system GTK or CEF ${backend} ozone:\n${last}\n${diagnostics()}`,
  );
};

const stopViteParent = async (
  vite: ChildProcess,
  muonPids: readonly number[],
): Promise<void> => {
  if (vite.exitCode !== null || vite.signalCode !== null)
    throw new Error("Vite exited before the parent-stop assertion");
  const closed = waitForChildClose(vite);
  vite.kill("SIGTERM");
  const [code, signal] = await closed;
  expect(signal).toBeNull();
  // Node reports its handled SIGTERM as the conventional 128 + signal value.
  expect(code).toBe(143);
  await Promise.all(muonPids.map(waitForProcessExit));
};

const runtimeMuonPids = async (child: ChildProcess): Promise<number[]> =>
  (await runtimeProcesses(child))
    .filter((process) => basename(process.executable) === "muon-core")
    .map((process) => process.pid);

const resultText = (content: unknown): string =>
  (content as Array<{ type: string; text?: string }>)
    .filter((item) => item.type === "text")
    .map((item) => item.text ?? "")
    .join("\n");

const parseEvaluation = <Value>(text: string): Value => {
  const json = text.match(/^### Result\s*\n([\s\S]*?)\n###/mu)?.[1];
  if (json === undefined)
    throw new Error(`Unexpected browser evaluation result:\n${text}`);
  return JSON.parse(json) as Value;
};

const assertPage = async (
  port: number,
  artifacts: string,
  backend: Backend,
  lifecycle: Lifecycle,
): Promise<number> => {
  const screenshotPath = resolve(artifacts, "frame.png");
  await mkdir(artifacts, { recursive: true });
  const mcp = await createConnection({
    browser: { cdpEndpoint: `http://127.0.0.1:${port}` },
    outputDir: artifacts,
  });
  const [clientTransport, serverTransport] =
    InMemoryTransport.createLinkedPair();
  const client = new Client({ name: "muon-packaged-wayland", version: "1" });
  try {
    await mcp.connect(serverTransport);
    await client.connect(clientTransport);
    let selected = false;
    let latestTabs = "";
    for (let attempt = 0; attempt < 300; attempt += 1) {
      const tabs = await client.callTool({
        name: "browser_tabs",
        arguments: { action: "list" },
      });
      latestTabs = resultText(tabs.content);
      const index = latestTabs.match(
        /- (\d+):[^\n]*(?:asset:\/\/main\/|http:\/\/127\.0\.0\.1:)/u,
      )?.[1];
      if (index !== undefined) {
        await client.callTool({
          name: "browser_tabs",
          arguments: { action: "select", index: Number(index) },
        });
        selected = true;
        break;
      }
      await delay(100);
    }
    expect(selected, latestTabs).toBe(true);
    const ready = await client.callTool({
      name: "browser_wait_for",
      arguments: { text: "consumer graphics green" },
    });
    expect(ready.isError, resultText(ready.content)).not.toBe(true);
    const screenshot = await client.callTool({
      name: "browser_take_screenshot",
      arguments: {
        element: "consumer graphics",
        filename: screenshotPath,
        target: "#frame",
      },
    });
    expect(screenshot.isError, resultText(screenshot.content)).not.toBe(true);
    const image = PNG.sync.read(await readFile(screenshotPath));
    const pixelOffset =
      (Math.floor(image.height / 2) * image.width +
        Math.floor(image.width / 2)) *
      4;
    expect(
      Array.from(image.data.subarray(pixelOffset, pixelOffset + 4)),
    ).toEqual([0, 255, 0, 255]);
    const result = await client.callTool({
      name: "browser_evaluate",
      arguments: {
        function: `async () => ({
          ready: document.querySelector('#ready')?.textContent,
          environment: await window.muon.environments.getVariables(),
          processId: await window.muon.environments.getProcessId(),
        })`,
      },
    });
    const text = resultText(result.content);
    expect(result.isError, text).not.toBe(true);
    const value = parseEvaluation<{
      ready: string | undefined;
      environment: Record<string, string | undefined>;
      processId: number;
    }>(text);
    expect(value.ready).toBe("consumer Wayland page");
    if (backend === "wayland") {
      expect(value.environment.DISPLAY).toBe("");
      expect(value.environment.GDK_BACKEND).toBe("wayland");
      expect(value.environment.WAYLAND_DISPLAY).toBe("wayland-muon-test");
    } else {
      expect(value.environment.GDK_BACKEND).toBe("x11");
    }
    if (lifecycle !== undefined) {
      try {
        const action = await client.callTool({
          name: "browser_evaluate",
          arguments: {
            // These APIs destroy the page before their promise may settle.
            // The caller verifies their observable process lifecycle below.
            function: `async () => {
              await window.muon.browser.${lifecycle}();
              return "requested";
            }`,
          },
        });
        if (!action.isError)
          expect(resultText(action.content)).toContain("requested");
      } catch {
        // CDP may disconnect while the awaited API tears down this page.
      }
    }
    return value.processId;
  } finally {
    await rm(screenshotPath, { force: true });
    if (lifecycle === undefined)
      await client.callTool({ name: "browser_close", arguments: {} });
    await client.close();
    await mcp.close();
  }
};

const launchEnvironment = (
  consumer: string,
  backend: Backend,
): NodeJS.ProcessEnv => ({
  ...process.env,
  XDG_STATE_HOME: join(consumer, "state"),
  ...(backend === "wayland"
    ? { DISPLAY: "", GDK_BACKEND: "wayland" }
    : { GDK_BACKEND: "x11", WAYLAND_DISPLAY: "", XDG_SESSION_TYPE: "x11" }),
});

const launch = (
  command: string,
  args: string[],
  consumer: string,
  backend: Backend,
): ChildProcess =>
  spawn(command, args, {
    cwd: consumer,
    detached: true,
    env: launchEnvironment(consumer, backend),
    stdio: ["ignore", "pipe", "pipe"],
  });

const writeConsumer = async (consumer: string, port: number): Promise<void> => {
  await writeFile(
    join(consumer, "package.json"),
    JSON.stringify({
      name: "muon-wayland-consumer",
      private: true,
      type: "module",
    }),
  );
  await writeFile(
    join(consumer, "muon.json"),
    JSON.stringify({
      cdp: { enable: true, port },
      browser: {
        startPage: "asset://main/index.html",
        profilePath: join(consumer, "profile"),
      },
      plugin: {
        mode: "simple",
        pages: ["asset://main/**"],
        plugins: [
          {
            name: "internal",
            allow: [
              "muon.environments.getVariables",
              "muon.environments.getProcessId",
              "muon.browser.close",
              "muon.browser.recycle",
            ],
          },
        ],
      },
    }),
  );
  await writeFile(
    join(consumer, "index.html"),
    "<!doctype html><main id=ready>consumer Wayland page</main><div id=frame style='width:64px;height:64px;background:#00ff00'></div><output id=painted>consumer graphics green</output>",
  );
  await mkdir(join(consumer, "assets", "main"), { recursive: true });
  await writeFile(
    join(consumer, "assets", "main", "index.html"),
    "<!doctype html><main id=ready>consumer Wayland page</main><div id=frame style='width:64px;height:64px;background:#00ff00'></div><output id=painted>consumer graphics green</output>",
  );
  await writeFile(
    join(consumer, "vite.config.mjs"),
    "import muon from 'muon-ui/vite'; export default { plugins: [muon()] };\n",
  );
};

it.skipIf(archive === undefined)(
  "starts an installed package through Vite, muon run, and its built launcher",
  async () => {
    const consumer = await mkdtemp(join(tmpdir(), "muon-packaged-wayland-"));
    try {
      await execute(
        "npm",
        ["install", "--no-audit", "--no-fund", resolve(archive!), "vite"],
        { cwd: consumer },
      );
      if (oldPrepared !== undefined)
        await cp(resolve(oldPrepared), join(consumer, ".muon", "linux-amd64"), {
          recursive: true,
        });

      const vitePort = await reservePort();
      await writeConsumer(consumer, vitePort);
      const vite = launch(
        join(consumer, "node_modules/.bin/vite"),
        ["--host", "127.0.0.1", "--port", String(await reservePort())],
        consumer,
        backend,
      );
      const viteDiagnostics = readDiagnostics(vite);
      try {
        await waitForCdp(vite, vitePort, viteDiagnostics);
        await assertRuntimeEvidence(vite, backend, viteDiagnostics);
        const viteMuons = await runtimeMuonPids(vite);
        await assertPage(
          vitePort,
          join(consumer, "playwright", "vite"),
          backend,
          undefined,
        );
        // Vite is the parent here. Do not terminate the whole process group:
        // this proves its ordinary shutdown also reaps the spawned Muon child.
        await stopViteParent(vite, viteMuons);
      } finally {
        await stop(vite);
      }
      if (oldPrepared !== undefined) {
        const prepared = await readdir(join(consumer, ".muon", "linux-amd64"), {
          recursive: true,
        });
        const old = await readdir(resolve(oldPrepared), { recursive: true });
        const gtkFiles = old.filter((file) =>
          /^(?:lib(?:gtk|gdk|glib|gio|gobject|gmodule|pango|atk)|linux\/)/.test(
            file,
          ),
        );
        expect(gtkFiles.length).toBeGreaterThan(0);
        expect(prepared.filter((file) => gtkFiles.includes(file))).toEqual([]);
      }

      const runPort = await reservePort();
      await writeConsumer(consumer, runPort);
      const direct = launch(
        process.execPath,
        [
          join(consumer, "node_modules/muon-ui/dist/cli.cjs"),
          "run",
          "--assets",
          join(consumer, "assets"),
        ],
        consumer,
        backend,
      );
      const directDiagnostics = readDiagnostics(direct);
      try {
        await waitForCdp(direct, runPort, directDiagnostics);
        const firstMuon = await assertRuntimeEvidence(
          direct,
          backend,
          directDiagnostics,
        );
        const firstMuons = await runtimeMuonPids(direct);
        const requestedRecycle = await assertPage(
          runPort,
          join(consumer, "playwright", "run-recycle"),
          backend,
          "recycle",
        );
        expect(requestedRecycle).toBe(firstMuon);
        await Promise.all(firstMuons.map(waitForProcessExit));
        await waitForCdp(direct, runPort, directDiagnostics);
        const recycledMuon = await assertRuntimeEvidence(
          direct,
          backend,
          directDiagnostics,
        );
        expect(recycledMuon).not.toBe(firstMuon);
        await assertPage(
          runPort,
          join(consumer, "playwright", "run"),
          backend,
          undefined,
        );
      } finally {
        await stop(direct);
      }

      const launcherPort = await reservePort();
      await writeConsumer(consumer, launcherPort);
      const viteConfig = join(consumer, "vite.config.mjs");
      const viteConfigBackup = join(consumer, "vite.config.mjs.disabled");
      await rename(viteConfig, viteConfigBackup);
      const built = await (async () => {
        try {
          return await execute(
            process.execPath,
            [
              join(consumer, "node_modules/muon-ui/dist/cli.cjs"),
              "build",
              "--target",
              "linux-amd64",
              "--assets",
              join(consumer, "assets"),
              "--json",
            ],
            { cwd: consumer, maxBuffer: 8 * 1024 * 1024 },
          );
        } finally {
          await rename(viteConfigBackup, viteConfig);
        }
      })();
      const result = JSON.parse(built.stdout) as {
        targets: { launcherPath: string }[];
      };
      const launcher = launch(
        result.targets[0]!.launcherPath,
        [],
        consumer,
        backend,
      );
      const launcherDiagnostics = readDiagnostics(launcher);
      try {
        await waitForCdp(launcher, launcherPort, launcherDiagnostics);
        const launcherMuon = await assertRuntimeEvidence(
          launcher,
          backend,
          launcherDiagnostics,
        );
        const launcherMuons = await runtimeMuonPids(launcher);
        const requestedClose = await assertPage(
          launcherPort,
          join(consumer, "playwright", "launcher"),
          backend,
          "close",
        );
        expect(requestedClose).toBe(launcherMuon);
        const [code, signal] = await waitForChildClose(launcher);
        expect(signal).toBeNull();
        expect(code).toBe(0);
        await Promise.all(launcherMuons.map(waitForProcessExit));
      } finally {
        await stop(launcher);
      }
    } finally {
      await rm(consumer, { recursive: true, force: true });
    }
  },
  1_800_000,
);

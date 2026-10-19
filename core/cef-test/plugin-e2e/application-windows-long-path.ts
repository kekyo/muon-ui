// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { createHash } from "node:crypto";
import {
  mkdtemp as localMkdtemp,
  mkdir as localMkdir,
  readFile as localReadFile,
  rm as localRm,
  writeFile as localWriteFile,
} from "node:fs/promises";
import { tmpdir as localTmpdir } from "node:os";
import { join as localJoin, win32 } from "node:path";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { InMemoryTransport } from "@modelcontextprotocol/sdk/inMemory.js";
import { createConnection } from "@playwright/mcp";
import { afterAll, beforeAll, expect, it, vi } from "vitest";
import {
  describeMuonPluginBridge,
  execFileAsync,
  join,
  mkdtemp,
  MUON_PORT,
  rm,
  tmpdir,
} from "./shared.js";
import { requireWindowsRemoteContext } from "./windows-context.js";

const title = "Long path application";
const html = `<!doctype html><meta charset="utf-8"><title>${title}</title><body>Unicode asset 日本語😀</body>`;

describeMuonPluginBridge("Windows long path application inputs", () => {
  let root: string;
  let assets: string;
  let fixture: string;
  let sequence = 0;

  const longDirectory = (length: number): string => {
    let path = win32.join(root, "data", `日本語😀-${++sequence}`);
    while (length - path.length > 100) path = win32.join(path, "p".repeat(70));
    path = win32.join(path, "q".repeat(length - path.length - 1));
    expect(path.length).toBe(length);
    return path;
  };

  const write = async (
    path: string,
    content: string | Buffer,
  ): Promise<void> => {
    const { agent } = requireWindowsRemoteContext();
    const bytes = typeof content === "string" ? Buffer.from(content) : content;
    if (path.length < 240 && !path.startsWith("\\\\?\\")) {
      await agent.files.mkdir(win32.dirname(path), { recursive: true });
      await agent.files.writeFile(path, bytes);
      return;
    }
    // Transfer through a short path, then let the independent native fixture
    // prepare the exact input. The agent itself has legacy path limits.
    const temporary = win32.join(root, `upload-${++sequence}.bin`);
    const native = path.startsWith("\\\\?\\") ? path : "\\\\?\\" + path;
    await agent.files.writeFile(temporary, bytes);
    try {
      await fixtureCommand(["mkdir", win32.dirname(native)]);
      await fixtureCommand(["remove", native]);
      await fixtureCommand(["copy", native, temporary]);
    } finally {
      await agent.files.remove(temporary);
    }
  };

  const read = async (path: string): Promise<Buffer> => {
    const { agent } = requireWindowsRemoteContext();
    const temporary = win32.join(root, `download-${++sequence}.bin`);
    await fixtureCommand(["copy", "\\\\?\\" + temporary, "\\\\?\\" + path]);
    try {
      return Buffer.from(await agent.files.readFile(temporary));
    } finally {
      await agent.files.remove(temporary);
    }
  };

  const fixtureCommand = async (args: string[]): Promise<void> => {
    const process =
      await requireWindowsRemoteContext().agent.processes.launchManaged({
        path: fixture,
        arguments: args,
        captureStdout: true,
        captureStderr: true,
        createNoWindow: true,
      });
    try {
      expect(
        (await process.waitForExit({ timeoutMs: 30000 })).root.exitCode,
        await process.stderrText(),
      ).toBe(0);
    } finally {
      await process.releaseAsync();
    }
  };

  const configuration = () => ({
    asset: { sourcePath: assets },
    network: { allow: ["asset://main/**"] },
    plugin: {
      mode: "simple",
      pages: ["asset://main/**"],
      plugins: [{ name: "internal", allow: ["muon.**"] }],
    },
    browser: {
      titleBarType: "native",
      profilePath: win32.join(root, `profile-${++sequence}`),
      startPage: "asset://main/index.html",
    },
    cdp: { enable: true, port: MUON_PORT },
  });

  const run = async (
    configPath: string,
    config: unknown,
    source: string,
    environment: Record<string, string>,
  ): Promise<unknown> => {
    const context = requireWindowsRemoteContext();
    await write(configPath, JSON.stringify(config));
    const relay = await context.agent.processes.launchManaged({
      path: context.runtime.relayExecutablePath,
      arguments: ["0", String(MUON_PORT)],
      captureStdout: true,
      captureStderr: true,
      createNoWindow: true,
    });
    let process:
      | Awaited<ReturnType<typeof context.agent.processes.launchManaged>>
      | undefined;
    try {
      const port = await vi.waitUntil(
        async () => {
          const match = /^MUON_CDP_RELAY_PORT=(\d+)$/m.exec(
            await relay.stdoutText(),
          );
          return match ? Number(match[1]) : false;
        },
        { timeout: 30000, interval: 100 },
      );
      process = await context.agent.processes.launchManaged({
        path: win32.join(
          context.runtime.debugRuntimeDirectory,
          "muon-core.exe",
        ),
        arguments: ["-c", configPath],
        workingDirectory: root,
        environment,
        captureStdout: true,
        captureStderr: true,
      });
      const endpoint = `http://${context.cdpHost}:${port}`;
      await vi.waitUntil(
        async () => {
          const snapshot = await process!.rootSnapshot();
          if (!snapshot.running)
            throw new Error(
              `Muon exited (${snapshot.exitCode}): ${await process!.stderrText()}`,
            );
          try {
            const response = await fetch(endpoint + "/json/list", {
              signal: AbortSignal.timeout(1000),
            });
            const pages = (await response.json()) as { title: string }[];
            return pages.some((page) => page.title === title);
          } catch {
            return false;
          }
        },
        { timeout: 30000, interval: 100 },
      );
      const server = await createConnection({
        browser: { cdpEndpoint: endpoint },
      });
      const client = new Client({
        name: "muon-application-path",
        version: "1.0.0",
      });
      try {
        const [a, b] = InMemoryTransport.createLinkedPair();
        await server.connect(b);
        await client.connect(a);
        const result = await client.callTool({
          name: "browser_evaluate",
          arguments: { function: source },
        });
        const text = (result.content as { type: string; text: string }[])
          .filter((item) => item.type === "text")
          .map((item) => item.text)
          .join("\n");
        expect(result.isError, text).not.toBe(true);
        const match = /### Result\n([\s\S]*?)(?:\n###|$)/u.exec(text);
        expect(match, text).not.toBeNull();
        const value = JSON.parse(match![1]!);
        await client.callTool({
          name: "browser_tabs",
          arguments: { action: "close" },
        });
        expect(
          (await process.waitForExit({ timeoutMs: 30000 })).root.exitCode,
          await process.stderrText(),
        ).toBe(0);
        return value;
      } finally {
        await client.close();
        await server.close();
      }
    } catch (error) {
      throw new Error(
        `${String(error)}\n${process === undefined ? "" : await process.stderrText()}`,
      );
    } finally {
      await process?.releaseAsync();
      await relay.releaseAsync();
    }
  };

  beforeAll(async () => {
    const context = requireWindowsRemoteContext();
    root = await mkdtemp(join(tmpdir(), "muon-app-path-"));
    assets = win32.join(root, "assets");
    await write(win32.join(assets, "main", "index.html"), html);
    await execFileAsync("make", [
      "windows-long-path-fixture",
      "windows-path-plugin",
      `WINDOWS_TARGET=${context.runtime.target}`,
    ]);
    fixture = win32.join(root, "fixture.exe");
    await write(
      fixture,
      await localReadFile(
        localJoin(
          ".run",
          "windows-long-path",
          context.runtime.target,
          "fixture.exe",
        ),
      ),
    );
    const check = await context.agent.processes.launchManaged({
      path: "powershell.exe",
      arguments: [
        "-NoProfile",
        "-NonInteractive",
        "-Command",
        "[pscustomobject]@{ Policy=(Get-ItemProperty 'HKLM:\\SYSTEM\\CurrentControlSet\\Control\\FileSystem').LongPathsEnabled; Administrator=([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator) } | ConvertTo-Json -Compress",
      ],
      captureStdout: true,
      captureStderr: true,
      createNoWindow: true,
    });
    try {
      expect(
        (await check.waitForExit({ timeoutMs: 30000 })).root.exitCode,
      ).toBe(0);
      expect(JSON.parse(await check.stdoutText())).toEqual({
        Policy: 0,
        Administrator: false,
      });
    } finally {
      await check.releaseAsync();
    }
  });

  afterAll(async () => {
    if (root !== undefined) {
      await fixtureCommand(["remove", "\\\\?\\" + win32.join(root, "data")]);
      await rm(root, { recursive: true, force: true });
    }
  });

  for (const length of [140, 300, 1100]) {
    it(`loads a Unicode -c path in a ${length}-unit directory and resolves relative assets`, async () => {
      const directory = longDirectory(length);
      await write(win32.join(directory, "資料😀", "main", "index.html"), html);
      const config = configuration();
      config.asset.sourcePath = "./資料😀";
      expect(
        await run(
          win32.join(directory, "設定😀.json"),
          config,
          "() => document.body.textContent",
          {},
        ),
      ).toBe("Unicode asset 日本語😀");
    });
  }

  it("keeps an explicitly extended config path and its relative assets intact", async () => {
    const directory = longDirectory(300);
    await write(win32.join(directory, "資料😀", "main", "index.html"), html);
    const config = configuration();
    config.asset.sourcePath = "./資料😀";
    expect(
      await run(
        "\\\\?\\" + win32.join(directory, "設定😀.json"),
        config,
        "() => document.body.textContent",
        {},
      ),
    ).toBe("Unicode asset 日本語😀");
  });

  it("serves long Unicode directory assets and rejects a junction outside the asset host", async () => {
    const directory = longDirectory(500);
    await write(win32.join(directory, "main", "index.html"), html);
    await write(win32.join(directory, "main", "内容😀.txt"), "inside");
    const outside = win32.join(root, "outside");
    await write(win32.join(outside, "secret.txt"), "outside-secret");
    await fixtureCommand([
      "junction",
      "\\\\?\\" + win32.join(directory, "main", "escape"),
      "\\\\?\\" + outside,
    ]);
    const config = configuration();
    config.asset.sourcePath = directory;
    expect(
      await run(
        win32.join(root, "directory.json"),
        config,
        `async () => {
      const inside = await fetch("asset://main/" + encodeURIComponent("内容😀.txt"));
      const outside = await fetch("asset://main/escape/secret.txt");
      return { inside: await inside.text(), outsideStatus: outside.status, outside: await outside.text() };
    }`,
        {},
      ),
    ).toEqual({ inside: "inside", outsideStatus: 403, outside: "" });
    expect(
      Buffer.from(
        await requireWindowsRemoteContext().agent.files.readFile(
          win32.join(outside, "secret.txt"),
        ),
      ).toString(),
    ).toBe("outside-secret");
  });

  it("reads and verifies a ZIP at a long Unicode path and rejects a changed signature", async () => {
    const local = await localMkdtemp(
      localJoin(localTmpdir(), "muon-zip-path-"),
    );
    let archive: Buffer;
    try {
      await localMkdir(localJoin(local, "main"));
      await localWriteFile(localJoin(local, "main", "index.html"), html);
      await execFileAsync("zip", ["-q", "assets.zip", "main/index.html"], {
        cwd: local,
      });
      archive = await localReadFile(localJoin(local, "assets.zip"));
    } finally {
      await localRm(local, { recursive: true, force: true });
    }
    const path = win32.join(longDirectory(500), "資料😀.zip");
    await write(path, archive);
    const salt = "deadbeef";
    const signature = createHash("sha256")
      .update(archive)
      .update(Buffer.from(salt, "hex"))
      .digest("hex");
    const config = {
      ...configuration(),
      asset: { sourcePath: path, signature, salt },
    };
    expect(
      await run(
        win32.join(root, "zip.json"),
        config,
        "() => document.body.textContent",
        {},
      ),
    ).toBe("Unicode asset 日本語😀");
    config.asset.signature = "0".repeat(64);
    await expect(
      run(win32.join(root, "zip-mismatch.json"), config, "() => true", {}),
    ).rejects.toThrow(/asset.signature does not match/);
  });

  it("creates and writes a log under a long Unicode parent", async () => {
    const path = win32.join(longDirectory(500), "ログ😀.txt");
    const config = {
      ...configuration(),
      log: {
        level: "info",
        output: { type: "file", path },
        sources: { console: "info" },
      },
    };
    expect(
      await run(
        win32.join(root, "log.json"),
        config,
        "() => { const script = document.createElement('script'); script.textContent = \"console.info('long-path-log-日本語😀')\"; document.documentElement.appendChild(script); script.remove(); return true; }",
        {},
      ),
    ).toBe(true);
    const log = (await read(path)).toString();
    expect(log).toContain("long-path-log-日本語😀");
  });

  const preparePlugin = async (directory: string) => {
    const target = requireWindowsRemoteContext().runtime.target;
    const plugin = await localReadFile(
      localJoin(".run", "windows-long-path", target, "path-plugin.dll"),
    );
    const name = "path-試験😀";
    await write(win32.join(directory, name + ".dll"), plugin);
    await write(
      win32.join(directory, "path-dependency.dll"),
      await localReadFile(
        localJoin(".run", "windows-long-path", target, "path-dependency.dll"),
      ),
    );
    const salt = "aabbcc";
    return {
      name,
      allow: ["muon.test.path.value"],
      salt,
      signature: createHash("sha256")
        .update(plugin)
        .update(Buffer.from(salt, "hex"))
        .digest("hex"),
    };
  };

  it("loads a signed Unicode plugin with its adjacent DLL from a long directory", async () => {
    const directory = longDirectory(500);
    const plugin = await preparePlugin(directory);
    const config = {
      ...configuration(),
      plugin: {
        mode: "simple",
        pages: ["asset://main/**"],
        path: directory,
        plugins: [plugin],
      },
    };
    expect(
      await run(
        win32.join(root, "plugin.json"),
        config,
        "async () => await muon.test.path.value()",
        {},
      ),
    ).toBe(42);
    plugin.signature = "0".repeat(64);
    await expect(
      run(win32.join(root, "plugin-mismatch.json"), config, "() => true", {}),
    ).rejects.toThrow(/Plugin signature mismatch/);
  });

  for (const length of [140, 500]) {
    it(`inspects a signed plugin via a Unicode input path in a ${length}-unit directory`, async () => {
      const context = requireWindowsRemoteContext();
      const directory = longDirectory(length);
      const plugin = await preparePlugin(directory);
      const inspector = win32.join(root, "inspector.exe");
      await write(
        inspector,
        await localReadFile(
          localJoin(
            "..",
            "..",
            "builder",
            ".run",
            `test-${context.runtime.target}-release`,
            "muon-plugin-inspector.exe",
          ),
        ),
      );
      const input = win32.join(directory, "入力😀.json");
      await write(
        input,
        JSON.stringify({ path: directory, plugins: [plugin] }),
      );
      const process = await context.agent.processes.launchManaged({
        path: inspector,
        arguments: [input],
        workingDirectory: root,
        captureStdout: true,
        captureStderr: true,
        createNoWindow: true,
      });
      try {
        expect(
          (await process.waitForExit({ timeoutMs: 30000 })).root.exitCode,
          await process.stderrText(),
        ).toBe(0);
        expect(JSON.parse(await process.stdoutText())).toEqual({
          plugins: [{ name: plugin.name, functions: ["muon.test.path.value"] }],
        });
      } finally {
        await process.releaseAsync();
      }
    });
  }

  for (const source of [
    "profilePath",
    "extended profilePath",
    "LOCALAPPDATA",
  ] as const) {
    it(`rejects an unsupported ${source} and persists data in an explicit short Unicode profile`, async () => {
      const directory = longDirectory(300);
      const config = configuration();
      const environment: Record<string, string> = {};
      const input =
        source === "extended profilePath" ? "\\\\?\\" + directory : directory;
      if (source !== "LOCALAPPDATA") config.browser.profilePath = input;
      else {
        const browser = config.browser as Record<string, unknown>;
        delete browser.profilePath;
        environment.LOCALAPPDATA = input;
        environment.MUON_LAUNCHER_APP_ID = "long-path-test";
      }
      const path = win32.join(root, `${source}.json`);
      await expect(
        run(path, config, "() => document.title", environment),
      ).rejects.toThrow(/browser\.profilePath.*short.*Windows path/);
      config.browser.profilePath = longDirectory(140);
      expect(
        await run(
          path,
          config,
          "() => { const old = localStorage.getItem('path-test'); localStorage.setItem('path-test', 'saved-😀'); return old; }",
          environment,
        ),
      ).toBeNull();
      expect(
        await run(
          path,
          config,
          "() => localStorage.getItem('path-test')",
          environment,
        ),
      ).toBe("saved-😀");
      // Observe the actual CEF profile on disk as well as localStorage. A CEF
      // fallback can preserve values across launches in a different location.
      const entries = await requireWindowsRemoteContext().agent.files.readdir(
        config.browser.profilePath,
      );
      expect(entries.map((entry) => entry.name)).toEqual(
        expect.arrayContaining(["Default", "Local State"]),
      );
    });
  }
});

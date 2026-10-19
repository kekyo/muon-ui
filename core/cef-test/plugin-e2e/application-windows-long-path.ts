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
      "windows-path-process",
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

  for (const length of [140, 300, 1100]) {
    it(`runs an executor child at ${length} units with Unicode arguments, environment and streams`, async () => {
      const executable = win32.join(longDirectory(length), "実行😀.exe");
      await write(
        executable,
        await localReadFile(
          localJoin(
            ".run",
            "windows-long-path",
            requireWindowsRemoteContext().runtime.target,
            "path-process.exe",
          ),
        ),
      );
      const args = ["io", '引用"と空白 😀', "末尾\\", "", "長".repeat(1400)];
      const value = "環境😀" + executable;
      const result = await run(
        win32.join(root, "executor.json"),
        configuration(),
        `async () => {
          const child = await muon.executor.spawn(${JSON.stringify({ command: executable, args, cwd: root, env: { MUON_PATH_VALUE: value } })});
          await child.writeStdin("入力😀\\n");
          await child.closeStdin();
          const result = await child.wait();
          const search = await muon.executor.spawn({command: 'cmd.exe', args: ['/d', '/c', 'exit', '23'], cwd: ${JSON.stringify(root)}});
          const searchCode = (await search.wait()).exitCode;
          const decoder = new TextDecoder();
          const stdout = decoder.decode(result.stdout);
          const newline = stdout.indexOf('\\n');
          return {code: result.exitCode, searchCode, report: JSON.parse(stdout.slice(0, newline)),
            stdin: stdout.slice(newline + 1), stderr: decoder.decode(result.stderr)};
        }`,
        {},
      );
      expect(result).toEqual({
        code: 37,
        searchCode: 23,
        report: { args, cwd: root, value, appId: "" },
        stdin: "入力😀\n",
        stderr: "process-stderr\n",
      });
    });
  }

  it("loads an executor DLL by a long Unicode path and still finds a bare system library", async () => {
    const directory = longDirectory(500);
    await preparePlugin(directory);
    expect(
      await run(
        win32.join(root, "executor-library.json"),
        configuration(),
        `async () => {
        const executor = muon.executor;
        const library = await executor.loadLibrary(${JSON.stringify(win32.join(directory, "path-dependency.dll"))});
        try {
          const value = await library.getFunction('path_dependency_value', {argTypes: [], returnType: executor.int32Type});
          const system = await executor.loadLibrary('kernel32.dll');
          try {
            const pid = await system.getFunction('GetCurrentProcessId', {argTypes: [], returnType: executor.uint32Type});
            return {value: await value(), hasPid: (await pid()) > 0};
          } finally { await system.release(); }
        } finally { await library.release(); }
      }`,
        {},
      ),
    ).toEqual({ value: 42, hasPid: true });
  });

  it("terminates a long-path executor child and its descendant after readiness", async () => {
    const executable = win32.join(longDirectory(500), "子孫😀.exe");
    await write(
      executable,
      await localReadFile(
        localJoin(
          ".run",
          "windows-long-path",
          requireWindowsRemoteContext().runtime.target,
          "path-process.exe",
        ),
      ),
    );
    const ids = (await run(
      win32.join(root, "executor-tree.json"),
      configuration(),
      `async () => {
        let ready;
        const started = new Promise(resolve => { ready = resolve; });
        let output = '';
        const child = await muon.executor.spawn({ command: ${JSON.stringify(executable)}, args: ['tree'],
          cwd: ${JSON.stringify(root)}, onStdout: chunk => {
            output += new TextDecoder().decode(chunk);
            if (output.includes('\\n')) ready(Number(output.trim()));
          }});
        try {
          const descendant = await started;
          await child.kill();
          await child.wait();
          return [child.processId, descendant];
        } finally { await child.release(); }
      }`,
      {},
    )) as number[];
    expect(ids).toHaveLength(2);
    for (const id of ids) {
      expect(id).toBeGreaterThan(0);
      expect(
        (
          await requireWindowsRemoteContext().agent.processes.waitForExit(id, {
            timeoutMs: 30000,
          })
        ).running,
      ).toBe(false);
    }
  });

  it("rejects a long executor cwd without substituting another directory", async () => {
    const directory = longDirectory(300);
    await write(win32.join(directory, "marker"), "cwd");
    const executable = win32.join(root, "process.exe");
    await write(
      executable,
      await localReadFile(
        localJoin(
          ".run",
          "windows-long-path",
          requireWindowsRemoteContext().runtime.target,
          "path-process.exe",
        ),
      ),
    );
    for (const cwd of [directory, "\\\\?\\" + directory]) {
      await expect(
        run(
          win32.join(root, "executor-cwd.json"),
          configuration(),
          `async () => {
        const child = await muon.executor.spawn(${JSON.stringify({ command: executable, cwd })});
        const result = await child.wait();
        return JSON.parse(new TextDecoder().decode(result.stdout)).cwd;
      }`,
          {},
        ),
      ).rejects.toThrow(/cwd.*short.*Windows/);
    }
  });

  for (const length of [140, 300, 1100]) {
    it(`handles a ${length}-unit Unicode Node project with an explicit short-cwd workaround when needed`, async () => {
      const directory = longDirectory(length);
      await write(
        win32.join(directory, "package.json"),
        JSON.stringify({ type: "module" }),
      );
      await write(
        win32.join(directory, "実装😀.mjs"),
        "export const value = () => 'Node 日本語😀'; export const cwd = () => process.cwd();",
      );
      const config = { ...configuration(), node: { project: directory } };
      let specifier = "./実装😀.mjs";
      if (length >= 260) {
        await expect(
          run(
            win32.join(root, "node-project-long.json"),
            config,
            "async () => { const node = await muon.node.createNode(); await node.release(); }",
            {},
          ),
        ).rejects.toThrow(
          /node\.project requires a short Windows working directory/,
        );
        config.node.project = win32.join(root, "data");
        await write(
          win32.join(config.node.project, "package.json"),
          JSON.stringify({ type: "module" }),
        );
        specifier =
          "./" +
          win32
            .relative(config.node.project, win32.join(directory, "実装😀.mjs"))
            .replaceAll("\\", "/");
      }
      expect(
        await run(
          win32.join(root, "node-project.json"),
          config,
          `async () => {
          const node = await muon.node.createNode();
          try {
            const module = await node.importModule(${JSON.stringify(specifier)});
            return {value: await module.value(), cwd: await module.cwd()};
          } finally { await node.release(); }
        }`,
          {},
        ),
      ).toEqual({ value: "Node 日本語😀", cwd: config.node.project });
    });
  }
});

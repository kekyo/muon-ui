// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { readFile as readLocalFile } from "node:fs/promises";
import { join as joinLocal, win32 } from "node:path";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { InMemoryTransport } from "@modelcontextprotocol/sdk/inMemory.js";
import { createConnection } from "@playwright/mcp";
import { afterAll, beforeAll, expect, it } from "vitest";
import {
  describeMuonPluginBridge,
  execFileAsync,
  join,
  mkdtemp,
  rm,
  tmpdir,
  withMuon,
  writeFile,
} from "./shared.js";
import { getWindowsRemoteContext } from "./windows-context.js";

const directoryOfLength = (root: string, length: number): string => {
  let result = root;
  while (length - result.length > 100)
    result = win32.join(result, "d".repeat(70));
  result = win32.join(result, "f".repeat(length - result.length - 1));
  expect(result.length).toBe(length);
  return result;
};

describeMuonPluginBridge("Windows long path filesystem API", () => {
  let directory: string;
  let fixture: string;

  const runFixture = async (
    operation: string,
    path: string,
    args: string[],
  ): Promise<void> => {
    const context = getWindowsRemoteContext()!;
    const process = await context.agent.processes.launchManaged({
      path: fixture,
      arguments: [operation, "\\\\?\\" + path, ...args],
      workingDirectory: directory,
      captureStdout: true,
      captureStderr: true,
      createNoWindow: true,
    });
    try {
      const result = await process.waitForExit({ timeoutMs: 30000 });
      expect(result.root.exitCode, await process.stderrText()).toBe(0);
    } finally {
      await process.releaseAsync();
    }
  };

  beforeAll(async () => {
    const context = getWindowsRemoteContext()!;
    const target = context.runtime.target;
    directory = await mkdtemp(join(tmpdir(), "muon-bcl-path-"));
    fixture = join(directory, "fixture.exe");
    await execFileAsync("make", [
      "windows-long-path-fixture",
      `WINDOWS_TARGET=${target}`,
    ]);
    await writeFile(
      fixture,
      await readLocalFile(
        joinLocal(".run", "windows-long-path", target, "fixture.exe"),
      ),
    );
    const policy = await context.agent.processes.launchManaged({
      path: "reg.exe",
      arguments: [
        "query",
        String.raw`HKLM\SYSTEM\CurrentControlSet\Control\FileSystem`,
        "/v",
        "LongPathsEnabled",
      ],
      captureStdout: true,
      captureStderr: true,
      createNoWindow: true,
    });
    try {
      expect(
        (await policy.waitForExit({ timeoutMs: 30000 })).root.exitCode,
      ).toBe(0);
      expect(await policy.stdoutText()).toMatch(
        /LongPathsEnabled\s+REG_DWORD\s+0x0\b/i,
      );
    } finally {
      await policy.releaseAsync();
    }
    const privilege = await context.agent.processes.launchManaged({
      path: "powershell.exe",
      arguments: [
        "-NoProfile",
        "-NonInteractive",
        "-Command",
        "([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)",
      ],
      captureStdout: true,
      captureStderr: true,
      createNoWindow: true,
    });
    try {
      expect(
        (await privilege.waitForExit({ timeoutMs: 30000 })).root.exitCode,
      ).toBe(0);
      expect((await privilege.stdoutText()).trim()).toBe("False");
    } finally {
      await privilege.releaseAsync();
    }
  });

  afterAll(async () => {
    if (directory !== undefined) {
      await runFixture("remove", join(directory, "data"), []);
      await rm(directory, { recursive: true, force: true });
    }
  });

  // Use the public API through Playwright MCP. The independent native fixture
  // only prepares and removes test data, so it cannot hide a product I/O error.
  const evaluate = async <T>(source: string): Promise<T> => {
    let value: T | undefined;
    await withMuon([], async () => {
      const context = getWindowsRemoteContext()!;
      const server = await createConnection({
        browser: {
          cdpEndpoint: `http://${context.cdpHost}:${context.cdpPort}`,
        },
      });
      const client = new Client({ name: "muon-bcl-path", version: "1.0.0" });
      const [clientTransport, serverTransport] =
        InMemoryTransport.createLinkedPair();
      const call = async (
        name: string,
        arguments_: Record<string, unknown>,
      ): Promise<string> => {
        const result = await client.callTool({ name, arguments: arguments_ });
        const text = (result.content as { type: string; text: string }[])
          .filter((item) => item.type === "text")
          .map((item) => item.text)
          .join("\n");
        expect(result.isError, text).not.toBe(true);
        return text;
      };
      try {
        await server.connect(serverTransport);
        await client.connect(clientTransport);
        const tabs = await call("browser_tabs", { action: "list" });
        const matches = [
          ...tabs.matchAll(/^- (\d+): (?:\(current\) )?\[muon test\]\(/gmu),
        ];
        expect(matches).toHaveLength(1);
        await call("browser_tabs", {
          action: "select",
          index: Number(matches[0]![1]),
        });
        const text = await call("browser_evaluate", {
          function: `async () => { const fs = window.muon.fs; ${source} }`,
        });
        const match = /### Result\n([\s\S]*?)(?:\n###|$)/u.exec(text);
        expect(match, text).not.toBeNull();
        value = JSON.parse(match![1]!) as T;
      } finally {
        await client.close();
        await server.close();
      }
    });
    return value as T;
  };

  // .NET IOInputs.GetPathsLongerThanMaxPath plus Muon's range, append and
  // metadata API contract; UTF-16 units, not UTF-8 byte counts.
  // https://github.com/dotnet/runtime/blob/v10.0.0/src/libraries/System.Runtime/tests/System.IO.FileSystem.Tests/PortedCommon/IOInputs.cs
  it.each([140, 259, 260, 261, 262, 300, 1100, 4200])(
    "roundtrips all file operations at %i UTF-16 units",
    async (length) => {
      const parent = directoryOfLength(
        join(directory, "data", String(length), "日本語 😀"),
        length - 9,
      );
      const path = win32.join(parent, "file.bin");
      expect(path.length).toBe(length);
      await runFixture("mkdir", parent, []);
      const result = await evaluate(`
        const parent = ${JSON.stringify(parent)};
        const path = ${JSON.stringify(path)};
        await fs.writeFile(path, Uint8Array.from([1, 2, 3, 4]));
        const range = Array.from(new Uint8Array(await fs.readFile(path, { position: 1, length: 2 })));
        await fs.writeFile(path, Uint8Array.from([9]), { position: 2 });
        await fs.appendFile(path, Uint8Array.from([5]));
        const stat = await fs.stat(path);
        const lstat = await fs.lstat(path);
        const exists = await fs.exists(path);
        const access = await fs.access(path, { mode: ["read", "write"] });
        const copy = parent + "\\\\copy.bin";
        await fs.copyFile(path, copy);
        let overwriteRejected = false;
        try { await fs.copyFile(path, copy, { overwrite: false }); }
        catch { overwriteRejected = true; }
        const moved = parent + "\\\\move.bin";
        await fs.rename(copy, moved);
        await fs.truncate(moved, 3);
        const truncated = Array.from(new Uint8Array(await fs.readFile(moved)));
        const canonical = await fs.realpath(path);
        const roundtrip = Array.from(new Uint8Array(await fs.readFile(canonical)));
        const text = parent + "\\\\text.txt";
        await fs.writeTextFile(text, "日本語", "utf8");
        await fs.appendTextFile(text, "😀", "utf8");
        const textValue = await fs.readTextFile(text, "utf8");
        const uri = "file:///" + encodeURI(text.replaceAll("\\\\", "/"));
        const uriValue = await fs.readTextFile(uri, "utf8");
        const entries = await fs.readdir(parent, { withFileTypes: true });
        await fs.unlink(moved);
        await fs.rm(text);
        const empty = parent + "\\\\empty";
        await fs.mkdir(empty);
        await fs.rmdir(empty);
        return { range, stat: [stat.type, stat.size], lstat: [lstat.type, lstat.size],
          exists, access, overwriteRejected, truncated, roundtrip, textValue, uriValue,
          canonicalExtended: canonical.startsWith("\\\\\\\\?\\\\"),
          entries: entries.map(e => [e.name, e.isFile()]).sort(),
          missing: [await fs.exists(copy), await fs.exists(moved), await fs.exists(text), await fs.exists(empty)] };
      `);
      expect(result).toEqual({
        range: [2, 3],
        stat: ["file", 5],
        lstat: ["file", 5],
        exists: true,
        access: true,
        overwriteRejected: true,
        truncated: [1, 2, 9],
        roundtrip: [1, 2, 9, 4, 5],
        textValue: "日本語😀",
        uriValue: "日本語😀",
        canonicalExtended: false,
        entries: [
          ["file.bin", true],
          ["move.bin", true],
          ["text.txt", true],
        ],
        missing: [false, false, false, false],
      });
    },
  );

  // Path.GetFullPath resolves relative inputs before the native long-path call.
  it("resolves a long relative Unicode path against the runtime working directory", async () => {
    const parent = directoryOfLength(
      join(directory, "data", "relative", "日本語 😀"),
      420,
    );
    await runFixture("mkdir", parent, []);
    const path = win32.join(parent, "file.txt");
    const relative = win32.relative(
      getWindowsRemoteContext()!.runtime.debugRuntimeDirectory,
      path,
    );
    expect(relative.length).toBeGreaterThan(260);
    const result = await evaluate(`
      const relative = ${JSON.stringify(relative.replaceAll("\\", "/"))};
      await fs.writeTextFile(relative, "relative contents", "utf8");
      return {
        contents: await fs.readTextFile(${JSON.stringify(path)}, "utf8"),
        canonical: await fs.realpath(relative),
        size: (await fs.stat(relative)).size,
      };
    `);
    expect(result).toEqual({
      contents: "relative contents",
      canonical: path,
      size: 17,
    });
  });

  // Directory.CreateDirectory, File.Create.ValidCreation_LongPathExtendedSyntax
  // and CreateInParentDirectory: normalization must not require existence.
  it("creates BCL boundary directories and normalizes missing parents", async () => {
    const paths = [247, 248, 249, 250, 260, 261, 262, 300, 500, 4200].map(
      (length) =>
        directoryOfLength(
          join(directory, "data", "mkdir", String(length)),
          length,
        ),
    );
    const result = await evaluate(`
      const results = [];
      for (const path of ${JSON.stringify(paths)}) {
        for (const prefix of ["", "\\\\\\\\?\\\\"]) {
          const input = prefix + path;
          await fs.mkdir(input + "\\\\", { recursive: true });
          await fs.mkdir(input, { recursive: true });
          const file = input + "\\\\file.txt";
          await fs.writeTextFile(file, "created", "utf8");
          let blocked = false;
          try { await fs.mkdir(file + "\\\\child", { recursive: true }); } catch { blocked = true; }
          results.push([(await fs.stat(input)).isDirectory(), blocked, await fs.readTextFile(file, "utf8")]);
          await fs.rm(input, { recursive: true });
        }
      }
      const root = ${JSON.stringify(join(directory, "data", "normalize"))};
      await fs.mkdir(root, { recursive: true });
      await fs.writeTextFile(root + "/missing/../file.txt", "normalized", "utf8");
      return { results, normalized: await fs.readTextFile(root + "/file.txt", "utf8") };
    `);
    expect(result).toEqual({
      results: paths.flatMap(() => [
        [true, true, "created"],
        [true, true, "created"],
      ]),
      normalized: "normalized",
    });
  });

  // Directory.GetFiles.EnumerateFilesOverLegacyMaxPath / ...DirectoryOverLegacyMaxPath.
  it.each([250, 270])(
    "enumerates BCL child names under a %i-unit parent",
    async (length) => {
      const parent = directoryOfLength(
        join(directory, "data", "enumerate", String(length)),
        length,
      );
      const names = Array.from({ length: 6 }, (_, i) =>
        String(i).repeat(i + 7),
      );
      await runFixture("mkdir", parent, []);
      for (const [index, name] of names.entries())
        await runFixture("write", win32.join(parent, name), [String(index)]);
      const result = await evaluate(`
      const parent = ${JSON.stringify(parent)};
      const names = (await fs.readdir(parent)).sort();
      const contents = [];
      for (const name of names) contents.push(await fs.readTextFile(parent + "\\\\" + name, "utf8"));
      await fs.rm(parent, { recursive: true });
      return { names, contents, removed: !await fs.exists(parent) };
    `);
      expect(result).toEqual({
        names,
        contents: ["0", "1", "2", "3", "4", "5"],
        removed: true,
      });
    },
  );

  // File.Move.OverMaxPathWorks_Windows and File.Copy.ShortenLongPath.
  it("moves BCL long destinations and rejects a copy onto a normalized alias", async () => {
    const parents = [260, 261, 262].map((length) =>
      directoryOfLength(
        join(directory, "data", "move", String(length)),
        length - 9,
      ),
    );
    for (const parent of parents) await runFixture("mkdir", parent, []);
    const path = join(directory, "data", "original");
    const result = await evaluate(`
      const path = ${JSON.stringify(path)};
      await fs.writeTextFile(path, "preserved", "utf8");
      const moves = [];
      for (const parent of ${JSON.stringify(parents)}) {
        const destination = parent + "\\\\file.txt";
        await fs.rename(path, destination);
        moves.push([await fs.exists(path), await fs.readTextFile(destination, "utf8")]);
        await fs.rename(destination, path);
        moves.push([await fs.exists(destination), await fs.readTextFile(path, "utf8")]);
      }
      let rejected = false;
      const alias = ${JSON.stringify(join(directory, "data"))} + "/.".repeat(150) + "/original";
      try { await fs.copyFile(path, alias); } catch { rejected = true; }
      return { moves, rejected, contents: await fs.readTextFile(path, "utf8") };
    `);
    expect(result).toEqual({
      moves: Array.from({ length: 6 }, () => [false, "preserved"]),
      rejected: true,
      contents: "preserved",
    });
  });

  // EnumerableTests.EnumerateDirectories_TrailingDot, also applied to files.
  // An explicit DOS-device spelling is a literal filename in this namespace.
  it("preserves BCL exact names through realpath, enumeration and deletion", async () => {
    const parent = directoryOfLength(join(directory, "data", "exact"), 300);
    const entries = [
      ["item", false],
      ["item.", false],
      ["item ", false],
      ["NUL", false],
      ["dir", true],
      ["dir.", true],
      ["dir ", true],
    ] as const;
    await runFixture("mkdir", parent, []);
    for (const [name, isDirectory] of entries) {
      const path = win32.join(parent, name);
      if (isDirectory) await runFixture("mkdir", path, []);
      await runFixture(
        "write",
        isDirectory ? win32.join(path, "file.txt") : path,
        [name],
      );
    }
    const result = await evaluate(`
      const parent = ${JSON.stringify(parent)};
      const prefix = "\\\\\\\\?\\\\";
      const names = (await fs.readdir(parent)).sort();
      const results = [];
      for (const [name, directory] of ${JSON.stringify(entries)}) {
        const input = prefix + parent + "\\\\" + name;
        const canonical = await fs.realpath(input);
        const file = canonical + (directory ? "\\\\file.txt" : "");
        const contents = await fs.readTextFile(file, "utf8");
        const copy = prefix + parent + "\\\\copied";
        await fs.copyFile(file, copy);
        results.push([canonical.startsWith(prefix), contents, await fs.readTextFile(copy, "utf8")]);
        await fs.unlink(copy);
      }
      await fs.rm(parent, { recursive: true });
      return { names, results, removed: !await fs.exists(parent) };
    `);
    expect(result).toEqual({
      names: entries.map(([name]) => name).sort(),
      results: entries.map(([name]) => [true, name, name]),
      removed: true,
    });
  });

  // Directory_Delete.RecursiveDeletingDoesntFollowLinks. A junction needs no
  // symlink privilege and lets the ordinary-user baseline verify this behavior.
  it.each([false, true])(
    "preserves an outside junction target (exact name=%s)",
    async (exact) => {
      const root = join(directory, "data", "junction-" + String(exact));
      const parent = directoryOfLength(win32.join(root, "tree"), 500);
      const outside = win32.join(root, exact ? "outside." : "outside");
      const link = win32.join(parent, "junction");
      await runFixture("mkdir", parent, []);
      await runFixture("mkdir", outside, []);
      await runFixture("write", win32.join(outside, "keep.txt"), ["preserved"]);
      if (exact) {
        await runFixture("mkdir", win32.join(root, "outside"), []);
        await runFixture("write", win32.join(root, "outside", "keep.txt"), [
          "different object",
        ]);
      }
      await runFixture("junction", link, ["\\\\?\\" + outside]);
      const result = await evaluate(`
      const link = ${JSON.stringify(link)};
      const parent = ${JSON.stringify(parent)};
      const outside = ${JSON.stringify("\\\\?\\" + outside)};
      const canonical = await fs.realpath(link);
      const target = await fs.readlink(link);
      const before = [
        (await fs.lstat(link)).isSymbolicLink(),
        (await fs.stat(link)).isDirectory(),
        await fs.readTextFile(canonical + "\\\\keep.txt", "utf8"),
        await fs.readTextFile(target + "\\\\keep.txt", "utf8"),
      ];
      await fs.rm(parent, { recursive: true });
      return { before, extended: canonical.startsWith("\\\\\\\\?\\\\"), removed: !await fs.exists(parent),
        outside: await fs.readTextFile(outside + "\\\\keep.txt", "utf8") };
    `);
      expect(result).toEqual({
        before: [true, true, "preserved", "preserved"],
        extended: exact,
        removed: true,
        outside: "preserved",
      });
    },
  );

  it("keeps relative symlinks relative after moving a long parent", async (testContext) => {
    const root = join(directory, "data", "symlink");
    const parent = directoryOfLength(win32.join(root, "long"), 300);
    await runFixture("mkdir", win32.join(parent, "nested"), []);
    const result = await evaluate<{
      supported: boolean;
      error?: string;
      relative?: string;
      contents?: string[];
    }>(`
      const root = ${JSON.stringify(root)};
      const parent = ${JSON.stringify(parent)};
      await fs.writeTextFile(root + "\\\\target.txt", "short target", "utf8");
      try { await fs.symlink("target.txt", root + "\\\\probe", "file"); }
      catch (error) { return { supported: false, error: String(error) }; }
      await fs.unlink(root + "\\\\probe");
      await fs.writeTextFile(parent + "\\\\target.txt", "long target", "utf8");
      await fs.symlink("..\\\\target.txt", parent + "\\\\nested\\\\link", "file");
      const moved = parent + "-moved";
      await fs.rename(parent, moved);
      const relative = await fs.readlink(moved + "\\\\nested\\\\link");
      await fs.symlink(moved + "\\\\target.txt", moved + "\\\\absolute", "file");
      return { supported: true, relative: relative.replaceAll("\\\\", "/"), contents: [
        await fs.readTextFile(moved + "\\\\nested\\\\link", "utf8"),
        await fs.readTextFile(await fs.readlink(moved + "\\\\absolute"), "utf8"),
      ] };
    `);
    if (!result.supported) {
      expect(result.error).toContain("Windows error 1314");
      testContext.skip(
        "Ordinary user cannot create even a short symlink; junction I/O is tested separately",
      );
    }
    expect(result).toEqual({
      supported: true,
      relative: "../target.txt",
      contents: ["long target", "long target"],
    });
  });

  it.each([300, 4200])(
    "watches a %i-unit directory after readiness",
    async (length) => {
      const parent = directoryOfLength(
        join(directory, "data", "watch", String(length)),
        length,
      );
      await runFixture("mkdir", parent, []);
      const result = await evaluate(`
      const parent = ${JSON.stringify(parent)};
      let resolveEvent, rejectEvent;
      const observed = new Promise((resolve, reject) => { resolveEvent = resolve; rejectEvent = reject; });
      const watcher = await fs.watch(parent, event => {
        if (event.eventType === "error") rejectEvent(new Error(event.message));
        else if (event.filename === "観測😀.txt") resolveEvent(event);
      });
      const timeout = setTimeout(() => rejectEvent(new Error("Expected filesystem event did not arrive")), 30000);
      try {
        await fs.writeTextFile(parent + "\\\\観測😀.txt", "observed", "utf8");
        const event = await observed;
        return { filename: event.filename, eventType: event.eventType,
          contents: await fs.readTextFile(parent + "\\\\" + event.filename, "utf8") };
      } finally {
        clearTimeout(timeout);
        await watcher.close();
      }
    `);
      expect(result).toEqual({
        filename: "観測😀.txt",
        eventType: "rename",
        contents: "observed",
      });
    },
  );

  it("keeps the error code and Unicode filename in a long path diagnostic", async () => {
    const parent = directoryOfLength(join(directory, "data", "errors"), 300);
    await runFixture("mkdir", parent, []);
    const missing = win32.join(parent, "missing-不存在😀");
    const error = await evaluate<string>(`
      try { await fs.readdir(${JSON.stringify(missing)}); return ""; }
      catch (error) { return String(error); }
    `);
    expect(error).toContain("readdir failed");
    expect(error).toContain(join(directory, "data"));
    expect(error).toContain("missing-不存在😀");
    expect(error).toContain("...");
    expect(error).toContain("Windows filesystem error 3");
    expect(error).not.toContain("\uFFFD");
    expect(
      Buffer.byteLength(error.slice("Error: ".length), "utf8"),
    ).toBeLessThanOrEqual(255);
    expect(error).not.toContain("\\\\?\\");
  });

  // IOInputs.GetPathsLongerThanMaxLongPath and component boundaries. Generate
  // oversized inputs in the browser to avoid CreateProcess argument limits.
  it("rejects BCL oversized paths without truncating to an existing name", async () => {
    const root = join(directory, "data", "limits");
    await runFixture("mkdir", root, []);
    const result = await evaluate(`
      const root = ${JSON.stringify(root)};
      const valid = root + "\\\\" + "a".repeat(255);
      await fs.writeTextFile(valid, "preserved", "utf8");
      const rejected = [];
      const invalid = [valid + "a"];
      for (const length of [32767, 32768]) {
        const tail = ("\\\\" + "b".repeat(70)).repeat(500);
        for (const prefix of ["", "\\\\\\\\?\\\\"])
          invalid.push((prefix + root + tail).slice(0, length));
      }
      for (const path of invalid) {
        let write = false, mkdir = false;
        try { await fs.writeTextFile(path, "wrong", "utf8"); } catch { write = true; }
        try { await fs.mkdir(path, { recursive: true }); } catch { mkdir = true; }
        rejected.push([write, mkdir]);
      }
      return { rejected, contents: await fs.readTextFile(valid, "utf8") };
    `);
    expect(result).toEqual({
      rejected: Array.from({ length: 5 }, () => [true, true]),
      contents: "preserved",
    });
  });
});

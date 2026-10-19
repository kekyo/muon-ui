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
        entries: [
          ["file.bin", true],
          ["move.bin", true],
          ["text.txt", true],
        ],
        missing: [false, false, false, false],
      });
    },
  );

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

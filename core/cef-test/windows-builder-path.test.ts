// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFile } from "node:child_process";
import { readFile } from "node:fs/promises";
import { win32 } from "node:path";
import { promisify } from "node:util";
import { connectRemoteAgent, type RemoteAgent } from "agent-rover";
import { afterAll, beforeAll, describe, expect, it } from "vitest";

import { resolveWindowsE2eEnvironment } from "../../scripts/windows-e2e-environment.js";

const execFileAsync = promisify(execFile);
const targets = ["windows-i686", "windows-amd64"] as const;
const environment = resolveWindowsE2eEnvironment(process.env);
let agent: RemoteAgent;
let directory: string;

const run = async (path: string, args: string[]) => {
  const process = await agent.processes.launchManaged({
    path,
    arguments: args,
    workingDirectory: directory,
    createNoWindow: true,
    captureStdout: true,
    captureStderr: true,
  });
  try {
    const status = await process.waitForExit({ timeoutMs: 30000 });
    return {
      code: status.root.exitCode,
      stdout: await process.stdoutText(),
      stderr: await process.stderrText(),
    };
  } finally {
    await process.releaseAsync();
  }
};

const directoryOfLength = (root: string, length: number) => {
  let path = root;
  while (length - path.length > 100) path = win32.join(path, "d".repeat(70));
  path = win32.join(path, "f".repeat(length - path.length - 1));
  expect(path.length).toBe(length);
  return path;
};

beforeAll(async () => {
  for (const target of targets) {
    await execFileAsync("make", [
      "windows-long-path-fixture",
      `WINDOWS_TARGET=${target}`,
    ]);
    await execFileAsync("npm", [
      "run",
      "build:target",
      "--workspace",
      "muon-builder",
      "--",
      "test",
      "Release",
      target,
    ]);
  }
  agent = await connectRemoteAgent({
    section: environment.section,
    timeoutMs: 30000,
  });
  await agent.files.mkdir(environment.workDir, { recursive: true });
  directory = await agent.files.mkdtemp(
    win32.join(environment.workDir, "builder-path-"),
  );
  const policy = await run("reg.exe", [
    "query",
    String.raw`HKLM\SYSTEM\CurrentControlSet\Control\FileSystem`,
    "/v",
    "LongPathsEnabled",
  ]);
  expect(policy.code, policy.stderr).toBe(0);
  expect(policy.stdout).toMatch(/LongPathsEnabled\s+REG_DWORD\s+0x0\b/i);
  const privilege = await run("powershell.exe", [
    "-NoProfile",
    "-NonInteractive",
    "-Command",
    "([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)",
  ]);
  expect(privilege.code, privilege.stderr).toBe(0);
  expect(privilege.stdout.trim()).toBe("False");
  for (const target of targets) {
    await agent.files.writeFile(
      win32.join(directory, `${target}-fixture.exe`),
      await readFile(`.run/windows-long-path/${target}/fixture.exe`),
    );
    await agent.files.writeFile(
      win32.join(directory, `${target}-builder-test.exe`),
      await readFile(
        `../../builder/.run/test-${target}-release/windows-filesystem-test.exe`,
      ),
    );
  }
});

afterAll(async () => {
  try {
    if (directory !== undefined)
      await agent.files.remove(directory, { recursive: true });
  } finally {
    agent?.release();
  }
});

describe.each(targets)(
  "builder Windows filesystem (%s, policy=0, ordinary user)",
  (target) => {
    const fixture = async (mode: string, path: string, args: string[] = []) => {
      const result = await run(win32.join(directory, `${target}-fixture.exe`), [
        mode,
        "\\\\?\\" + path,
        ...args,
      ]);
      expect(result.code, result.stderr).toBe(0);
    };
    const builder = async (mode: string, path: string, args: string[] = []) => {
      const result = await run(
        win32.join(directory, `${target}-builder-test.exe`),
        [mode, path, ...args],
      );
      expect(result.code, result.stderr).toBe(0);
    };

    // .NET IOInputs.GetPathsLongerThanMaxDirectory / MaxPath.
    // https://github.com/dotnet/runtime/blob/v10.0.0/src/libraries/System.Runtime/tests/System.IO.FileSystem.Tests/PortedCommon/IOInputs.cs
    it.each([247, 248, 249, 250, 260, 261, 262, 300, 4200])(
      "creates a %i-unit directory",
      async (length) => {
        const root = win32.join(directory, `${target}-mkdir-${length}`);
        const path = directoryOfLength(win32.join(root, "日本語😀"), length);
        try {
          await builder("mkdir", path);
          // The independent driver must find and remove the actual Unicode path.
          expect(await agent.files.stat("\\\\?\\" + path)).toBeDefined();
        } finally {
          await fixture("remove", root);
        }
      },
    );

    it.each([
      [140, "ascii", false],
      [140, "日本語😀", false],
      [300, "日本語😀", false],
      [4200, "日本語😀", false],
      [300, "日本語😀", true],
    ] as const)(
      "copies, hashes, replaces and removes files under %i units (%s, extended=%s)",
      async (length, name, extended) => {
        const root = win32.join(directory, `${target}-files-${length}`);
        const path = directoryOfLength(win32.join(root, name), length);
        try {
          await fixture("mkdir", path);
          await builder("files", (extended ? "\\\\?\\" : "") + path, [
            "\\\\?\\" + path,
          ]);
        } finally {
          await fixture("remove", root);
        }
      },
    );

    it.each([300, 4200])(
      "copies and deletes a Unicode tree under %i units",
      async (length) => {
        const root = win32.join(directory, `${target}-tree-${length}`);
        const parent = directoryOfLength(win32.join(root, "日本語😀"), length);
        const source = win32.join(parent, "input");
        const destination = win32.join(parent, "output");
        try {
          await fixture("mkdir", win32.join(source, "子階層"));
          await fixture("write", win32.join(source, "日本語.txt"), ["first"]);
          await fixture("write", win32.join(source, "子階層", "emoji-😀.txt"), [
            "second",
          ]);
          await builder("tree", source, [destination]);
        } finally {
          await fixture("remove", root);
        }
      },
    );

    // .NET Directory_GetFiles.EnumerateFilesOverLegacyMaxPath and
    // EnumerateFilesDirectoryOverLegacyMaxPath use these lengths and six names.
    // https://github.com/dotnet/runtime/blob/v10.0.0/src/libraries/System.Runtime/tests/System.IO.FileSystem.Tests/Directory/GetFiles.cs
    it.each([250, 270])(
      "enumerates children crossing MAX_PATH under a %i-unit parent",
      async (length) => {
        const root = win32.join(directory, `${target}-enumerate-${length}`);
        const source = directoryOfLength(win32.join(root, "日本語😀"), length);
        const destination = win32.join(root, "copy");
        try {
          await fixture("mkdir", source);
          for (let i = 0; i < 6; ++i)
            await fixture(
              "write",
              win32.join(source, String(i).repeat(i + 7)),
              [String(i)],
            );
          await builder("enumerate", source, [destination]);
        } finally {
          await fixture("remove", root);
        }
      },
    );

    it("preserves distinct trailing-dot and trailing-space names while copying and deleting", async () => {
      const root = win32.join(directory, `${target}-exact`);
      const parent = directoryOfLength(root, 300);
      const source = win32.join(parent, "input");
      const destination = win32.join(parent, "output");
      try {
        await fixture("mkdir", source);
        for (const [i, name] of ["item", "item.", "item "].entries())
          await fixture("write", win32.join(source, name), [String(i)]);
        await builder("exact", source, [destination]);
      } finally {
        await fixture("remove", root);
      }
    });

    // .NET File_Move.OverMaxPathWorks_Windows moves to 260/261/262 and back.
    // https://github.com/dotnet/runtime/blob/v10.0.0/src/libraries/System.Runtime/tests/System.IO.FileSystem.Tests/File/Move.cs
    it.each([260, 261, 262])(
      "moves a file to a %i-unit destination and restores it",
      async (length) => {
        const root = win32.join(directory, `${target}-move-${length}`);
        const source = win32.join(root, "source.txt");
        const destination = directoryOfLength(
          win32.join(root, "日本語😀"),
          length,
        );
        try {
          await fixture("mkdir", win32.dirname(destination));
          await fixture("write", source, ["preserved"]);
          await builder("move", source, [destination]);
        } finally {
          await fixture("remove", root);
        }
      },
    );

    it("rejects copying a long normalized alias over its own source", async () => {
      const root = win32.join(directory, `${target}-copy-self`);
      try {
        await builder("copy-self", root);
      } finally {
        await fixture("remove", root);
      }
    });

    it("rejects paths over 32766 units and components over 255 without truncating existing names", async () => {
      const root = win32.join(directory, `${target}-limits`);
      try {
        await builder("limits", root);
      } finally {
        await fixture("remove", root);
      }
    });

    // .NET Directory_Delete.RecursiveDeletingDoesntFollowLinks. A local
    // directory junction exercises the reparse guard without symlink privilege.
    it("deletes a long tree without following a junction outside that tree", async () => {
      const root = win32.join(directory, `${target}-junction`);
      const parent = directoryOfLength(win32.join(root, "input"), 300);
      const targetPath = win32.join(root, "outside");
      const sentinel = win32.join(targetPath, "keep.txt");
      try {
        await fixture("mkdir", parent);
        await fixture("mkdir", targetPath);
        await fixture("write", sentinel, ["untouched"]);
        const linked = await run("cmd.exe", [
          "/d",
          "/c",
          "mklink",
          "/J",
          "\\\\?\\" + win32.join(parent, "junction"),
          targetPath,
        ]);
        expect(linked.code, linked.stderr + linked.stdout).toBe(0);
        await builder("contents", win32.join(parent, "junction", "keep.txt"), [
          "untouched",
        ]);
        await builder("remove", parent);
        await builder("contents", sentinel, ["untouched"]);
      } finally {
        await fixture("remove", root);
      }
    });
  },
);

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

import {
  connectWindowsE2eAgent,
  resolveWindowsE2eEnvironment,
} from "../../scripts/windows-e2e-environment.js";

const execFileAsync = promisify(execFile);
const environment = resolveWindowsE2eEnvironment(process.env);
const connection = await connectWindowsE2eAgent(environment);
const describeWindowsE2e =
  connection.status === "connected" ? describe : describe.skip;
if (connection.status === "connected") {
  // Release the probe before local compilation and reconnect in beforeAll.
  connection.agent.release();
} else {
  process.stderr.write(`Skipping Windows path tests: ${connection.reason}\n`);
}
const targets = ["windows-i686", "windows-amd64"] as const;
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

const pathOfLength = (root: string, length: number) => {
  let parent = root;
  while (length - parent.length > 100) {
    parent = win32.join(parent, "d".repeat(70));
  }
  const path = win32.join(
    parent,
    "f".repeat(length - parent.length - 5) + ".txt",
  );
  expect(path.length).toBe(length);
  return path;
};

beforeAll(async () => {
  for (const target of targets) {
    await execFileAsync("make", [
      "windows-path-test",
      "windows-path-autostart",
      `WINDOWS_TARGET=${target}`,
    ]);
  }
  agent = await connectRemoteAgent({
    section: environment.section,
    timeoutMs: 30000,
  });
  await agent.files.mkdir(environment.workDir, { recursive: true });
  directory = await agent.files.mkdtemp(
    win32.join(environment.workDir, "path-test-"),
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
    for (const exe of ["fixture", "path-test", "path-autostart"]) {
      await agent.files.writeFile(
        win32.join(directory, `${target}-${exe}.exe`),
        await readFile(`.run/windows-long-path/${target}/${exe}.exe`),
      );
    }
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

describeWindowsE2e.each(targets)(
  "native Windows paths (%s, policy=0, ordinary user)",
  (target) => {
    it.each([140, 259, 260, 261, 1100])(
      "keeps a %i-unit quoted Run command within the Windows autostart limit",
      async (length) => {
        const result = await run(
          win32.join(directory, `${target}-path-autostart.exe`),
          [String(length)],
        );
        expect(result.code, result.stderr).toBe(0);
      },
    );

    const exercise = async (
      mode: string,
      input: (path: string) => string,
      length: number,
    ) => {
      const fixture = win32.join(directory, `${target}-fixture.exe`);
      const root = win32.join(
        directory,
        `${target}-${mode}-${length}`,
        "日本語😀",
      );
      const path = pathOfLength(root, length);
      try {
        const setup = await run(fixture, [
          "mkdir",
          "\\\\?\\" + win32.dirname(path),
        ]);
        expect(setup.code, setup.stderr).toBe(0);
        const result = await run(
          win32.join(directory, `${target}-path-test.exe`),
          [mode, input(path), "\\\\?\\" + path],
        );
        expect(result.code, result.stderr).toBe(0);
      } finally {
        const cleanup = await run(fixture, [
          "remove",
          "\\\\?\\" + win32.dirname(root),
        ]);
        expect(cleanup.code, cleanup.stderr).toBe(0);
      }
    };

    // .NET IOInputs.GetPathsLongerThanMaxPath also covers 262 UTF-16 units.
    // https://github.com/dotnet/runtime/blob/v10.0.0/src/libraries/System.Runtime/tests/System.IO.FileSystem.Tests/PortedCommon/IOInputs.cs
    it.each([259, 260, 261, 262, 300, 1100, 4200])(
      "reads and writes a %i-unit ordinary path",
      async (length) => {
        await exercise("file", (path) => path, length);
      },
    );

    it.each([247, 248, 249, 250, 260, 261, 262, 4200])(
      "creates a %i-unit directory",
      async (length) => {
        await exercise("directory", (path) => path, length);
      },
    );

    it("resolves a long relative path with dot segments before opening a new file", async () => {
      await exercise(
        "file",
        (path) => `.\\absent\\..\\${win32.relative(directory, path)}`,
        1100,
      );
    });

    // File.Copy.ShortenLongPath and File.Create.CreateInParentDirectory test
    // lexical normalization before touching the filesystem.
    it("opens a short target through an input longer than MAX_PATH", async () => {
      await exercise(
        "file",
        (path) =>
          `${win32.dirname(path)}${"\\.".repeat(150)}\\${win32.basename(path)}`,
        180,
      );
    });

    it("normalizes dot segments while the resulting path remains long", async () => {
      await exercise("file", (path) => path.replaceAll("\\", "\\.\\"), 1100);
    });

    it("uses a short relative input whose absolute target exceeds MAX_PATH", async () => {
      // The native process keeps a short cwd; a root-relative input itself can
      // be below MAX_PATH while adding the drive reaches the boundary.
      await exercise("file", (path) => path.slice(2), 260);
    });

    it("creates an explicitly extended directory with a trailing separator", async () => {
      await exercise("directory", (path) => "\\\\?\\" + path + "\\", 500);
    });

    it("resolves a drive-relative path using the current directory", async () => {
      await exercise(
        "file",
        (path) => path.slice(0, 2) + win32.relative(directory, path),
        300,
      );
    });

    it("resolves a root-relative path", async () => {
      await exercise("file", (path) => path.slice(2), 300);
    });

    it("normalizes ordinary forward slashes", async () => {
      await exercise("file", (path) => path.replaceAll("\\", "/"), 4200);
    });

    it("accepts an explicitly extended path", async () => {
      await exercise("file", (path) => "\\\\?\\" + path, 4200);
    });

    it("opens the same Unicode file through the UTF-8 entry point", async () => {
      await exercise("utf8", (path) => path, 4200);
    });

    it("preserves namespace semantics, validates input and classifies UNC roots", async () => {
      const result = await run(
        win32.join(directory, `${target}-path-test.exe`),
        ["contracts"],
      );
      expect(result.code, result.stderr).toBe(0);
    });
  },
);

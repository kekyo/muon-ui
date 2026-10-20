// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFile } from "node:child_process";
import { createHash } from "node:crypto";
import { mkdir, mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { join, win32 } from "node:path";
import { tmpdir } from "node:os";
import { promisify } from "node:util";
import { fileURLToPath } from "node:url";
import {
  connectRemoteAgent,
  type RemoteAgent,
  type RemoteManagedProcess,
} from "agent-rover";
import { afterAll, beforeAll, describe, expect, it } from "vitest";

import {
  connectWindowsE2eAgent,
  resolveWindowsE2eEnvironment,
} from "../../scripts/windows-e2e-environment.js";

const execFileAsync = promisify(execFile);
const targets = ["windows-i686", "windows-amd64"] as const;
const environment = resolveWindowsE2eEnvironment(process.env);
const connection = await connectWindowsE2eAgent(environment);
const describeWindowsE2e =
  connection.status === "connected" ? describe : describe.skip;
if (connection.status === "connected") {
  // Release the probe before local compilation and reconnect in beforeAll.
  connection.agent.release();
} else {
  process.stderr.write(
    `Skipping Windows builder path tests: ${connection.reason}\n`,
  );
}
let agent: RemoteAgent;
let directory: string;
let localDirectory: string;

const run = async (
  path: string,
  args: string[],
  processEnvironment: Readonly<Record<string, string>> | undefined,
) => {
  const process = await agent.processes.launchManaged({
    path,
    arguments: args,
    ...(processEnvironment === undefined
      ? {}
      : { environment: processEnvironment }),
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
  localDirectory = await mkdtemp(join(tmpdir(), "muon-builder-path-"));
  const cefRoot = join(localDirectory, "cef-source");
  await mkdir(join(cefRoot, "Release"), { recursive: true });
  await mkdir(join(cefRoot, "Resources", "locales"), { recursive: true });
  await writeFile(join(cefRoot, "Release", "libcef.dll"), "CEF payload");
  await writeFile(join(cefRoot, "Resources", "icudtl.dat"), "CEF data");
  await writeFile(
    join(cefRoot, "Resources", "locales", "en-US.pak"),
    "CEF locale",
  );
  await writeFile(join(cefRoot, "LICENSE"), "CEF license");
  await execFileAsync("tar", [
    "-cjf",
    join(localDirectory, "cef.tar.bz2"),
    "-C",
    localDirectory,
    "cef-source",
  ]);
  const cefArchive = await readFile(join(localDirectory, "cef.tar.bz2"));
  await execFileAsync(process.execPath, [
    "../../builder/scripts/create-windows-resource-fixture.mjs",
    join(localDirectory, "unused.exe"),
    join(localDirectory, "app.ico"),
  ]);
  for (const target of targets) {
    await execFileAsync("make", [
      "windows-path-test",
      "windows-path-process",
      "windows-path-progress",
      `WINDOWS_TARGET=${target}`,
    ]);
    const nodeRoot = `node-v24.4.0-win-${target === "windows-amd64" ? "x64" : "x86"}`;
    await mkdir(join(localDirectory, nodeRoot));
    await writeFile(join(localDirectory, nodeRoot, "node.exe"), "Node payload");
    await execFileAsync(
      "zip",
      ["-q", "-r", join(localDirectory, `${target}-bad-node.zip`), nodeRoot],
      { cwd: localDirectory },
    );
    await writeFile(join(localDirectory, nodeRoot, "LICENSE"), "Node license");
    await writeFile(
      join(localDirectory, nodeRoot, "README.md"),
      "not installed",
    );
    await execFileAsync(
      "zip",
      ["-q", "-r", join(localDirectory, `${target}-node.zip`), nodeRoot],
      { cwd: localDirectory },
    );
    await execFileAsync(
      "npm",
      [
        "run",
        "build:target",
        "--workspace",
        "muon-builder",
        "--",
        "test",
        "Release",
        target,
      ],
      {
        env: {
          ...process.env,
          CPPFLAGS: `${process.env.CPPFLAGS ?? ""} -DMUON_TEST_CEF_SHA1=\\\"${createHash("sha1").update(cefArchive).digest("hex")}\\\" -DMUON_TEST_CEF_SIZE=${cefArchive.length}`,
          MUON_RUNTIME_INFO_HEADER: fileURLToPath(
            new URL(
              "../../builder/test/windows_runtime_info.h",
              import.meta.url,
            ),
          ),
        },
      },
    );
  }
  agent = await connectRemoteAgent({
    section: environment.section,
    timeoutMs: 30000,
  });
  await agent.files.mkdir(environment.workDir, { recursive: true });
  directory = await agent.files.mkdtemp(
    win32.join(environment.workDir, "builder-path-"),
  );
  const policy = await run(
    "reg.exe",
    [
      "query",
      String.raw`HKLM\SYSTEM\CurrentControlSet\Control\FileSystem`,
      "/v",
      "LongPathsEnabled",
    ],
    undefined,
  );
  expect(policy.code, policy.stderr).toBe(0);
  expect(policy.stdout).toMatch(/LongPathsEnabled\s+REG_DWORD\s+0x0\b/i);
  const privilege = await run(
    "powershell.exe",
    [
      "-NoProfile",
      "-NonInteractive",
      "-Command",
      "([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)",
    ],
    undefined,
  );
  expect(privilege.code, privilege.stderr).toBe(0);
  expect(privilege.stdout.trim()).toBe("False");
  for (const file of ["cef.tar.bz2", "app.ico"]) {
    await agent.files.writeFile(
      win32.join(directory, file),
      await readFile(join(localDirectory, file)),
    );
  }
  for (const target of targets) {
    for (const file of [`${target}-node.zip`, `${target}-bad-node.zip`]) {
      await agent.files.writeFile(
        win32.join(directory, file),
        await readFile(join(localDirectory, file)),
      );
    }
    await agent.files.writeFile(
      win32.join(directory, `${target}-path-test.exe`),
      await readFile(`.run/windows-long-path/${target}/path-test.exe`),
    );
    await agent.files.writeFile(
      win32.join(directory, `${target}-download-fixture.exe`),
      await readFile(`.run/windows-long-path/${target}/download-fixture.exe`),
    );
    await agent.files.writeFile(
      win32.join(directory, `${target}-fixture.exe`),
      await readFile(`.run/windows-long-path/${target}/fixture.exe`),
    );
    await agent.files.writeFile(
      win32.join(directory, `${target}-process.exe`),
      await readFile(`.run/windows-long-path/${target}/path-process.exe`),
    );
    await agent.files.writeFile(
      win32.join(directory, `${target}-progress.exe`),
      await readFile(`.run/windows-long-path/${target}/path-progress.exe`),
    );
    await agent.files.writeFile(
      win32.join(directory, `${target}-builder-test.exe`),
      await readFile(
        `../../builder/.run/test-${target}-release/windows-filesystem-test.exe`,
      ),
    );
    for (const executable of ["muon-builder", "muon-launcher"]) {
      await agent.files.writeFile(
        win32.join(directory, `${target}-${executable}.exe`),
        await readFile(
          `../../builder/.run/test-${target}-release/${executable}.exe`,
        ),
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
    if (localDirectory !== undefined)
      await rm(localDirectory, { recursive: true, force: true });
  }
});

describeWindowsE2e.each(targets)(
  "builder Windows filesystem (%s, policy=0, ordinary user)",
  (target) => {
    const fixture = async (mode: string, path: string, args: string[] = []) => {
      const result = await run(
        win32.join(directory, `${target}-fixture.exe`),
        [mode, "\\\\?\\" + path, ...args],
        undefined,
      );
      expect(result.code, result.stderr).toBe(0);
    };
    const builder = async (mode: string, path: string, args: string[] = []) => {
      const result = await run(
        win32.join(directory, `${target}-builder-test.exe`),
        [mode, path, ...args],
        undefined,
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
        const linked = await run(
          "cmd.exe",
          [
            "/d",
            "/c",
            "mklink",
            "/J",
            "\\\\?\\" + win32.join(parent, "junction"),
            targetPath,
          ],
          undefined,
        );
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

    it.each([
      [140, false],
      [300, false],
      [4200, false],
      [300, true],
    ] as const)(
      "prepares and reuses a runtime from Unicode CLI paths at %i units (extended=%s)",
      async (length, extended) => {
        const root = win32.join(directory, `${target}-cli-${length}`);
        const parent = directoryOfLength(win32.join(root, "日本語😀"), length);
        const source = win32.join(parent, "muon");
        const cef = win32.join(parent, "cef");
        const output = win32.join(parent, "output");
        const cache = win32.join(parent, "cache");
        const argumentPath = (path: string) =>
          (extended ? "\\\\?\\" : "") + path;
        try {
          await fixture("mkdir", source);
          await fixture("write", win32.join(source, "muon-core.exe"), [
            "local core payload",
          ]);
          await fixture("mkdir", win32.join(cef, "Release"));
          await fixture("write", win32.join(cef, "Release", "libcef.dll"), [
            "local CEF payload",
          ]);
          for (const cacheHit of [false, true]) {
            const result = await run(
              win32.join(directory, `${target}-muon-builder.exe`),
              [
                "runtime",
                "--muon-path",
                argumentPath(source),
                "--cef-path",
                argumentPath(cef),
                "--stage-dir",
                argumentPath(output),
                "--cache-dir",
                argumentPath(cache),
                "--target",
                target,
                "--json",
              ],
              undefined,
            );
            expect(result.code, result.stderr).toBe(0);
            expect(JSON.parse(result.stdout)).toMatchObject({ cacheHit });
            await builder("contents", win32.join(output, "muon-core.exe"), [
              "local core payload",
            ]);
            await builder("contents", win32.join(output, "libcef.dll"), [
              "local CEF payload",
            ]);
          }
        } finally {
          await fixture("remove", root);
        }
      },
    );

    it.each([
      [140, false],
      [300, false],
      [4200, false],
      [300, true],
    ] as const)(
      "extracts and reuses a CEF archive at %i units (extended=%s)",
      async (length, extended) => {
        const root = win32.join(directory, `${target}-cef-${length}`);
        const parent = directoryOfLength(win32.join(root, "日本語😀"), length);
        const archive = win32.join(parent, "配布😀.tar.bz2");
        const output = win32.join(parent, "output");
        const ioPath = (path: string) => (extended ? "\\\\?\\" : "") + path;
        try {
          await fixture("mkdir", parent);
          await fixture("copy", archive, [
            win32.join(directory, "cef.tar.bz2"),
          ]);
          for (const mode of ["cef", "cef", "cef-force"]) {
            await builder(mode, ioPath(archive), [ioPath(output)]);
            await builder(
              "contents",
              win32.join(output, "Release", "libcef.dll"),
              ["CEF payload"],
            );
            await builder(
              "contents",
              win32.join(output, "Resources", "locales", "en-US.pak"),
              ["CEF locale"],
            );
          }
          const invalid = win32.join(parent, "invalid.tar.bz2");
          await fixture("write", invalid, ["broken archive"]);
          const failed = await run(
            win32.join(directory, `${target}-builder-test.exe`),
            ["cef-force", ioPath(invalid), ioPath(output)],
            undefined,
          );
          expect(failed.code).not.toBe(0);
          await builder(
            "contents",
            win32.join(output, "Release", "libcef.dll"),
            ["CEF payload"],
          );
        } finally {
          await fixture("remove", root);
        }
      },
    );

    it.each([
      [140, false],
      [300, false],
      [4200, false],
      [300, true],
    ] as const)(
      "installs and replaces a Node ZIP at %i units (extended=%s)",
      async (length, extended) => {
        const root = win32.join(directory, `${target}-zip-${length}`);
        const parent = directoryOfLength(win32.join(root, "日本語😀"), length);
        const archive = win32.join(parent, "配布😀.zip");
        const invalid = win32.join(parent, "incomplete.zip");
        const output = win32.join(parent, "output");
        const ioPath = (path: string) => (extended ? "\\\\?\\" : "") + path;
        try {
          await fixture("mkdir", parent);
          await fixture("copy", archive, [
            win32.join(directory, `${target}-node.zip`),
          ]);
          await fixture("copy", invalid, [
            win32.join(directory, `${target}-bad-node.zip`),
          ]);
          for (let round = 0; round < 2; ++round) {
            await builder("node", ioPath(archive), [ioPath(output)]);
            await builder(
              "contents",
              win32.join(output, "runtimes", "node", "bin", "node.exe"),
              ["Node payload"],
            );
            await builder(
              "contents",
              win32.join(output, "runtimes", "node", "LICENSE"),
              ["Node license"],
            );
          }
          const previous = win32.join(
            output,
            "runtimes",
            "node",
            "previous.txt",
          );
          await fixture("write", previous, ["previous installation"]);
          const interrupted = await run(
            win32.join(directory, `${target}-builder-test.exe`),
            ["node-failure", ioPath(archive), ioPath(output)],
            undefined,
          );
          expect(interrupted.code).toBe(1);
          expect(interrupted.stderr).toContain(
            "Injected runtime commit failure",
          );
          await builder("contents", previous, ["previous installation"]);
          const failed = await run(
            win32.join(directory, `${target}-builder-test.exe`),
            ["node", ioPath(invalid), ioPath(output)],
            undefined,
          );
          expect(failed.code).not.toBe(0);
          expect(failed.stderr).toContain("LICENSE");
          await builder(
            "contents",
            win32.join(output, "runtimes", "node", "bin", "node.exe"),
            ["Node payload"],
          );
        } finally {
          await fixture("remove", root);
        }
      },
    );

    it.each(["MUON_CACHE_DIR", "LOCALAPPDATA", "USERPROFILE"])(
      "reads a Unicode long cache path from %s",
      async (name) => {
        const root = win32.join(directory, `${target}-env-${name}`);
        const parent = directoryOfLength(win32.join(root, "日本語😀"), 300);
        const source = win32.join(root, "muon");
        const cef = win32.join(root, "cef");
        const expected =
          name === "MUON_CACHE_DIR"
            ? parent
            : name === "LOCALAPPDATA"
              ? win32.join(parent, "muon")
              : win32.join(parent, ".cache", "muon");
        try {
          await fixture("mkdir", source);
          await fixture("write", win32.join(source, "muon-core.exe"), ["core"]);
          await fixture("mkdir", cef);
          await fixture("write", win32.join(cef, "libcef.dll"), ["cef"]);
          const result = await run(
            win32.join(directory, `${target}-muon-builder.exe`),
            [
              "runtime",
              "--muon-path",
              source,
              "--cef-path",
              cef,
              "--stage-dir",
              win32.join(root, "output"),
              "--target",
              target,
              "--json",
            ],
            {
              MUON_CACHE_DIR: "",
              LOCALAPPDATA: "",
              USERPROFILE: "",
              [name]: parent,
            },
          );
          expect(result.code, result.stderr).toBe(0);
          // This checks the actual selected directory, rather than displayed JSON.
          await builder("exists-directory", expected);
        } finally {
          await fixture("remove", root);
        }
      },
    );

    it.each([140, 300, 1100])(
      "shows the launcher progress window from a %i-unit source",
      async (length) => {
        const root = win32.join(directory, `${target}-progress-${length}`);
        const source = directoryOfLength(win32.join(root, "配布😀"), length);
        const executable = win32.join(source, "progress.exe");
        try {
          await fixture("mkdir", source);
          await fixture("copy", executable, [
            win32.join(directory, `${target}-progress.exe`),
          ]);
          const result = await run(
            win32.join(directory, `${target}-fixture.exe`),
            ["launch-command", "\\\\?\\" + executable, ""],
            undefined,
          );
          expect(result.code, result.stderr).toBe(0);
          expect(result.stdout.trim()).toBe("progress window shown");
        } finally {
          await fixture("remove", root);
        }
      },
    );

    it("rejects a long runtime before creating installation files", async () => {
      const root = win32.join(directory, `${target}-runtime-cwd`);
      const state = directoryOfLength(win32.join(root, "状態😀"), 300);
      const executable = win32.join(root, "app.exe");
      try {
        await fixture("mkdir", root);
        await fixture("copy", executable, [
          win32.join(directory, `${target}-muon-launcher.exe`),
        ]);
        const result = await run(executable, [], {
          LOCALAPPDATA: state,
          MUON_CACHE_DIR: win32.join(root, "cache"),
        });
        expect(result.code).toBe(1);
        expect(result.stderr).toMatch(/runtime.*short.*Windows/);
        await fixture("absent", win32.join(state, "app", "runtime"));
        await fixture("absent", win32.join(root, "cache"));
      } finally {
        await fixture("remove", root);
      }
    });

    it.each([140, 300, 1100])(
      "launches and restarts from a %i-unit source into a short Unicode runtime",
      async (length) => {
        const root = win32.join(directory, `${target}-launch-${length}`);
        const source = directoryOfLength(win32.join(root, "配布😀"), length);
        const state = win32.join(root, "状態😀");
        const cache = win32.join(root, "cache");
        const runtime = win32.join(state, "app", "runtime");
        const executable = win32.join(source, "app.exe");
        const value = "環境😀" + source;
        try {
          await fixture("mkdir", source);
          await fixture("copy", executable, [
            win32.join(directory, `${target}-muon-launcher.exe`),
          ]);
          await fixture("copy", win32.join(source, "muon-core.exe"), [
            win32.join(directory, `${target}-process.exe`),
          ]);
          await fixture("mkdir", win32.join(cache, "artifacts", "cef"));
          await fixture(
            "copy",
            win32.join(cache, "artifacts", "cef", "test-cef.tar.bz2"),
            [win32.join(directory, "cef.tar.bz2")],
          );
          const result = await run(
            win32.join(directory, `${target}-fixture.exe`),
            [
              "launch-command",
              "\\\\?\\" + executable,
              String.raw` "日本語😀" "quote\"value" "tail\\" ""`,
            ],
            {
              LOCALAPPDATA: state,
              MUON_CACHE_DIR: cache,
              MUON_PATH_VALUE: value,
              MUON_PATH_RESTART: win32.join(root, "restarted"),
            },
          );
          expect(result.code, result.stderr).toBe(37);
          const reports = result.stdout
            .trim()
            .split(/\r?\n/)
            .map((line) => JSON.parse(line));
          expect(reports).toEqual(
            Array.from({ length: 2 }, () => ({
              args: ["日本語😀", 'quote"value', "tail\\", ""],
              cwd: runtime,
              value,
              appId: "app",
            })),
          );
          expect(result.stderr.match(/process-stderr/g)).toHaveLength(2);
        } finally {
          await fixture("remove", root);
        }
      },
    );

    it.each([
      [140, false],
      [140, true],
      [300, true],
      [4200, true],
    ] as const)(
      "reads adjacent launcher metadata at %i units (Unicode=%s)",
      async (length, unicode) => {
        const root = win32.join(directory, `${target}-launcher-${length}`);
        const parent = directoryOfLength(
          win32.join(root, unicode ? "日本語😀" : "ascii"),
          length,
        );
        const executable = win32.join(parent, "muon-launcher.exe");
        try {
          await fixture("mkdir", parent);
          await fixture("copy", executable, [
            win32.join(directory, `${target}-muon-launcher.exe`),
          ]);
          await fixture("write", win32.join(parent, "muon-install.json"), [
            "invalid json",
          ]);
          const result = await run(
            win32.join(directory, `${target}-fixture.exe`),
            ["launch", "\\\\?\\" + executable],
            {
              LOCALAPPDATA: win32.join(root, "state"),
              MUON_CACHE_DIR: win32.join(root, "cache"),
            },
          );
          expect(result.code, result.stderr).toBe(1);
          expect(result.stderr).toContain("failed to read install metadata");
        } finally {
          await fixture("remove", root);
        }
      },
    );

    it.each([
      [140, false],
      [300, false],
      [4200, false],
      [300, true],
    ] as const)(
      "updates PE resources at %i units and preserves an executable manifest (extended=%s)",
      async (length, extended) => {
        const root = win32.join(directory, target + "-resource-" + length);
        const parent = directoryOfLength(win32.join(root, "日本語😀"), length);
        const input = win32.join(parent, "入力.exe");
        const output = win32.join(parent, "出力.exe");
        const updates = win32.join(parent, "更新.json");
        const ioPath = (path: string) => (extended ? "\\\\?\\" : "") + path;
        try {
          await fixture("mkdir", parent);
          await fixture("copy", input, [
            win32.join(directory, target + "-muon-builder.exe"),
          ]);
          await fixture("copy", win32.join(parent, "図😀.ico"), [
            win32.join(directory, "app.ico"),
          ]);
          await fixture("write", updates, [
            JSON.stringify({
              version: {
                language: 1033,
                codePage: 1200,
                fixed: { fileVersion: "1.2.3.4", productVersion: "2.3.4.5" },
                strings: { FileDescription: "日本語😀 Resource Test" },
              },
              icons: [{ id: 1, language: 1033, path: "図😀.ico" }],
            }),
          ]);
          const result = await run(
            win32.join(directory, target + "-muon-builder.exe"),
            [
              "resource",
              "--input",
              ioPath(input),
              "--updates-json",
              ioPath(updates),
              "--output",
              ioPath(output),
            ],
            undefined,
          );
          expect(result.code, result.stderr).toBe(0);
          const manifest = await run(
            win32.join(directory, target + "-path-test.exe"),
            ["manifest", "\\\\?\\" + output],
            undefined,
          );
          expect(manifest.code, manifest.stderr).toBe(0);
          const launched = await run(
            win32.join(directory, target + "-fixture.exe"),
            ["launch-help", "\\\\?\\" + output],
            undefined,
          );
          expect(launched.code, launched.stderr).toBe(0);
          expect(launched.stderr).toContain("Usage: muon-builder");
          const copy = win32.join(root, "verify.exe");
          await fixture("copy", copy, ["\\\\?\\" + output]);
          const localCopy = join(localDirectory, target + "-updated.exe");
          await writeFile(localCopy, await agent.files.readFile(copy));
          await execFileAsync(process.execPath, [
            "../../builder/scripts/assert-windows-icon.mjs",
            localCopy,
            join(localDirectory, "app.ico"),
          ]);
          await execFileAsync(process.execPath, [
            "../../builder/scripts/assert-windows-version.mjs",
            localCopy,
            "--file-version",
            "1.2.3.4",
            "--product-version",
            "2.3.4.5",
            "FileDescription=日本語😀 Resource Test",
          ]);
          const partial = win32.join(parent, "partial.exe");
          const failed = await run(
            win32.join(directory, `${target}-builder-test.exe`),
            [
              "resource-failure",
              ioPath(input),
              ioPath(updates),
              ioPath(partial),
            ],
            undefined,
          );
          expect(failed.code, failed.stderr).toBe(1);
          expect(failed.stderr).toContain(
            "Injected partial resource write failure",
          );
          await fixture("absent", partial);
          // Opening a directory as output must fail without deleting its contents.
          const blocked = win32.join(parent, "blocked");
          await fixture("mkdir", blocked);
          await fixture("write", win32.join(blocked, "sentinel"), [
            "preserved",
          ]);
          const blockedResult = await run(
            win32.join(directory, `${target}-muon-builder.exe`),
            [
              "resource",
              "--input",
              ioPath(input),
              "--updates-json",
              ioPath(updates),
              "--output",
              ioPath(blocked),
            ],
            undefined,
          );
          expect(blockedResult.code).toBe(1);
          await builder("contents", win32.join(blocked, "sentinel"), [
            "preserved",
          ]);
        } finally {
          await fixture("remove", root);
        }
      },
    );

    it.each([
      [140, false, "http", false],
      [300, false, "local", true],
      [300, true, "local", true],
      [300, false, "http", true],
      [4200, false, "http", true],
      [300, true, "http", true],
    ] as const)(
      "acquires verified runtime archives at %i units (extended=%s, source=%s, Unicode=%s)",
      async (length, extended, transport, unicode) => {
        const root = win32.join(
          directory,
          target + "-transfer-" + transport + "-" + length,
        );
        const parent = directoryOfLength(
          win32.join(root, unicode ? "日本語 😀" : "ascii"),
          length,
        );
        const dist =
          transport === "http"
            ? win32.join(root, "dist")
            : win32.join(parent, "dist");
        const source = win32.join(parent, "source");
        const cefOutput = win32.join(parent, "cef");
        const output = win32.join(parent, "runtime");
        const cache = win32.join(parent, "cache");
        const ioPath = (path: string) => (extended ? "\\\\?\\" : "") + path;
        const nodeTarget = target === "windows-amd64" ? "win-x64" : "win-x86";
        const nodeName = "node-v24.4.0-" + nodeTarget + ".zip";
        const cefData = await readFile(join(localDirectory, "cef.tar.bz2"));
        const nodeData = await readFile(
          join(localDirectory, target + "-node.zip"),
        );
        let server: RemoteManagedProcess | undefined;
        try {
          await fixture("mkdir", win32.join(dist, "v24.4.0"));
          await fixture("copy", win32.join(dist, "cef.tar.bz2"), [
            win32.join(directory, "cef.tar.bz2"),
          ]);
          await fixture("copy", win32.join(dist, "v24.4.0", nodeName), [
            win32.join(directory, target + "-node.zip"),
          ]);
          await fixture("write", win32.join(dist, "cef-catalog.json"), [
            JSON.stringify({
              [target === "windows-amd64" ? "windows64" : "windows32"]: {
                versions: [
                  {
                    cef_version: "test-cef",
                    channel: "stable",
                    chromium_version: "test-chromium",
                    files: [
                      {
                        name: "cef.tar.bz2",
                        sha1: createHash("sha1").update(cefData).digest("hex"),
                        size: cefData.length,
                        type: "minimal",
                      },
                    ],
                  },
                ],
              },
            }),
          ]);
          await fixture("write", win32.join(dist, "index.json"), [
            JSON.stringify([
              {
                version: "v24.4.0",
                lts: "Krypton",
                files: [nodeTarget + "-zip"],
              },
            ]),
          ]);
          const checksums = win32.join(dist, "v24.4.0", "SHASUMS256.txt");
          await fixture("write", checksums, [
            createHash("sha256").update(nodeData).digest("hex") +
              "  " +
              nodeName +
              "\n",
          ]);
          await fixture("mkdir", source);
          await fixture("write", win32.join(source, "muon-core.exe"), [
            "core payload",
          ]);
          let cefUrl = ioPath(win32.join(dist, "cef-catalog.json"));
          let nodeUrl = ioPath(dist);
          if (transport === "http") {
            server = await agent.processes.launchManaged({
              path: win32.join(directory, target + "-download-fixture.exe"),
              arguments: ["\\\\?\\" + dist],
              workingDirectory: directory,
              createNoWindow: true,
              captureStdout: true,
              captureStderr: true,
            });
            const activeServer = server;
            await expect
              .poll(async () => await activeServer.stdoutText(), {
                timeout: 30000,
              })
              .toMatch(/^READY \d+/);
            const ready = (await server.stdoutText()).match(/^READY (\d+)/);
            expect(ready).not.toBeNull();
            nodeUrl = "http://127.0.0.1:" + ready![1];
            cefUrl = nodeUrl + "/cef-catalog.json";
            const progress = await run(
              win32.join(directory, target + "-builder-test.exe"),
              [
                "download",
                ioPath(win32.join(parent, "progress.tar.bz2")),
                nodeUrl + "/cef.tar.bz2",
                String(cefData.length),
              ],
              undefined,
            );
            expect(progress.code, progress.stderr).toBe(0);
            expect(progress.stdout.trim()).toBe(
              createHash("sha256").update(cefData).digest("hex"),
            );
          }
          const processEnvironment = {
            MUON_CEF_CATALOG_URL: cefUrl,
            MUON_NODE_DIST_URL: nodeUrl,
          };
          const buildtime = await run(
            win32.join(directory, target + "-muon-builder.exe"),
            [
              "buildtime",
              "--version",
              "test-cef",
              "--target",
              target,
              "--output-dir",
              ioPath(cefOutput),
              "--cache-dir",
              ioPath(cache),
              "--json",
            ],
            processEnvironment,
          );
          expect(buildtime.code, buildtime.stderr).toBe(0);
          expect(JSON.parse(buildtime.stdout)).toMatchObject({
            target,
            version: "test-cef",
          });
          await builder(
            "contents",
            win32.join(cefOutput, "Release", "libcef.dll"),
            ["CEF payload"],
          );
          const arguments_ = [
            "runtime",
            "--muon-path",
            ioPath(source),
            "--cef-path",
            ioPath(cefOutput),
            "--stage-dir",
            ioPath(output),
            "--cache-dir",
            ioPath(cache),
            "--target",
            target,
            "--node-runtime-requirement",
            JSON.stringify({
              required: true,
              engineRangeSpecified: false,
              engineRange: "*",
              comparatorSets: [[]],
            }),
            "--json",
          ];
          for (const cacheHit of [false, true]) {
            const runtime = await run(
              win32.join(directory, target + "-muon-builder.exe"),
              arguments_,
              processEnvironment,
            );
            expect(runtime.code, runtime.stderr).toBe(0);
            expect(JSON.parse(runtime.stdout)).toMatchObject({ cacheHit });
            await builder(
              "contents",
              win32.join(output, "runtimes", "node", "bin", "node.exe"),
              ["Node payload"],
            );
          }
          await fixture("remove", checksums);
          await fixture("write", checksums, [
            "0".repeat(64) + "  " + nodeName + "\n",
          ]);
          const corrupted = await run(
            win32.join(directory, target + "-muon-builder.exe"),
            [...arguments_, "--force"],
            processEnvironment,
          );
          expect(corrupted.code).not.toBe(0);
          expect(corrupted.stderr).toContain("SHA-256 mismatch");
          await builder(
            "contents",
            win32.join(output, "runtimes", "node", "bin", "node.exe"),
            ["Node payload"],
          );
        } finally {
          try {
            if (server !== undefined) await server.releaseAsync();
          } finally {
            await fixture("remove", root);
          }
        }
      },
    );
  },
);

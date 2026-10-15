// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFile } from "node:child_process";
import { mkdir, mkdtemp, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { promisify } from "node:util";

import { afterEach, describe, expect, it } from "vitest";

import {
  auditLinuxDependencies,
  inspectLinuxElf,
} from "../cef/linux-dependencies.js";
import { calculateLinuxDebianDependencies } from "../cef/linux-debian-dependencies.js";
import {
  createLinuxDependencyManifest,
  readLinuxReferenceCef,
} from "../cef/linux-dependency-manifest.js";

const execute = promisify(execFile);
const directories: string[] = [];

afterEach(async () => {
  for (const directory of directories.splice(0)) {
    await rm(directory, { recursive: true, force: true });
  }
});

const createLibraries = async () => {
  const root = await mkdtemp(join(tmpdir(), "muon-linux-dependencies-"));
  directories.push(root);
  const good = join(root, "good");
  const old = join(root, "old");
  await mkdir(good);
  await mkdir(old);
  await writeFile(
    join(root, "versions.map"),
    "LIB_1.0 { global: answer; legacy; };\nLIB_99.0 { global: unused_new; local: *; } LIB_1.0;\n",
  );
  await writeFile(
    join(root, "good.c"),
    "int answer(void) { return 42; }\nint legacy(void) { return 1; }\nint unused_new(void) { return 99; }\n",
  );
  await writeFile(join(root, "old.c"), "int legacy(void) { return 1; }\n");
  for (const [source, output] of [
    ["good", good],
    ["old", old],
  ] as const) {
    await execute("gcc", [
      "-shared",
      "-fPIC",
      "-nostdlib",
      "-Wl,-soname,libfixture.so",
      `-Wl,--version-script=${join(root, "versions.map")}`,
      join(root, `${source}.c`),
      "-o",
      join(output, "libfixture.so"),
    ]);
  }
  await writeFile(
    join(root, "consumer.c"),
    "extern int answer(void);\nextern int optional(void) __attribute__((weak));\nint run(void) { return answer() + (optional ? optional() : 0); }\n",
  );
  const consumer = join(root, "libconsumer.so");
  await execute("gcc", [
    "-shared",
    "-fPIC",
    "-nostdlib",
    "-Wl,-soname,libconsumer.so",
    join(root, "consumer.c"),
    `-L${good}`,
    "-lfixture",
    "-o",
    consumer,
  ]);
  return { root, good, old, consumer };
};

describe.skipIf(process.platform !== "linux" || process.arch !== "x64")(
  "Linux binary dependency requirements",
  () => {
    it("reads the native build reference and rejects a cache for another target", async () => {
      const fixture = await createLibraries();
      const cache = join(fixture.root, "CMakeCache.txt");
      await writeFile(
        cache,
        "MUON_TARGET_NAME:STRING=linux-amd64\nMUON_CEF_VERSION:STRING=147.reference\nMUON_CEF_PACKAGE:STRING=cef_reference.tar.bz2\nMUON_CEF_SHA1:STRING=" +
          "a".repeat(40) +
          "\nMUON_CEF_PACKAGE_URL:STRING=https://example.invalid/cef_reference.tar.bz2\n",
      );
      expect(await readLinuxReferenceCef(cache, "linux-amd64")).toEqual({
        version: "147.reference",
        archive: "cef_reference.tar.bz2",
        sha1: "a".repeat(40),
        url: "https://example.invalid/cef_reference.tar.bz2",
      });
      await expect(readLinuxReferenceCef(cache, "linux-arm64")).rejects.toThrow(
        "build target does not match linux-arm64",
      );
    });

    it("extracts required versions without confusing newer definitions with requirements", async () => {
      const fixture = await createLibraries();
      const consumer = await inspectLinuxElf(fixture.consumer, "linux-amd64");
      const provider = await inspectLinuxElf(
        join(fixture.good, "libfixture.so"),
        "linux-amd64",
      );
      expect(consumer.needed).toEqual(["libfixture.so"]);
      expect(consumer.imports).toContainEqual({
        name: "answer",
        version: "LIB_1.0",
        weak: false,
      });
      expect(consumer.imports).toContainEqual({
        name: "optional",
        version: undefined,
        weak: true,
      });
      expect(consumer.versionNeeds).toEqual([
        { library: "libfixture.so", version: "LIB_1.0", weak: false },
      ]);
      expect(provider.versionNeeds).toEqual([]);
      expect(provider.versionDefinitions).toContain("LIB_99.0");
      expect(consumer.sha256).toMatch(/^[a-f0-9]{64}$/);
    });

    it("accepts a library providing every required symbol and permits absent weak imports", async () => {
      const fixture = await createLibraries();
      const result = await auditLinuxDependencies({
        target: "linux-amd64",
        files: [fixture.consumer],
        libraryPaths: [fixture.good],
      });
      expect(result.issues).toEqual([]);
      expect(result.binaries.map((binary) => binary.soname).sort()).toEqual([
        "libconsumer.so",
        "libfixture.so",
      ]);
    });

    it("rejects a missing symbol even when the replacement provides the required version node", async () => {
      const fixture = await createLibraries();
      const result = await auditLinuxDependencies({
        target: "linux-amd64",
        files: [fixture.consumer],
        libraryPaths: [fixture.old],
      });
      expect(result.issues).toContainEqual({
        kind: "symbol",
        binary: fixture.consumer,
        requirement: "answer@LIB_1.0",
      });
      expect(result.issues.some((issue) => issue.kind === "version")).toBe(
        false,
      );
    });

    it("reports an unavailable library instead of consulting the build host implicitly", async () => {
      const fixture = await createLibraries();
      const result = await auditLinuxDependencies({
        target: "linux-amd64",
        files: [fixture.consumer],
        libraryPaths: [],
      });
      expect(result.issues).toContainEqual({
        kind: "library",
        binary: fixture.consumer,
        requirement: "libfixture.so",
      });
    });

    it("follows transitive dependencies and identifies the binary requiring a missing symbol", async () => {
      const fixture = await createLibraries();
      await writeFile(
        join(fixture.root, "outer.c"),
        "extern int run(void); int outer(void) { return run(); }\n",
      );
      const outer = join(fixture.root, "libouter.so");
      await execute("gcc", [
        "-shared",
        "-fPIC",
        "-nostdlib",
        join(fixture.root, "outer.c"),
        `-L${fixture.root}`,
        "-lconsumer",
        "-o",
        outer,
      ]);
      const result = await auditLinuxDependencies({
        target: "linux-amd64",
        files: [outer],
        libraryPaths: [fixture.root, fixture.old],
      });
      expect(result.binaries).toHaveLength(3);
      expect(result.issues).toContainEqual({
        kind: "symbol",
        binary: fixture.consumer,
        requirement: "answer@LIB_1.0",
      });
    });

    it("rejects a binary for the wrong CPU before resolving its libraries", async () => {
      const fixture = await createLibraries();
      await expect(
        inspectLinuxElf(fixture.consumer, "linux-arm64"),
      ).rejects.toThrow("ELF machine does not match linux-arm64");
    });

    it("calculates real Debian version constraints while excluding the application's private libraries", async () => {
      const fixture = await createLibraries();
      const entry = join(fixture.root, "entry");
      await writeFile(
        join(fixture.root, "entry.c"),
        '#include <stdio.h>\nextern int answer(void);\nint main(void) { printf("%d\\n", answer()); return 0; }\n',
      );
      await execute("gcc", [
        join(fixture.root, "entry.c"),
        `-L${fixture.good}`,
        "-lfixture",
        "-o",
        entry,
      ]);
      const libraryPaths = [
        fixture.good,
        "/usr/lib/x86_64-linux-gnu",
        "/lib/x86_64-linux-gnu",
      ];
      const audit = await auditLinuxDependencies({
        target: "linux-amd64",
        files: [entry],
        libraryPaths,
      });
      expect(audit.issues).toEqual([]);
      const result = await calculateLinuxDebianDependencies({
        audit,
        privateFiles: [entry, join(fixture.good, "libfixture.so")],
        libraryPaths,
      });
      expect(result.depends).toEqual([
        expect.stringMatching(/^libc6 \(>= [^)]+\)$/),
      ]);
      expect(result.tool).toContain("dpkg-shlibdeps");
      expect(result.diagnostics).toEqual([]);
    });

    it("refuses to generate package constraints from an unsuccessful ABI audit", async () => {
      const fixture = await createLibraries();
      const audit = await auditLinuxDependencies({
        target: "linux-amd64",
        files: [fixture.consumer],
        libraryPaths: [fixture.old],
      });
      await expect(
        calculateLinuxDebianDependencies({
          audit,
          privateFiles: [fixture.consumer],
          libraryPaths: [fixture.old],
        }),
      ).rejects.toThrow("unresolved requirements");
    });

    it("uses the build reference even when application runtime CEF policies differ", async () => {
      const fixture = await createLibraries();
      const application = join(fixture.root, "application");
      await mkdir(application);
      await execute("gcc", [
        "-shared",
        "-fPIC",
        "-nostdlib",
        "-Wl,-soname,libconsumer.so",
        join(fixture.root, "consumer.c"),
        `-L${fixture.good}`,
        "-lfixture",
        "-Wl,--no-as-needed",
        "-lm",
        "-o",
        join(application, "libconsumer.so"),
      ]);
      const referenceCef = {
        version: "reference-build",
        archive: "reference.tar.bz2",
        sha1: "1234",
        url: "https://example.invalid/reference.tar.bz2",
      };
      let first:
        Awaited<ReturnType<typeof createLinuxDependencyManifest>> | undefined;
      for (const policy of [
        "tested",
        "same-major-latest",
        "compat-latest",
        "exact",
      ]) {
        await writeFile(
          join(application, "muon.json"),
          JSON.stringify({ cef: { policy, version: "runtime-choice" } }),
        );
        const result = await createLinuxDependencyManifest({
          target: "linux-amd64",
          referenceCef,
          cefDirectory: fixture.good,
          runtimeDirectory: application,
          nativeDirectory: undefined,
          libraryPaths: ["/usr/lib/x86_64-linux-gnu", "/lib/x86_64-linux-gnu"],
        });
        expect(result.referenceCef).toEqual(referenceCef);
        expect(result.audit.issues).toEqual([]);
        expect(result.audit.roots).toEqual([
          "cef/libfixture.so",
          "runtime/libconsumer.so",
        ]);
        expect(JSON.stringify(result).includes(fixture.root)).toBe(false);
        expect(result.debian!.diagnostics.length).toBeGreaterThan(0);
        expect(
          JSON.stringify(result).includes("/tmp/muon-debian-dependencies-"),
        ).toBe(false);
        if (first) expect(result).toEqual(first);
        first = result;
      }
    });
  },
);

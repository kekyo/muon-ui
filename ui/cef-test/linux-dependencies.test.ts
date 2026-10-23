// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFile } from "node:child_process";
import {
  copyFile,
  mkdir,
  mkdtemp,
  readFile,
  rm,
  symlink,
  writeFile,
} from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { promisify } from "node:util";

import { afterEach, describe, expect, it } from "vitest";

import {
  auditLinuxDependencies,
  inspectLinuxElf,
} from "../cef/linux-dependencies.js";
import { calculateLinuxDebianDependencies } from "../cef/linux-debian-dependencies.js";
import { getLinuxAppDependencies } from "../cef/linux-app-dependencies.js";
import {
  readLinuxDebianLibraries,
  getLinuxDebianSymbolDependencies,
} from "../cef/linux-debian-symbols.js";
import {
  createLinuxDependencyManifest,
  collectLinuxElfFiles,
  readLinuxReferenceCef,
  type LinuxDependencyManifest,
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

const createTemporaryDpkgDirectory = async (): Promise<string> => {
  const directory = await mkdtemp(join(tmpdir(), "muon-debian-symbols-"));
  directories.push(directory);
  await mkdir(join(directory, "info"));
  // dpkg's multiarch database format uses architecture-qualified control files.
  await writeFile(join(directory, "info/format"), "1\n");
  return directory;
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

    it("audits linked output without treating intermediate object files as runtime plugins", async () => {
      const fixture = await createLibraries();
      const object = join(fixture.root, "intermediate.o");
      await execute("gcc", [
        "-c",
        join(fixture.root, "consumer.c"),
        "-o",
        object,
      ]);
      const files = await collectLinuxElfFiles(fixture.root);
      expect(files).toContain(fixture.consumer);
      expect(files).not.toContain(object);
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

    it("retains symbol-level package requirements for plugins added after the native build", async () => {
      const fixture = await createLibraries();
      await writeFile(
        join(fixture.root, "plugin.c"),
        "extern int gettid(void);\nint plugin(void) { return gettid(); }\n",
      );
      const plugin = join(fixture.root, "plugin.so");
      await execute("gcc", [
        "-shared",
        "-fPIC",
        "-nostdlib",
        join(fixture.root, "plugin.c"),
        "-lc",
        "-o",
        plugin,
      ]);
      const binary = await inspectLinuxElf(plugin, "linux-amd64");
      const audit = await auditLinuxDependencies({
        target: "linux-amd64",
        files: [plugin],
        libraryPaths: ["/usr/lib/x86_64-linux-gnu", "/lib/x86_64-linux-gnu"],
      });
      const libraries = await readLinuxDebianLibraries(
        audit.binaries
          .filter((item) => item.soname !== undefined)
          .map((item) => ({ path: item.path, soname: item.soname! })),
        "/var/lib/dpkg",
      );
      expect(
        getLinuxDebianSymbolDependencies(
          libraries["libc.so.6"]!,
          binary.imports,
        ),
      ).toContain("libc6 (>= 2.30)");
      expect(libraries["libc.so.6"]!.source).toContain("libc6");
      expect(libraries["libc.so.6"]!.sha256).toMatch(/^[a-f0-9]{64}$/);
    });

    it("keeps library fallbacks and additional symbol dependency templates", async () => {
      const directory = await createTemporaryDpkgDirectory();
      const metadata = join(directory, "info");
      const fixture = join(directory, "libfixture.so.1");
      const fallback = join(directory, "libfallback.so.1");
      await writeFile(fixture, "");
      await writeFile(fallback, "");
      await writeFile(
        join(directory, "status"),
        ["libfixture1", "libfallback1"]
          .map(
            (name) =>
              `Package: ${name}\nStatus: install ok installed\nArchitecture: amd64\nMulti-Arch: same\nVersion: 4.0\nMaintainer: Muon <muon@example.invalid>\nDescription: Dependency metadata fixture\n\n`,
          )
          .join(""),
      );
      await writeFile(join(metadata, "libfixture1:amd64.list"), `${fixture}\n`);
      await writeFile(
        join(metadata, "libfallback1:amd64.list"),
        `${fallback}\n`,
      );
      await writeFile(
        join(metadata, "libfixture1:amd64.symbols"),
        "libfixture.so.1 libfixture1 #MINVER#\n| libfixture-driver #MINVER#\n basic@Base 1.0\n accelerated@Base 2:3.1-1~test 1\n",
      );
      await writeFile(
        join(metadata, "libfallback1:amd64.shlibs"),
        "libfallback 1 libfallback1 (>= 4.0)\nudeb: libfallback 1 libfallback1-udeb\n",
      );
      const libraries = await readLinuxDebianLibraries(
        [
          { path: fixture, soname: "libfixture.so.1" },
          { path: fallback, soname: "libfallback.so.1" },
        ],
        directory,
      );
      expect(
        getLinuxDebianSymbolDependencies(libraries["libfixture.so.1"]!, [
          { name: "accelerated", version: undefined, weak: false },
        ]),
      ).toEqual(
        expect.arrayContaining([
          "libfixture1 (>= 2:3.1-1~test)",
          "libfixture-driver (>= 2:3.1-1~test)",
        ]),
      );
      expect(
        getLinuxDebianSymbolDependencies(libraries["libfallback.so.1"]!, []),
      ).toEqual(["libfallback1 (>= 4.0)"]);
      await expect(
        readLinuxDebianLibraries(
          [{ path: fixture, soname: "missing.so.1" }],
          directory,
        ),
      ).rejects.toThrow("missing.so.1");
    });

    it.each(["symbols", "shlibs"])(
      "uses the resolved library's package %s when another ABI has the same SONAME",
      async (format) => {
        const directory = await createTemporaryDpkgDirectory();
        const metadata = join(directory, "info");
        await mkdir(join(directory, "lib64"));
        await mkdir(join(directory, "lib32"));
        await symlink(join(directory, "lib64"), join(directory, "lib"));
        const provider = join(directory, "lib64/libfixture.so.1");
        const otherAbi = join(directory, "lib32/libfixture.so.1");
        await writeFile(provider, "");
        await writeFile(otherAbi, "");
        await writeFile(
          join(directory, "status"),
          ["libfixture1", "libfixture1-32"]
            .map(
              (name) =>
                `Package: ${name}\nStatus: install ok installed\nArchitecture: amd64\n${name === "libfixture1" ? "Multi-Arch: same\n" : ""}Version: 99.0\nMaintainer: Muon <muon@example.invalid>\nDescription: Dependency metadata fixture\n\n`,
            )
            .join(""),
        );
        await writeFile(
          join(metadata, "libfixture1:amd64.list"),
          `${join(directory, "lib/libfixture.so.1")}\n`,
        );
        await writeFile(join(metadata, "libfixture1-32.list"), `${otherAbi}\n`);
        await writeFile(
          join(metadata, "libfixture1:amd64.symbols"),
          "libfixture.so.1 libfixture1 #MINVER#\n available@Base 2.0\n",
        );
        await writeFile(
          join(metadata, "libfixture1:amd64.shlibs"),
          "libfixture 1 libfixture1 (>= 3.0)\n",
        );
        if (format === "shlibs")
          await rm(join(metadata, "libfixture1:amd64.symbols"));
        await writeFile(
          join(metadata, "libfixture1-32.symbols"),
          "libfixture.so.1 libfixture1-32 #MINVER#\n available@Base 99.0\n",
        );
        await writeFile(
          join(metadata, "libfixture1-32.shlibs"),
          "libfixture 1 libfixture1-32 (>= 99.0)\n",
        );
        const libraries = await readLinuxDebianLibraries(
          [{ path: provider, soname: "libfixture.so.1" }],
          directory,
        );
        const library = libraries["libfixture.so.1"]!;
        expect(library.source).toBe(`libfixture1:amd64.${format}`);
        expect(
          getLinuxDebianSymbolDependencies(library, [
            { name: "available", version: undefined, weak: false },
          ]),
        ).toEqual(
          format === "symbols"
            ? ["libfixture1", "libfixture1 (>= 2.0)"]
            : ["libfixture1 (>= 3.0)"],
        );
        expect(
          getLinuxDebianSymbolDependencies(library, [
            { name: "unlisted", version: undefined, weak: false },
          ]),
        ).toEqual(
          format === "symbols"
            ? ["libfixture1", "libfixture1 (>= 3.0)"]
            : ["libfixture1 (>= 3.0)"],
        );
        // A same-named file outside the installed package is not that package's
        // library, and two claimed owners must not silently select one of them.
        const unowned = join(directory, "libfixture.so.1");
        await writeFile(unowned, "");
        await expect(
          readLinuxDebianLibraries(
            [{ path: unowned, soname: "libfixture.so.1" }],
            directory,
          ),
        ).rejects.toThrow("No Debian package owns");
        await writeFile(join(metadata, "libfixture1-32.list"), `${provider}\n`);
        await expect(
          readLinuxDebianLibraries(
            [{ path: provider, soname: "libfixture.so.1" }],
            directory,
          ),
        ).rejects.toThrow("Ambiguous Debian package ownership");
      },
    );

    it("composes plugin requirements from the saved baseline and rejects unavailable symbols or libraries", async () => {
      const fixture = await createLibraries();
      const audit = await auditLinuxDependencies({
        target: "linux-amd64",
        files: [join(fixture.good, "libfixture.so")],
        libraryPaths: [],
      });
      const manifest: LinuxDependencyManifest = {
        schemaVersion: 1,
        runtimePackages: [],
        referenceCef: {
          version: "build-reference",
          archive: "cef.tar.bz2",
          sha1: "a".repeat(40),
          url: "https://example.invalid/cef.tar.bz2",
        },
        baseline: { osRelease: "fixture", packages: [], readelf: "readelf" },
        audit,
        debian: {
          depends: ["libc6 (>= 2.28)"],
          tool: "fixture",
          diagnostics: [],
        },
        debianLibraries: {
          "libfixture.so": {
            source: "libfixture1:amd64.symbols",
            sha256: "a".repeat(64),
            templates: ["libfixture1 #MINVER#"],
            symbols: { "answer@LIB_1.0": { minimum: "2.0", template: 0 } },
            fallback: undefined,
          },
        },
      };
      expect(
        await getLinuxAppDependencies(manifest, [fixture.consumer]),
      ).toEqual(
        expect.arrayContaining(["libc6 (>= 2.28)", "libfixture1 (>= 2.0)"]),
      );
      const old = await auditLinuxDependencies({
        target: "linux-amd64",
        files: [join(fixture.old, "libfixture.so")],
        libraryPaths: [],
      });
      await expect(
        getLinuxAppDependencies({ ...manifest, audit: old }, [
          fixture.consumer,
        ]),
      ).rejects.toThrow("answer@LIB_1.0");
      const empty = { ...audit, roots: [], binaries: [], resolutions: {} };
      await expect(
        getLinuxAppDependencies({ ...manifest, audit: empty }, [
          fixture.consumer,
        ]),
      ).rejects.toThrow("libfixture.so");
      // A private provider resolves the plugin without adding a host package.
      expect(
        await getLinuxAppDependencies({ ...manifest, audit: empty }, [
          fixture.consumer,
          join(fixture.good, "libfixture.so"),
        ]),
      ).toEqual(["libc6 (>= 2.28)"]);

      // The system package owns its implementation dependencies. A later OS
      // may replace those libraries without changing libfixture's public ABI.
      await writeFile(
        join(fixture.root, "detail.c"),
        "int detail(void) { return 42; }\n",
      );
      await execute("gcc", [
        "-shared",
        "-fPIC",
        "-nostdlib",
        "-Wl,-soname,libdetail.so.1",
        join(fixture.root, "detail.c"),
        "-o",
        join(fixture.good, "libdetail.so.1"),
      ]);
      await writeFile(
        join(fixture.root, "good.c"),
        "extern int detail(void); int answer(void) { return detail(); }\n",
      );
      await execute("gcc", [
        "-shared",
        "-fPIC",
        "-nostdlib",
        "-Wl,-soname,libfixture.so",
        `-Wl,--version-script=${join(fixture.root, "versions.map")}`,
        join(fixture.root, "good.c"),
        `-L${fixture.good}`,
        "-l:libdetail.so.1",
        "-o",
        join(fixture.good, "libfixture.so"),
      ]);
      const transitive = await auditLinuxDependencies({
        target: "linux-amd64",
        files: [join(fixture.good, "libfixture.so")],
        libraryPaths: [fixture.good],
      });
      expect(
        await getLinuxAppDependencies({ ...manifest, audit: transitive }, [
          fixture.consumer,
        ]),
      ).toEqual(
        expect.arrayContaining(["libc6 (>= 2.28)", "libfixture1 (>= 2.0)"]),
      );
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
          JSON.stringify({ launcher: { defaultVersionPolicy: policy } }),
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
        expect(result.runtimePackages).toEqual(["ca-certificates", "curl"]);
        expect(result.audit.issues).toEqual([]);
        expect(result.debianLibraries["libm.so.6"]?.source).toContain("libc6");
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

// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFile } from "node:child_process";
import {
  mkdir,
  mkdtemp,
  realpath,
  rm,
  symlink,
  writeFile,
} from "node:fs/promises";
import { tmpdir } from "node:os";
import { basename, join, resolve } from "node:path";
import { promisify } from "node:util";

import { runScriptOnceToText, type FunCityLogEntry } from "funcity";

import type { LinuxDependencyAudit } from "./linux-dependencies.js";

/** Debian dependency calculation inputs for an already audited target. */
export interface LinuxDebianDependencyOptions {
  /** Successful symbol audit for the same environment. */
  audit: LinuxDependencyAudit;
  /** Files distributed with the application, including the reference CEF. */
  privateFiles: readonly string[];
  /** Explicit search paths inside the target build environment. */
  libraryPaths: readonly string[];
}

/** Package constraints calculated using the target's Debian package metadata. */
export interface LinuxDebianDependencies {
  /** Comma-separated Depends entries, retaining Debian version semantics. */
  depends: string[];
  /** Diagnostics retained with temporary staging paths restored to input paths. */
  diagnostics: string[];
  /** Tool version used to calculate the result. */
  tool: string;
}

const execute = promisify(execFile);
const analysisPackage = "muon-dependency-analysis";

/**
 * Calculates minimum package dependencies using dpkg-shlibdeps.
 * @param options - Audited inputs and private library classification.
 * @returns Debian dependency clauses and the tool's diagnostics.
 * @remarks Must run inside the selected Debian-family baseline environment.
 */
export const calculateLinuxDebianDependencies = async (
  options: LinuxDebianDependencyOptions,
): Promise<LinuxDebianDependencies> => {
  if (options.audit.issues.length) {
    throw new Error(
      "Cannot calculate Debian dependencies with unresolved requirements.",
    );
  }
  const environment: NodeJS.ProcessEnv = {
    ...process.env,
    LC_ALL: "C",
    // dpkg groups diagnostic filenames in Perl hashes; stabilize their order.
    PERL_HASH_SEED: "0",
    PERL_PERTURB_KEYS: "0",
  };
  delete environment.LD_LIBRARY_PATH;
  const { stdout: architecture } = await execute(
    "dpkg",
    ["--print-architecture"],
    { env: environment },
  );
  const expectedArchitecture = options.audit.target.slice("linux-".length);
  if (architecture.trim() !== expectedArchitecture) {
    throw new Error(
      `Run dependency calculation in the ${expectedArchitecture} baseline environment; current dpkg architecture is ${architecture.trim()}.`,
    );
  }
  const { stdout: version } = await execute("dpkg-shlibdeps", ["--version"], {
    env: environment,
  });
  const directory = await mkdtemp(join(tmpdir(), "muon-debian-dependencies-"));
  try {
    const packageRoot = join(directory, "debian", analysisPackage);
    const libraries = join(packageRoot, "usr/lib");
    const binaries = join(packageRoot, "usr/bin");
    const metadata = join(packageRoot, "DEBIAN");
    await mkdir(libraries, { recursive: true });
    await mkdir(binaries, { recursive: true });
    await mkdir(metadata);
    const logs: FunCityLogEntry[] = [];
    const control = await runScriptOnceToText(
      "Source: {{package}}\nSection: misc\nPriority: optional\nMaintainer: Muon <muon@example.invalid>\nStandards-Version: 4.6.2\n\nPackage: {{package}}\nArchitecture: any\nDescription: Build-time dependency analysis\n",
      {
        sourceId: "muon-dependency-control",
        variables: new Map([["package", analysisPackage]]),
        logs,
      },
    );
    if (control === undefined || logs.some((entry) => entry.type === "error")) {
      throw new Error(
        `Cannot render dependency analysis control: ${JSON.stringify(logs)}`,
      );
    }
    await writeFile(join(directory, "debian/control"), control);
    const symbols: string[] = [];
    const stagedNames = new Map<string, string>();
    const stagedPaths = new Map<string, string>();
    const analysisInputs: string[] = [];
    const privatePaths = new Set<string>();
    for (const file of options.privateFiles)
      privatePaths.add(await realpath(file));
    for (const path of privatePaths) {
      const binary = options.audit.binaries.find((item) => item.path === path);
      if (!binary) throw new Error(`Private library was not audited: ${path}`);
      const name = binary.soname ?? basename(path);
      const previous = stagedNames.get(name);
      if (previous !== undefined && previous !== binary.sha256) {
        throw new Error(`Conflicting private library or executable: ${name}`);
      }
      if (previous !== undefined) continue;
      stagedNames.set(name, binary.sha256);
      const destination = join(binary.soname ? libraries : binaries, name);
      await symlink(path, destination);
      stagedPaths.set(destination, path);
      if (binary.needed.length) analysisInputs.push(destination);
      if (binary.soname) {
        // Give dpkg explicit metadata for private libraries. It can then inspect
        // their system dependencies without inventing OS packages for CEF/muon.
        symbols.push(`${binary.soname} ${analysisPackage} #MINVER#`);
        for (const symbol of binary.exports) {
          symbols.push(` ${symbol.name}@${symbol.version ?? "Base"} 0`);
        }
      }
    }
    await writeFile(join(metadata, "symbols"), `${symbols.join("\n")}\n`);
    if (!analysisInputs.length)
      return { depends: [], diagnostics: [], tool: version.split("\n")[0]! };
    const { stdout, stderr } = await execute(
      "dpkg-shlibdeps",
      [
        "-O",
        `-x${analysisPackage}`,
        `-S${packageRoot}`,
        `-l${libraries}`,
        ...options.libraryPaths.map((path) => `-l${resolve(path)}`),
        ...analysisInputs.map((path) => `-e${path}`),
      ],
      { cwd: directory, env: environment, maxBuffer: 16 * 1024 * 1024 },
    );
    const result = stdout
      .split("\n")
      .find((line) => line.startsWith("shlibs:Depends="));
    if (result === undefined && stdout.trim())
      throw new Error(
        `dpkg-shlibdeps did not return dependency information: ${stdout}`,
      );
    // dpkg emits no variable when every dependency belongs to our private package.
    const clauses = result?.slice("shlibs:Depends=".length).trim() ?? "";
    return {
      depends: clauses ? clauses.split(/,\s*/) : [],
      diagnostics: stderr
        .split("\n")
        .filter((line) => line.trim())
        .map((line) => {
          for (const [staged, original] of [...stagedPaths].sort(
            (left, right) => right[0].length - left[0].length,
          ))
            line = line.replaceAll(staged, original);
          return line;
        }),
      tool: version.split("\n")[0]!,
    };
  } finally {
    await rm(directory, { recursive: true, force: true });
  }
};

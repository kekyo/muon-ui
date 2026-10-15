// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFile } from "node:child_process";
import { createHash } from "node:crypto";
import { createReadStream } from "node:fs";
import { open, realpath, stat } from "node:fs/promises";
import { isAbsolute, join, resolve } from "node:path";
import { promisify } from "node:util";

import type { MuonDesktopTarget } from "./targets.js";

/** Linux CPU target accepted by the dependency analyzer. */
export type LinuxDependencyTarget = Extract<
  MuonDesktopTarget,
  `linux-${string}`
>;

/** A dynamic symbol required or provided by an ELF object. */
export interface LinuxElfSymbol {
  /** Unmangled symbol name. */
  name: string;
  /** Required/provided symbol version, if versioned. */
  version: string | undefined;
  /** Whether the reference or definition is weak. */
  weak: boolean;
}

/** An ELF definition, including whether it satisfies unversioned references. */
export interface LinuxElfExport extends LinuxElfSymbol {
  /** True for unversioned definitions and default (@@) versions. */
  defaultVersion: boolean;
}

/** A version node required from a specific shared library. */
export interface LinuxElfVersionNeed {
  /** Library recorded in the version needs table. */
  library: string;
  /** Required version node. */
  version: string;
  /** Whether the version requirement is weak. */
  weak: boolean;
}

/** Statically extracted loader information; inspecting it never executes the ELF. */
export interface LinuxElfInfo {
  /** Canonical path of the analyzed file. */
  path: string;
  /** SHA-256 of the complete file. */
  sha256: string;
  /** Direct DT_NEEDED entries, in loader order. */
  needed: string[];
  /** Shared library name, if present. */
  soname: string | undefined;
  /** Recorded RPATH/RUNPATH components, for packaging diagnostics. */
  runpath: string[];
  /** Imported symbols, including optional weak references. */
  imports: LinuxElfSymbol[];
  /** Definitions visible to other objects. */
  exports: LinuxElfExport[];
  /** Version nodes required from dependencies. */
  versionNeeds: LinuxElfVersionNeed[];
  /** Version nodes defined by this object, not its requirements. */
  versionDefinitions: string[];
}

/** An unmet requirement, attributed to the binary that introduced it. */
export interface LinuxDependencyIssue {
  /** Missing library, version node, or symbol. */
  kind: "library" | "version" | "symbol";
  /** Canonical path of the requiring binary. */
  binary: string;
  /** Missing library name, version node, or name@version. */
  requirement: string;
}

/** Inputs to an isolated build-time audit, independent of runtime CEF policies. */
export interface LinuxDependencyAuditOptions {
  /** Required CPU architecture. */
  target: LinuxDependencyTarget;
  /** Entry binaries, including plugins and the build reference CEF. */
  files: readonly string[];
  /** Explicit target library search paths, in precedence order; no host fallback. */
  libraryPaths: readonly string[];
}

/** Dependency closure and unresolved requirements for a target environment. */
export interface LinuxDependencyAudit {
  /** Validated target CPU. */
  target: LinuxDependencyTarget;
  /** Canonical entry paths. */
  roots: string[];
  /** Every binary in the dependency closure, each inspected once. */
  binaries: LinuxElfInfo[];
  /** Direct resolution edges, keyed by canonical binary path and DT_NEEDED. */
  resolutions: Record<string, Record<string, string>>;
  /** All unresolved mandatory requirements. */
  issues: LinuxDependencyIssue[];
}

const execute = promisify(execFile);
const elfMachines: Record<LinuxDependencyTarget, number> = {
  "linux-amd64": 62,
  "linux-armhf": 40,
  "linux-arm64": 183,
};

/**
 * Reads imports, exports and versions using GNU readelf and verifies the CPU.
 * @param path - ELF executable or shared library to inspect.
 * @param target - Expected target architecture.
 * @returns Loader information and a content hash.
 * @remarks Requires binutils; target code is never executed. Readelf diagnostics
 * are reported as errors rather than returning an incomplete successful audit.
 */
export const inspectLinuxElf = async (
  path: string,
  target: LinuxDependencyTarget,
): Promise<LinuxElfInfo> => {
  const canonical = await realpath(path);
  const handle = await open(canonical, "r");
  const header = Buffer.alloc(20);
  try {
    const { bytesRead } = await handle.read(header, 0, header.length, 0);
    if (
      bytesRead !== header.length ||
      header.readUInt32LE(0) !== 0x464c457f ||
      header[5] !== 1
    ) {
      throw new Error(`Expected a little-endian ELF file: ${canonical}`);
    }
    if (
      header[4] !== (target === "linux-armhf" ? 1 : 2) ||
      header.readUInt16LE(18) !== elfMachines[target]
    ) {
      throw new Error(`ELF machine does not match ${target}: ${canonical}`);
    }
  } finally {
    await handle.close();
  }
  const { stdout, stderr } = await execute(
    "readelf",
    ["--wide", "--dynamic", "--version-info", "--dyn-syms", canonical],
    {
      encoding: "utf8",
      maxBuffer: 64 * 1024 * 1024,
      env: { ...process.env, LC_ALL: "C" },
    },
  );
  if (stderr.trim())
    throw new Error(`readelf diagnostics for ${canonical}: ${stderr.trim()}`);
  const hash = createHash("sha256");
  for await (const chunk of createReadStream(canonical)) hash.update(chunk);
  const needed: string[] = [];
  const runpath: string[] = [];
  let soname: string | undefined;
  const versionNeeds: LinuxElfVersionNeed[] = [];
  const versionDefinitions: string[] = [];
  const neededVersionIndices = new Set<number>();
  let section = "";
  let versionLibrary = "";
  const symbolRows: RegExpMatchArray[] = [];
  for (const line of stdout.split("\n")) {
    const dynamic = line.match(
      /\((NEEDED|SONAME|RPATH|RUNPATH)\).*\[([^\]]*)\]/,
    );
    if (dynamic) {
      const value = dynamic[2]!;
      if (dynamic[1] === "NEEDED") needed.push(value);
      else if (dynamic[1] === "SONAME") soname = value;
      else runpath.push(...value.split(":"));
    }
    if (line.startsWith("Version needs section")) section = "needs";
    else if (line.startsWith("Version definition section"))
      section = "definitions";
    else if (
      /^(Version symbols section|Symbol table |Dynamic section)/.test(line)
    )
      section = "";
    if (section === "needs") {
      const file = line.match(/File:\s+(\S+)/);
      if (file) versionLibrary = file[1]!;
      const entry = line.match(
        /Name:\s+(\S+)\s+Flags:\s+(\S+)\s+Version:\s+(\d+)/,
      );
      if (entry) {
        if (!versionLibrary)
          throw new Error(`Invalid ELF version needs in ${canonical}`);
        versionNeeds.push({
          library: versionLibrary,
          version: entry[1]!,
          weak: entry[2]!.includes("WEAK"),
        });
        neededVersionIndices.add(Number(entry[3]));
      }
    } else if (section === "definitions") {
      const definition = line.match(/Name:\s+(\S+)/);
      if (definition) versionDefinitions.push(definition[1]!);
    }
    const symbol = line.match(
      /^\s*\d+:\s+[0-9a-f]+\s+\S+\s+\S+\s+(GLOBAL|WEAK|UNIQUE)\s+(DEFAULT|PROTECTED|HIDDEN|INTERNAL)\s+(\S+)\s+(\S+)(?:\s+\((\d+)\))?\s*$/,
    );
    if (symbol) symbolRows.push(symbol);
  }
  const imports: LinuxElfSymbol[] = [];
  const exports: LinuxElfExport[] = [];
  for (const row of symbolRows) {
    const named = row[4]!;
    const versioned = named.match(/^(.+?)(@@?)([^@]+)$/);
    const symbol: LinuxElfSymbol = {
      name: versioned ? versioned[1]! : named,
      version: versioned?.[3],
      weak: row[1] === "WEAK",
    };
    // Executable copy relocations have a section index but still require a
    // version supplied by a dependency. Do not mistake them for local exports.
    if (
      row[3] === "UND" ||
      (row[5] !== undefined && neededVersionIndices.has(Number(row[5])))
    ) {
      imports.push(symbol);
    } else if (row[2] === "DEFAULT" || row[2] === "PROTECTED") {
      exports.push({
        ...symbol,
        defaultVersion: !versioned || versioned[2] === "@@",
      });
    }
  }
  return {
    path: canonical,
    sha256: hash.digest("hex"),
    needed,
    soname,
    runpath,
    imports,
    exports,
    versionNeeds,
    versionDefinitions,
  };
};

const findLibrary = async (
  name: string,
  directories: readonly string[],
): Promise<string | undefined> => {
  // Absolute DT_NEEDED entries are not portable and must not escape the explicit
  // target search paths into the machine running the audit.
  if (isAbsolute(name) || name.includes("/")) return undefined;
  for (const directory of directories) {
    const candidate = join(directory, name);
    try {
      if ((await stat(candidate)).isFile()) return await realpath(candidate);
    } catch (error) {
      if ((error as NodeJS.ErrnoException).code !== "ENOENT") throw error;
    }
  }
  return undefined;
};

/**
 * Resolves a target's full library closure and verifies required symbol sets.
 * @param options - Entry binaries and explicit paths from the chosen baseline.
 * @returns Requirements with their origin and every unresolved mandatory item.
 * @remarks This is a build-time ABI audit, not a runtime CEF selection filter.
 * Library paths describe the target environment; host defaults, LD_LIBRARY_PATH
 * and recorded absolute RPATHs are never consulted implicitly. Packaging must
 * separately arrange equivalent loader paths in the resulting application.
 */
export const auditLinuxDependencies = async (
  options: LinuxDependencyAuditOptions,
): Promise<LinuxDependencyAudit> => {
  const roots: string[] = [];
  for (const file of options.files) roots.push(await realpath(file));
  const queue = [...new Set(roots)];
  const byPath = new Map<string, LinuxElfInfo>();
  const resolutions: Record<string, Record<string, string>> = {};
  const paths = options.libraryPaths.map((path) => resolve(path));
  for (let index = 0; index < queue.length; index++) {
    const path = queue[index]!;
    if (byPath.has(path)) continue;
    const binary = await inspectLinuxElf(path, options.target);
    byPath.set(path, binary);
    const edges: Record<string, string> = {};
    resolutions[path] = edges;
    for (const name of binary.needed) {
      const provider = await findLibrary(name, paths);
      if (provider) {
        edges[name] = provider;
        queue.push(provider);
      }
    }
  }
  return {
    target: options.target,
    roots: [...new Set(roots)],
    binaries: [...byPath.values()],
    resolutions,
    issues: validateLinuxDependencyGraph([...byPath.values()], resolutions),
  };
};

/**
 * Validates symbol sets in a resolved graph, including a saved build baseline.
 * @param binaries - ELF metadata for all files in the graph.
 * @param resolutions - Direct library resolution edges.
 * @returns Every unresolved mandatory library, version or symbol.
 */
export const validateLinuxDependencyGraph = (
  binaries: readonly LinuxElfInfo[],
  resolutions: Readonly<Record<string, Record<string, string>>>,
): LinuxDependencyIssue[] => {
  const byPath = new Map(binaries.map((binary) => [binary.path, binary]));
  const issues: LinuxDependencyIssue[] = [];
  for (const binary of binaries) {
    for (const name of binary.needed) {
      if (!resolutions[binary.path]?.[name])
        issues.push({
          kind: "library",
          binary: binary.path,
          requirement: name,
        });
    }
  }
  for (const binary of byPath.values()) {
    const scope = new Set<string>();
    const pending = [binary.path];
    for (let index = 0; index < pending.length; index++) {
      const path = pending[index]!;
      if (scope.has(path)) continue;
      scope.add(path);
      pending.push(...Object.values(resolutions[path] ?? {}));
    }
    const definitions = new Set<string>();
    const defaultDefinitions = new Set<string>();
    for (const path of scope) {
      for (const symbol of byPath.get(path)!.exports) {
        definitions.add(`${symbol.name}@${symbol.version ?? ""}`);
        if (symbol.defaultVersion) defaultDefinitions.add(symbol.name);
      }
    }
    for (const need of binary.versionNeeds) {
      const provider = resolutions[binary.path]![need.library];
      if (
        provider &&
        !need.weak &&
        !byPath.get(provider)!.versionDefinitions.includes(need.version)
      ) {
        issues.push({
          kind: "version",
          binary: binary.path,
          requirement: `${need.library}:${need.version}`,
        });
      }
    }
    for (const symbol of binary.imports) {
      if (symbol.weak) continue;
      const available =
        symbol.version === undefined
          ? defaultDefinitions.has(symbol.name)
          : definitions.has(`${symbol.name}@${symbol.version}`);
      if (!available)
        issues.push({
          kind: "symbol",
          binary: binary.path,
          requirement: symbol.version
            ? `${symbol.name}@${symbol.version}`
            : symbol.name,
        });
    }
  }
  return issues;
};

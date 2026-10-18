// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFile } from "node:child_process";
import { createHash } from "node:crypto";
import { realpath } from "node:fs/promises";
import { basename } from "node:path";
import { promisify } from "node:util";

import type { LinuxElfSymbol } from "./linux-dependencies.js";

/** Debian metadata retained for a baseline system library. */
export interface LinuxDebianLibrary {
  /** Source binary package metadata file. */
  source: string;
  /** SHA-256 of that metadata file. */
  sha256: string;
  /** Main dependency template followed by alternative templates. */
  templates: string[];
  /** Minimum package versions of exported name@version symbols. */
  symbols: Record<string, { minimum: string; template: number }>;
  /** Library-level fallback used when no symbols information is available. */
  fallback: string | undefined;
}

/** System library selected by the target's ELF dependency audit. */
export interface LinuxDebianLibraryProvider {
  /** Resolved library file, whose owning package supplies dependency metadata. */
  path: string;
  /** Shared library name recorded in the audited ELF. */
  soname: string;
}

const execute = promisify(execFile);

const queryDpkg = async (
  directory: string,
  args: readonly string[],
): Promise<string> => {
  const { stdout } = await execute(
    "dpkg-query",
    [`--admindir=${directory}`, ...args],
    { env: { ...process.env, LC_ALL: "C" }, maxBuffer: 16 * 1024 * 1024 },
  );
  return stdout;
};

const findLibraryPackage = async (
  path: string,
  directory: string,
): Promise<string> => {
  // Search by filename, then compare canonical paths. On merged-/usr systems,
  // dpkg may still record /lib while the ELF audit has resolved it to /usr/lib.
  // Multilib packages may own the same filename under a different directory.
  const pattern = `*/${basename(path).replace(/[\\*?\[]/g, "\\$&")}`;
  let output: string;
  try {
    output = await queryDpkg(directory, ["--search", pattern]);
  } catch (error) {
    if ((error as { code: unknown }).code !== 1) throw error;
    throw new Error(`No Debian package owns ${path}`, { cause: error });
  }
  const packages = new Set<string>();
  for (const line of output.split("\n")) {
    const entry = line.match(/^(\S+(?:, \S+)*): (\/.*)$/);
    if (!entry) continue;
    let candidate: string;
    try {
      candidate = await realpath(entry[2]!);
    } catch (error) {
      if ((error as NodeJS.ErrnoException).code === "ENOENT") continue;
      throw error;
    }
    if (candidate === path)
      for (const name of entry[1]!.split(", ")) packages.add(name);
  }
  if (packages.size !== 1)
    throw new Error(
      packages.size
        ? `Ambiguous Debian package ownership for ${path}: ${[...packages].join(", ")}`
        : `No Debian package owns ${path}`,
    );
  return [...packages][0]!;
};

const readPackageLibraries = async (
  packageName: string,
  directory: string,
): Promise<Record<string, LinuxDebianLibrary>> => {
  const result: Record<string, LinuxDebianLibrary> = {};
  const files = (
    await queryDpkg(directory, ["--control-list", packageName])
  ).split("\n");
  // Read installed binary metadata, not source-package templates with patterns
  // and architecture tags. Its public format is documented in deb-symbols(5).
  // Use dpkg-query's public control-file interface instead of scanning unrelated
  // packages or depending on the internal layout of its metadata directory.
  if (files.includes("symbols")) {
    const file = `${packageName}.symbols`;
    const text = await queryDpkg(directory, [
      "--control-show",
      packageName,
      "symbols",
    ]);
    const hash = createHash("sha256").update(text).digest("hex");
    let current: LinuxDebianLibrary | undefined;
    for (const line of text.split("\n")) {
      if (!line.trim() || line.startsWith("#")) continue;
      if (line.startsWith("*")) continue;
      if (line.startsWith("|")) {
        current?.templates.push(line.slice(1).trim());
      } else if (/^\s/.test(line)) {
        if (!current) continue;
        const symbol = line.trim().match(/^(\S+) (\S+)(?: (\d+))?$/);
        if (!symbol)
          throw new Error(
            `Unsupported installed Debian symbol entry in ${file}: ${line}`,
          );
        current.symbols[symbol[1]!] = {
          minimum: symbol[2]!,
          template: Number(symbol[3] ?? 0),
        };
      } else {
        const header = line.match(/^(\S+) (.+)$/);
        if (!header)
          throw new Error(`Invalid Debian library entry in ${file}: ${line}`);
        if (result[header[1]!])
          throw new Error(
            `Ambiguous Debian symbol information for ${header[1]}`,
          );
        current = {
          source: file,
          sha256: hash,
          templates: [header[2]!],
          symbols: {},
          fallback: undefined,
        };
        result[header[1]!] = current;
      }
    }
  }
  if (files.includes("shlibs")) {
    const file = `${packageName}.shlibs`;
    const text = await queryDpkg(directory, [
      "--control-show",
      packageName,
      "shlibs",
    ]);
    for (const line of text.split("\n")) {
      if (!line.trim() || line.startsWith("#")) continue;
      const entry = line.match(/^(?:(\w+):\s+)?(\S+)\s+(\S+)\s+(.+)$/);
      if (!entry)
        throw new Error(`Invalid Debian shlibs entry in ${file}: ${line}`);
      if (entry[1] !== undefined && entry[1] !== "deb") continue;
      for (const soname of [
        `${entry[2]}.so.${entry[3]}`,
        `${entry[2]}-${entry[3]}.so`,
      ]) {
        if (result[soname]) {
          result[soname]!.fallback ??= entry[4]!;
        } else
          result[soname] = {
            source: file,
            sha256: createHash("sha256").update(text).digest("hex"),
            templates: [],
            symbols: {},
            fallback: entry[4]!,
          };
      }
    }
  }
  return result;
};

/**
 * Retains the baseline package information needed to audit later app plugins.
 * @param libraries - System library files selected by the ELF dependency audit.
 * @param directory - dpkg administrative directory, normally /var/lib/dpkg.
 * @returns Metadata from each resolved file's owning package, indexed by SONAME.
 * @remarks Rejects ambiguous file ownership or conflicting providers rather than
 * selecting another ABI's package merely because its SONAME is the same.
 */
export const readLinuxDebianLibraries = async (
  libraries: readonly LinuxDebianLibraryProvider[],
  directory: string,
): Promise<Record<string, LinuxDebianLibrary>> => {
  const result: Record<string, LinuxDebianLibrary> = {};
  const providers = new Map<string, string>();
  const packages = new Map<string, Record<string, LinuxDebianLibrary>>();
  for (const { path, soname } of libraries) {
    const canonical = await realpath(path);
    const previous = providers.get(soname);
    if (previous !== undefined) {
      if (previous !== canonical)
        throw new Error(`Conflicting Debian library providers for ${soname}`);
      continue;
    }
    providers.set(soname, canonical);
    const packageName = await findLibraryPackage(canonical, directory);
    let metadata = packages.get(packageName);
    if (metadata === undefined) {
      metadata = await readPackageLibraries(packageName, directory);
      packages.set(packageName, metadata);
    }
    const library = metadata[soname];
    if (!library)
      throw new Error(`No baseline Debian package information for ${soname}`);
    result[soname] = library;
  }
  return result;
};

/**
 * Expands package conditions for symbols used by an additional application ELF.
 * @param library - Metadata of the resolved baseline provider.
 * @param symbols - Symbols imported from that provider.
 * @returns Conjunctive dependency clauses, preserving Debian version semantics.
 */
export const getLinuxDebianSymbolDependencies = (
  library: LinuxDebianLibrary,
  symbols: readonly LinuxElfSymbol[],
): string[] => {
  const dependencies = new Set<string>();
  const addTemplate = (template: string, minimum: string): void => {
    const condition = minimum === "0" ? "" : `(>= ${minimum})`;
    for (const clause of template
      .replaceAll("#MINVER#", condition)
      .split(/,\s*/))
      dependencies.add(clause.trim());
  };
  if (!library.templates.length) {
    if (!library.fallback)
      throw new Error(`Incomplete Debian package metadata: ${library.source}`);
    return library.fallback.split(/,\s*/);
  }
  // A DT_NEEDED library must exist even if no mandatory symbol is imported.
  addTemplate(library.templates[0]!, "0");
  for (const symbol of symbols) {
    if (symbol.weak) continue;
    const requirement =
      library.symbols[`${symbol.name}@${symbol.version ?? "Base"}`];
    if (!requirement) {
      if (!library.fallback)
        throw new Error(
          `No Debian package condition for ${symbol.name}@${symbol.version ?? "Base"} in ${library.source}`,
        );
      for (const clause of library.fallback.split(/,\s*/))
        dependencies.add(clause);
      continue;
    }
    addTemplate(library.templates[0]!, requirement.minimum);
    if (requirement.template) {
      const template = library.templates[requirement.template];
      if (!template)
        throw new Error(
          `Missing Debian dependency template ${requirement.template} in ${library.source}`,
        );
      addTemplate(template, requirement.minimum);
    }
  }
  return [...dependencies].sort();
};

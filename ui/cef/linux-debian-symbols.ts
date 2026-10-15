// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { createHash } from "node:crypto";
import { readdir, readFile } from "node:fs/promises";
import { join } from "node:path";

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

/**
 * Retains the baseline package information needed to audit later app plugins.
 * @param sonames - System libraries in the audited dependency graph.
 * @param architecture - Debian architecture of that graph.
 * @param directory - Installed dpkg metadata directory.
 * @returns Package metadata indexed by SONAME.
 */
export const readLinuxDebianLibraries = async (
  sonames: readonly string[],
  architecture: string,
  directory: string,
): Promise<Record<string, LinuxDebianLibrary>> => {
  const wanted = new Set(sonames);
  const result: Record<string, LinuxDebianLibrary> = {};
  const files = (await readdir(directory))
    .filter((name) => {
      const match = name.match(/^(.*?)(?::([^:]+))?\.(symbols|shlibs)$/);
      return match && (match[2] === undefined || match[2] === architecture);
    })
    .sort();
  // Read installed binary metadata, not source-package templates with patterns
  // and architecture tags. Its public format is documented in deb-symbols(5).
  for (const file of files.filter((name) => name.endsWith(".symbols"))) {
    const text = await readFile(join(directory, file), "utf8");
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
        current = undefined;
        if (!wanted.has(header[1]!)) continue;
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
  for (const file of files.filter((name) => name.endsWith(".shlibs"))) {
    const text = await readFile(join(directory, file), "utf8");
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
        if (!wanted.has(soname)) continue;
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
  for (const soname of wanted) {
    if (!result[soname])
      throw new Error(`No baseline Debian package information for ${soname}`);
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

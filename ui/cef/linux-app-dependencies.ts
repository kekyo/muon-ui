// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import type { LinuxDependencyManifest } from "./linux-dependency-manifest.js";
import {
  inspectLinuxElf,
  validateLinuxDependencyGraph,
  type LinuxElfInfo,
  type LinuxElfSymbol,
} from "./linux-dependencies.js";
import { getLinuxDebianSymbolDependencies } from "./linux-debian-symbols.js";

const isPrivate = (path: string): boolean =>
  /^(cef|runtime|native)\//.test(path);
const provides = (binary: LinuxElfInfo, required: LinuxElfSymbol): boolean =>
  binary.exports.some(
    (symbol) =>
      symbol.name === required.name &&
      (required.version === undefined
        ? symbol.defaultVersion
        : symbol.version === required.version),
  );

/**
 * Combines an application's native requirements with the saved build baseline.
 * @param manifest - Dependency metadata distributed with the target runtime.
 * @param files - Additional application ELF files, including native plugins.
 * @returns Debian dependency clauses for the complete application.
 */
export const getLinuxAppDependencies = async (
  manifest: LinuxDependencyManifest,
  files: readonly string[],
): Promise<string[]> => {
  if (
    manifest.schemaVersion !== 1 ||
    !manifest.debian ||
    manifest.audit.issues.length
  )
    throw new Error(
      "A successful Linux build dependency manifest is required.",
    );
  const binaries = new Map(
    manifest.audit.binaries.map((binary) => [binary.path, binary]),
  );
  const bySoname = new Map(
    manifest.audit.binaries.flatMap((binary) =>
      binary.soname ? [[binary.soname, binary] as const] : [],
    ),
  );
  const hashes = new Set(
    manifest.audit.binaries.map((binary) => binary.sha256),
  );
  const added: LinuxElfInfo[] = [];
  for (const file of files) {
    const binary = await inspectLinuxElf(file, manifest.audit.target);
    if (hashes.has(binary.sha256)) continue;
    if (binary.soname && bySoname.has(binary.soname))
      throw new Error(
        `Application library conflicts with the build baseline: ${file} (${binary.soname})`,
      );
    binaries.set(binary.path, binary);
    if (binary.soname) bySoname.set(binary.soname, binary);
    hashes.add(binary.sha256);
    added.push(binary);
  }
  const resolutions = { ...manifest.audit.resolutions };
  for (const binary of added) {
    resolutions[binary.path] = Object.fromEntries(
      binary.needed.flatMap((name) => {
        const provider = bySoname.get(name);
        return provider ? [[name, provider.path]] : [];
      }),
    );
  }
  const issues = validateLinuxDependencyGraph(
    [...binaries.values()],
    resolutions,
  );
  if (issues.length)
    throw new Error(
      `Application exceeds the Linux build baseline:\n${issues
        .map(
          (issue) =>
            `${issue.binary}: missing ${issue.kind} ${issue.requirement}`,
        )
        .join("\n")}`,
    );
  const privatePaths = new Set(added.map((binary) => binary.path));
  const dependencies = new Set([
    ...manifest.debian.depends,
    ...manifest.runtimePackages,
  ]);
  for (const binary of added) {
    const scope = new Set<string>();
    const pending = [binary.path];
    for (let index = 0; index < pending.length; index++) {
      const path = pending[index]!;
      if (scope.has(path)) continue;
      scope.add(path);
      pending.push(...Object.values(resolutions[path] ?? {}));
    }
    for (const path of scope) {
      if (isPrivate(path) || privatePaths.has(path)) continue;
      const provider = binaries.get(path)!;
      const required = binary.imports.filter(
        (symbol) => !symbol.weak && provides(provider, symbol),
      );
      // System packages manage their own implementation dependencies. Keep
      // only libraries this app links to or actually imports symbols from.
      if (
        required.length === 0 &&
        !binary.needed.includes(provider.soname ?? "")
      )
        continue;
      const metadata =
        provider.soname && manifest.debianLibraries[provider.soname];
      if (!metadata)
        throw new Error(`Missing baseline package information for ${path}`);
      for (const dependency of getLinuxDebianSymbolDependencies(
        metadata,
        required,
      ))
        dependencies.add(dependency);
    }
  }
  return [...dependencies].sort();
};

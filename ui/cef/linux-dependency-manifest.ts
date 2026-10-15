// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFile } from "node:child_process";
import { open, readdir, readFile, realpath } from "node:fs/promises";
import { dirname, join, relative, resolve } from "node:path";
import { promisify } from "node:util";

import {
  auditLinuxDependencies,
  type LinuxDependencyAudit,
  type LinuxDependencyTarget,
} from "./linux-dependencies.js";
import {
  calculateLinuxDebianDependencies,
  type LinuxDebianDependencies,
} from "./linux-debian-dependencies.js";
import {
  readLinuxDebianLibraries,
  type LinuxDebianLibrary,
} from "./linux-debian-symbols.js";

/** CEF provenance taken from the native build, never from a runtime policy. */
export interface LinuxReferenceCef {
  /** Exact CEF build reference. */
  version: string;
  /** Name of the verified CEF distribution archive. */
  archive: string;
  /** Upstream SHA-1 already verified by the native build. */
  sha1: string;
  /** Upstream download URL. */
  url: string;
}

/** Inputs to a reproducible dependency report. */
export interface LinuxDependencyManifestOptions {
  /** Target CPU, matching the baseline environment. */
  target: LinuxDependencyTarget;
  /** Reference extracted from the native build's CMake cache. */
  referenceCef: LinuxReferenceCef;
  /** Release directory of that CEF reference. */
  cefDirectory: string;
  /** muon runtime payload directory. */
  runtimeDirectory: string;
  /** Additional launcher/builder payload, if separate. */
  nativeDirectory: string | undefined;
  /** Ordered library directories of the baseline, with no host fallback. */
  libraryPaths: readonly string[];
}

/** Build-time dependency metadata carried with a Linux runtime. */
export interface LinuxDependencyManifest {
  /** Format version. */
  schemaVersion: 1;
  /** Runtime tools/data required outside ELF linkage, as Debian package names. */
  runtimePackages: string[];
  /** Exact build-time CEF reference. */
  referenceCef: LinuxReferenceCef;
  /** Baseline provenance used for ABI and Debian package checks. */
  baseline: {
    /** Contents of the baseline's os-release. */
    osRelease: string;
    /** Installed binary package names and versions. */
    packages: string[];
    /** GNU readelf version. */
    readelf: string;
  };
  /** ELF dependency graph; private files use cef/, runtime/ or native/ paths. */
  audit: LinuxDependencyAudit;
  /** Calculated Debian constraints, absent when the ABI audit fails. */
  debian: LinuxDebianDependencies | undefined;
  /** Baseline system package symbols used when an app adds native plugins. */
  debianLibraries: Record<string, LinuxDebianLibrary>;
}

const execute = promisify(execFile);

/**
 * Reads the immutable CEF reference recorded by a native target build.
 * @param path - CMakeCache.txt of the matching native build.
 * @param target - Target whose reference is required.
 * @returns Version and archive provenance, rejecting a mismatched build.
 */
export const readLinuxReferenceCef = async (
  path: string,
  target: LinuxDependencyTarget,
): Promise<LinuxReferenceCef> => {
  const cache = new Map<string, string>();
  for (const line of (await readFile(path, "utf8")).split("\n")) {
    const entry = line.match(/^(MUON_[A-Z0-9_]+):[^=]+=(.*)$/);
    if (entry) cache.set(entry[1]!, entry[2]!);
  }
  if (cache.get("MUON_TARGET_NAME") !== target) {
    throw new Error(`CEF build target does not match ${target}: ${path}`);
  }
  const requireValue = (key: string): string => {
    const value = cache.get(key);
    if (!value)
      throw new Error(`Missing ${key} in native build cache: ${path}`);
    return value;
  };
  const sha1 = requireValue("MUON_CEF_SHA1");
  if (!/^[a-f0-9]{40}$/i.test(sha1))
    throw new Error(`Invalid CEF archive SHA-1 in ${path}`);
  return {
    version: requireValue("MUON_CEF_VERSION"),
    archive: requireValue("MUON_CEF_PACKAGE"),
    sha1,
    url: requireValue("MUON_CEF_PACKAGE_URL"),
  };
};

/**
 * Finds ELF inputs under a native build output directory without executing them.
 * @param directory - CEF, runtime or launcher output directory.
 * @returns Linked executables and shared libraries, excluding intermediate objects.
 */
export const collectLinuxElfFiles = async (
  directory: string,
): Promise<string[]> => {
  const files: string[] = [];
  for (const entry of (await readdir(directory, { withFileTypes: true })).sort(
    (left, right) => left.name.localeCompare(right.name, "en"),
  )) {
    const path = join(directory, entry.name);
    if (entry.isDirectory()) files.push(...(await collectLinuxElfFiles(path)));
    else if (entry.isFile() || entry.isSymbolicLink()) {
      const handle = await open(path, "r");
      try {
        if (!(await handle.stat()).isFile()) continue;
        const header = Buffer.alloc(20);
        const { bytesRead } = await handle.read(header, 0, header.length, 0);
        if (
          bytesRead === header.length &&
          header.readUInt32LE(0) === 0x464c457f
        ) {
          const type =
            header[5] === 2 ? header.readUInt16BE(16) : header.readUInt16LE(16);
          if (type === 2 || type === 3) files.push(path);
        }
      } finally {
        await handle.close();
      }
    }
  }
  return files;
};

/**
 * Analyzes the build reference and private payload, and calculates OS requirements.
 * @param options - Baseline and build outputs; runtime selection is not an input.
 * @returns A report including any unresolved requirements, with relocatable private paths.
 */
export const createLinuxDependencyManifest = async (
  options: LinuxDependencyManifestOptions,
): Promise<LinuxDependencyManifest> => {
  const directories: [string, string][] = [
    ["cef", resolve(options.cefDirectory)],
    ["runtime", resolve(options.runtimeDirectory)],
  ];
  if (options.nativeDirectory !== undefined)
    directories.push(["native", resolve(options.nativeDirectory)]);
  const files: string[] = [];
  const labels = new Map<string, string>();
  for (const [kind, directory] of directories) {
    for (const path of await collectLinuxElfFiles(directory)) {
      const canonical = await realpath(path);
      if (!labels.has(canonical)) {
        files.push(canonical);
        labels.set(canonical, `${kind}/${relative(directory, path)}`);
      }
    }
  }
  if (!files.some((path) => labels.get(path)!.startsWith("cef/"))) {
    throw new Error(
      `CEF reference contains no ELF files: ${options.cefDirectory}`,
    );
  }
  const libraryPaths = [
    ...new Set([
      ...files.map((path) => dirname(path)),
      ...options.libraryPaths,
    ]),
  ];
  const audit = await auditLinuxDependencies({
    target: options.target,
    files,
    libraryPaths,
  });
  const debian = audit.issues.length
    ? undefined
    : await calculateLinuxDebianDependencies({
        audit,
        privateFiles: files,
        libraryPaths,
      });
  const { stdout: packageVersions } = await execute(
    "dpkg-query",
    ["-W", "-f=${binary:Package}\t${Version}\n"],
    { maxBuffer: 16 * 1024 * 1024, env: { ...process.env, LC_ALL: "C" } },
  );
  const { stdout: readelf } = await execute("readelf", ["--version"], {
    env: { ...process.env, LC_ALL: "C" },
  });
  const label = (path: string): string => labels.get(path) ?? path;
  const systemLibraries = audit.binaries
    .filter((binary) => !labels.has(binary.path) && binary.soname !== undefined)
    .map((binary) => binary.soname!);
  return {
    schemaVersion: 1,
    // CEF/Node acquisition invokes curl and relies on the host trust store.
    // Neither requirement can be discovered from the muon ELF dependency graph.
    runtimePackages: ["ca-certificates", "curl"],
    debianLibraries: await readLinuxDebianLibraries(
      systemLibraries,
      options.target.slice("linux-".length),
      "/var/lib/dpkg/info",
    ),
    referenceCef: options.referenceCef,
    baseline: {
      osRelease: await readFile("/etc/os-release", "utf8"),
      packages: packageVersions.trim().split("\n").sort(),
      readelf: readelf.split("\n")[0]!,
    },
    audit: {
      ...audit,
      roots: audit.roots.map(label),
      binaries: audit.binaries.map((binary) => ({
        ...binary,
        path: label(binary.path),
      })),
      resolutions: Object.fromEntries(
        Object.entries(audit.resolutions).map(([path, dependencies]) => [
          label(path),
          Object.fromEntries(
            Object.entries(dependencies).map(([name, provider]) => [
              name,
              label(provider),
            ]),
          ),
        ]),
      ),
      issues: audit.issues.map((issue) => ({
        ...issue,
        binary: label(issue.binary),
      })),
    },
    debian: debian && {
      ...debian,
      diagnostics: debian.diagnostics.map((line) => {
        for (const [path, name] of [...labels].sort(
          (left, right) => right[0].length - left[0].length,
        ))
          line = line.replaceAll(path, name);
        return line;
      }),
    },
  };
};

// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { copyFile, mkdir } from "node:fs/promises";
import { join } from "node:path";

import type {
  LinuxDependencyAudit,
  LinuxElfInfo,
} from "./linux-dependencies.js";

// These implement the host platform ABI or communicate with its graphics/udev
// stack. They must never be collected merely because GTK depends on them.
const hostLibrary =
  /^(?:ld(?:-linux[^/]*|-[^/]+)\.so(?:\.|$)|lib(?:c|m|pthread|dl|rt|util|anl|resolv|nss_[^/]+|gcc_s|stdc\+\+|GL|GLX|GLdispatch|EGL|GLESv[0-9]+|gbm|drm(?:_[^/]+)?|vulkan|udev|systemd)\.so(?:\.|$))/;

/** Libraries that share an ABI identity, and distinct ABI versions kept privately. */
export interface LinuxBundlePolicy {
  /** Include these libraries and their users, such as GLib and GTK. */
  coherent: readonly string[];
  /** Include only these distinct SONAMEs, such as libffi.so.7 beside host libffi.so.8. */
  independent: readonly string[];
}

/**
 * Selects libraries that must use a coherent baseline implementation.
 * @param audit - Successful dependency graph of CEF and the native payload.
 * @param privateFiles - Canonical paths already distributed with the payload.
 * @param policy - SONAMEs to distribute together or as independent ABI versions.
 * @returns Libraries to distribute, excluding the platform C library and drivers.
 * @remarks Includes users of the seed libraries to prevent mixing old GLib with
 * newer host GTK libraries. Other dependencies remain audited host requirements;
 * the C library, loader and graphics drivers are never bundled.
 */
export const selectLinuxBundledLibraries = (
  audit: LinuxDependencyAudit,
  privateFiles: readonly string[],
  policy: LinuxBundlePolicy,
): LinuxElfInfo[] => {
  if (audit.issues.length)
    throw new Error("Cannot bundle libraries with unresolved requirements.");
  const privatePaths = new Set(privateFiles);
  const byPath = new Map(audit.binaries.map((binary) => [binary.path, binary]));
  const selected = new Set<string>();
  for (const binary of audit.binaries) {
    if (
      binary.soname &&
      policy.coherent.includes(binary.soname) &&
      !privatePaths.has(binary.path)
    ) {
      if (hostLibrary.test(binary.soname))
        throw new Error(
          `Cannot bundle host platform library: ${binary.soname}`,
        );
      selected.add(binary.path);
    }
  }
  // Select reverse dependencies: libraries such as
  // host GTK/pixbuf may require newer GLib entry points than our private copy.
  let changed = true;
  while (changed) {
    changed = false;
    for (const binary of audit.binaries) {
      if (selected.has(binary.path) || privatePaths.has(binary.path)) continue;
      if (
        !Object.values(audit.resolutions[binary.path] ?? {}).some((path) =>
          selected.has(path),
        )
      )
        continue;
      if (!binary.soname || hostLibrary.test(binary.soname))
        throw new Error(
          `Host platform library depends on the private library group: ${binary.path}`,
        );
      selected.add(binary.path);
      changed = true;
    }
  }
  for (const binary of audit.binaries) {
    if (
      binary.soname &&
      policy.independent.includes(binary.soname) &&
      !privatePaths.has(binary.path)
    ) {
      if (hostLibrary.test(binary.soname))
        throw new Error(
          `Cannot bundle host platform library: ${binary.soname}`,
        );
      selected.add(binary.path);
    }
  }
  return [...selected]
    .map((path) => byPath.get(path)!)
    .sort((left, right) => left.soname!.localeCompare(right.soname!, "en"));
};

/**
 * Copies selected libraries under their SONAMEs into a private loader directory.
 * @param libraries - Libraries selected from the audited baseline.
 * @param directory - Destination directory used by the application's RPATH.
 */
export const stageLinuxBundledLibraries = async (
  libraries: readonly LinuxElfInfo[],
  directory: string,
): Promise<void> => {
  await mkdir(directory, { recursive: true });
  const names = new Map<string, string>();
  for (const binary of libraries) {
    const name = binary.soname;
    if (!name || name.includes("/") || hostLibrary.test(name))
      throw new Error(`Cannot stage private library: ${binary.path}`);
    const previous = names.get(name);
    if (previous && previous !== binary.sha256)
      throw new Error(`Conflicting private library: ${name}`);
    names.set(name, binary.sha256);
    await copyFile(binary.path, join(directory, name));
  }
};

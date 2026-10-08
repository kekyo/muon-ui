// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { readFile } from "node:fs/promises";
import { resolve } from "node:path";
import type { MuonAndroidAbi, MuonAndroidPackagedPlugin } from "./build.js";

const runtimeLibraries = new Set([
  "libcardio.so",
  "libc++_shared.so",
  "libmuon_android_rpc.so",
]);
const systemLibraries = new Set([
  "libc.so",
  "libm.so",
  "libdl.so",
  "liblog.so",
  "libandroid.so",
  "libjnigraphics.so",
  "libz.so",
  "libEGL.so",
  "libGLESv2.so",
  "libGLESv3.so",
  "libOpenSLES.so",
  "libvulkan.so",
]);
const record = (value: unknown, label: string): Record<string, unknown> => {
  if (value === null || typeof value !== "object" || Array.isArray(value))
    throw new Error(`${label} must be an object.`);
  return value as Record<string, unknown>;
};
const text = (value: unknown, label: string): string => {
  if (typeof value !== "string" || !value || value.includes("\0"))
    throw new Error(`${label} must be a nonempty string without NUL.`);
  return value;
};

// ELF64 little-endian is the shared format of the two supported Android ABIs.
// Validate the actual load segments and dynamic tables without requiring an NDK.
const inspectLibrary = (
  data: Buffer,
  abi: MuonAndroidAbi,
  soname: string,
  packaged: ReadonlySet<string>,
): void => {
  if (
    data.length < 64 ||
    data.readUInt32BE(0) !== 0x7f454c46 ||
    data[4] !== 2 ||
    data[5] !== 1 ||
    data.readUInt16LE(16) !== 3
  )
    throw new Error(`${soname} is not an ELF64 little-endian shared library.`);
  if (data.readUInt16LE(18) !== (abi === "arm64-v8a" ? 183 : 62))
    throw new Error(`${soname} ELF machine does not match ${abi}.`);
  const u64 = (offset: number): number => {
    const value = data.readBigUInt64LE(offset);
    if (value > BigInt(Number.MAX_SAFE_INTEGER))
      throw new Error(`${soname} ELF offset is too large.`);
    return Number(value);
  };
  const phoff = u64(32);
  const phsize = data.readUInt16LE(54);
  const phnum = data.readUInt16LE(56);
  let loads = 0;
  for (let index = 0; index < phnum; index += 1) {
    const offset = phoff + index * phsize;
    if (data.readUInt32LE(offset) !== 1) continue;
    loads += 1;
    if (
      u64(offset + 48) < 16384 ||
      u64(offset + 8) % 16384 !== u64(offset + 16) % 16384
    )
      throw new Error(`${soname} ELF load segments must support 16 KiB pages.`);
  }
  if (!loads) throw new Error(`${soname} ELF has no load segments.`);
  const shoff = u64(40);
  const shsize = data.readUInt16LE(58);
  const shnum = data.readUInt16LE(60);
  const sections = Array.from({ length: shnum }, (_, index) => {
    const offset = shoff + index * shsize;
    return {
      type: data.readUInt32LE(offset + 4),
      offset: u64(offset + 24),
      size: u64(offset + 32),
      link: data.readUInt32LE(offset + 40),
      entrySize: u64(offset + 56),
    };
  });
  const cstring = (
    table: (typeof sections)[number] | undefined,
    offset: number,
  ): string => {
    if (!table || offset < 0 || offset >= table.size)
      throw new Error(`${soname} ELF string table is invalid.`);
    const start = table.offset + offset;
    const end = data.indexOf(0, start);
    if (end < start || end >= table.offset + table.size)
      throw new Error(`${soname} ELF string is unterminated.`);
    return data.toString("utf8", start, end);
  };
  let entrypoint = false;
  let declaredSoname: string | undefined;
  for (const section of sections) {
    if (section.type !== 11 && section.type !== 6) continue;
    const expectedSize = section.type === 11 ? 24 : 16;
    if (
      section.entrySize !== expectedSize ||
      section.size % expectedSize !== 0 ||
      section.offset + section.size > data.length
    )
      throw new Error(`${soname} ELF dynamic table is invalid.`);
    for (
      let offset = section.offset;
      offset < section.offset + section.size;
      offset += expectedSize
    ) {
      if (section.type === 11) {
        const name = cstring(sections[section.link], data.readUInt32LE(offset));
        const binding = data[offset + 4]! >> 4;
        if (
          name === "muon_init_plugin" &&
          data.readUInt16LE(offset + 6) !== 0 &&
          (binding === 1 || binding === 2) &&
          (data[offset + 5]! & 3) === 0
        )
          entrypoint = true;
      } else {
        const tag = u64(offset);
        if (tag === 1 || tag === 14) {
          const name = cstring(sections[section.link], u64(offset + 8));
          if (tag === 14) declaredSoname = name;
          else if (
            !runtimeLibraries.has(name) &&
            !systemLibraries.has(name) &&
            !packaged.has(name)
          )
            throw new Error(
              `${soname} requires an unpackaged library: ${name}`,
            );
        }
      }
    }
  }
  if (!entrypoint) throw new Error(`${soname} must export muon_init_plugin.`);
  if (declaredSoname !== soname)
    throw new Error(`${soname} ELF SONAME must match its packaged name.`);
};

/**
 * Resolves application-owned prebuilt plugins and validates their Android ELF inputs.
 * @param input - Parsed android.plugins value.
 * @param abis - Selected APK ABIs.
 * @param directory - Base directory of the configuration supplying the plugins.
 * @returns Runtime registry entries with absolute library paths.
 */
export const resolveAndroidPlugins = async (
  input: unknown,
  abis: readonly MuonAndroidAbi[],
  directory: string,
): Promise<MuonAndroidPackagedPlugin[]> => {
  if (!Array.isArray(input))
    throw new Error("android.plugins must be an array.");
  const names = new Set<string>();
  const sonames = new Set<string>();
  const result = input.map((value) => {
    const plugin = record(value, "android.plugins entry");
    const name = text(plugin.name, "plugin.name");
    const soname = text(plugin.soname, "plugin.soname");
    if (
      !/^[A-Za-z_][A-Za-z0-9_]*$/u.test(name) ||
      name === "internal" ||
      names.has(name)
    )
      throw new Error("Android plugin names must be valid and unique.");
    if (!/^lib[A-Za-z0-9_]+\.so$/u.test(soname) || sonames.has(soname))
      throw new Error("Android plugin sonames must be valid and unique.");
    if (runtimeLibraries.has(soname) || systemLibraries.has(soname))
      throw new Error(`Android library name is reserved: ${soname}`);
    names.add(name);
    sonames.add(soname);
    if (!Array.isArray(plugin.allow) || !plugin.allow.length)
      throw new Error(
        `Android plugin ${name} requires a nonempty allow policy.`,
      );
    const allow = plugin.allow.map((value) => text(value, "plugin.allow"));
    if (allow.some((value) => !/^[A-Za-z0-9_.$*]+$/u.test(value)))
      throw new Error(
        "Android plugin.allow contains an invalid function pattern.",
      );
    const rawLibraries = record(plugin.libraries, "plugin.libraries");
    const libraries: Partial<Record<MuonAndroidAbi, string>> = {};
    for (const abi of abis)
      libraries[abi] = resolve(
        directory,
        text(rawLibraries[abi], `plugin.libraries.${abi}`),
      );
    const config = Object.entries(
      record(plugin.config ?? {}, "plugin.config"),
    ).map(([key, value]) => {
      if (
        !key ||
        key.includes("\0") ||
        typeof value !== "string" ||
        value.includes("\0")
      )
        throw new Error(
          "Android plugin.config must contain string keys and values without NUL.",
        );
      return { key, value };
    });
    if (
      plugin.expectedFunctions !== undefined &&
      (!Array.isArray(plugin.expectedFunctions) ||
        !plugin.expectedFunctions.every((path) => typeof path === "string"))
    )
      throw new Error(`Invalid expected functions for Android plugin ${name}.`);
    return {
      name,
      soname,
      allow,
      libraries,
      config,
      ...(plugin.expectedFunctions === undefined
        ? {}
        : { expectedFunctions: plugin.expectedFunctions as string[] }),
    };
  });
  for (const plugin of result)
    for (const abi of abis) {
      try {
        inspectLibrary(
          await readFile(plugin.libraries[abi]!),
          abi,
          plugin.soname,
          sonames,
        );
      } catch (error) {
        throw new Error(
          `Invalid Android plugin ${plugin.name} (${abi}): ${error instanceof Error ? error.message : String(error)}`,
        );
      }
    }
  return result;
};

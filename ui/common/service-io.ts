// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { readFile, realpath } from "node:fs/promises";
import { isAbsolute, relative, resolve, sep, win32 } from "node:path";
import type { MuonServiceInputObserver } from "./service-inputs.ts";

/** Normalized per-service grants and the contents of explicitly packaged files. */
export interface MuonServiceIoPolicy {
  /** Exact allowed HTTP and HTTPS origins. */ readonly http?: {
    readonly origins: readonly string[];
  };
  /** File areas available through the portable SDK. */
  readonly fs?: {
    /** Private directory access. */ readonly data?: "read" | "read-write";
    /** Immutable binary assets indexed by logical name, encoded as Base64. */ readonly assets?: Readonly<
      Record<string, string>
    >;
    /** Additional host directories and their access level. */ readonly directories?: Readonly<
      Record<
        string,
        { readonly path: string; readonly access: "read" | "read-write" }
      >
    >;
  };
}

const object = (
  input: unknown,
  label: string,
  keys: string[] | undefined,
): Record<string, unknown> => {
  if (!input || typeof input !== "object" || Array.isArray(input))
    throw new Error(label + " must be an object");
  if (keys && Object.keys(input).some((key) => !keys.includes(key)))
    throw new Error(label + " contains an unknown grant");
  return input as Record<string, unknown>;
};
const access = (value: unknown, label: string) => {
  if (value !== "read" && value !== "read-write")
    throw new Error(label + " requires read or read-write");
  return value;
};
const safeName = (name: string) =>
  /^[A-Za-z0-9_-][A-Za-z0-9_.-]*$/u.test(name) &&
  !/[ .]$/u.test(name) &&
  !/^(con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\.|$)/iu.test(name);

/** @param input Service I/O declaration. @param directory Service project root.
 * @param observe Development input observer. @param label Configuration key for diagnostics.
 * @returns Validated host grants without source-project paths for bundled assets. */
export const resolveServiceIo = async (
  input: unknown,
  directory: string,
  observe: MuonServiceInputObserver | undefined,
  label: string,
): Promise<MuonServiceIoPolicy | undefined> => {
  if (input === undefined) return undefined;
  const io = object(input, label, ["fs", "http"]);
  let http: MuonServiceIoPolicy["http"];
  if (io.http !== undefined) {
    const { origins } = object(io.http, label + ".http", ["origins"]);
    if (!Array.isArray(origins) || origins.length > 128)
      throw new Error(label + ".http.origins must list at most 128 origins");
    const normalized = origins.map((value) => {
      if (
        typeof value !== "string" ||
        !/^https?:\/\/[^/?#@*\\\s]+\/?$/iu.test(value)
      )
        throw new Error(label + ": invalid HTTP origin");
      const url = new URL(value);
      return url.origin;
    });
    http = { origins: [...new Set(normalized)] };
  }
  const network = http ? { http } : {};
  if (io.fs === undefined) return network;
  const fs = object(io.fs, label + ".fs", ["data", "assets", "directories"]);
  const directories: Record<
    string,
    { path: string; access: "read" | "read-write" }
  > = Object.create(null);
  for (const [area, grant] of Object.entries(
    object(fs.directories ?? {}, label + ".fs.directories", undefined),
  )) {
    const value = object(grant, label + "." + area, ["path", "access"]);
    if (
      !safeName(area) ||
      ["data", "assets"].includes(area) ||
      typeof value.path !== "string" ||
      value.path.includes("\0") ||
      !(isAbsolute(value.path) || win32.isAbsolute(value.path))
    )
      throw new Error(label + ": invalid additional directory " + area);
    directories[area] = {
      path: value.path,
      access: access(value.access, label),
    };
  }
  const assets: Record<string, string> = Object.create(null);
  const root = await realpath(directory);
  for (const [name, source] of Object.entries(
    object(fs.assets ?? {}, label + ".fs.assets", undefined),
  )) {
    if (
      !name.split("/").every(safeName) ||
      typeof source !== "string" ||
      !source ||
      isAbsolute(source) ||
      win32.isAbsolute(source)
    )
      throw new Error(label + ": invalid service asset " + name);
    const filename = resolve(root, source);
    const inside = (file: string) => {
      const path = relative(root, file);
      if (
        !path ||
        path === ".." ||
        path.startsWith(".." + sep) ||
        isAbsolute(path)
      )
        throw new Error(label + ": asset escapes project: " + name);
    };
    inside(filename);
    observe?.({ path: filename, kind: "file" });
    const actual = await realpath(filename);
    inside(actual);
    if (actual !== filename) observe?.({ path: actual, kind: "file" });
    const bytes = await readFile(actual);
    if (bytes.length > 1024 * 1024)
      throw new Error(label + ": asset exceeds 1 MiB: " + name);
    assets[name] = bytes.toString("base64");
  }
  return {
    ...network,
    fs: {
      ...(fs.data === undefined ? {} : { data: access(fs.data, label) }),
      ...(fs.assets === undefined ? {} : { assets }),
      ...(fs.directories === undefined ? {} : { directories }),
    },
  };
};

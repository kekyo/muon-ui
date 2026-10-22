// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFile } from "node:child_process";
import { createHash } from "node:crypto";
import { mkdir, readFile, writeFile } from "node:fs/promises";
import { basename, isAbsolute, join, relative } from "node:path";
import { promisify } from "node:util";

import type { LinuxElfInfo } from "./linux-dependencies.js";
import type { LinuxBundledSourceFile } from "./linux-baseline-modules.js";

/** Upstream source built in the baseline instead of installed from apt. */
export interface LinuxBaselineSource {
  /** Upstream project name. */
  name: string;
  /** Source release version. */
  version: string;
  /** Private installation prefix inside the build container. */
  prefix: string;
  /** Source archive URL. */
  url: string;
  /** Verified SHA-256 of that source archive. */
  sha256: string;
  /** Copyright/license text installed by the baseline build. */
  license: string;
}

/** Provenance and redistribution notice for a bundled shared library. */
export interface LinuxBundledLibrary {
  /** File relative to the runtime payload. */
  file: string;
  /** SHA-256 of the shared library. */
  sha256: string;
  /** Upstream source or distribution source-package name. */
  source: string;
  /** Source version, including distribution revision when present. */
  version: string;
  /** Source archive location or distribution repository. */
  url: string;
  /** Verified source archive hash when built from an upstream archive. */
  sourceSha256: string | undefined;
  /** Copyright/license file relative to the payload. */
  license: string;
}

const execute = promisify(execFile);

/**
 * Reads notices and source records for private libraries already staged in a payload.
 * @param directory - Native runtime payload directory.
 * @returns Records, or an empty list for a build without a private library group.
 */
export const readLinuxBundleProvenance = async (
  directory: string,
): Promise<LinuxBundledLibrary[]> => {
  let text: string;
  try {
    text = await readFile(join(directory, "linux-bundle.json"), "utf8");
  } catch (error) {
    if ((error as NodeJS.ErrnoException).code === "ENOENT") return [];
    throw error;
  }
  const records = JSON.parse(text) as LinuxBundledLibrary[];
  if (
    !Array.isArray(records) ||
    records.some(
      (record) =>
        typeof record.file !== "string" ||
        record.file.includes("\\") ||
        record.file
          .split("/")
          .some((part) => part === "" || part === "." || part === "..") ||
        !(
          (basename(record.file) === record.file &&
            /^lib[^/]+\.so(?:\.[^/]+)*$/.test(record.file)) ||
          /^linux\/lib\/(?:gtk-3\.0|gdk-pixbuf-2\.0|gio)\/.+\.so(?:\.[^/]+)*$/.test(
            record.file,
          ) ||
          record.file === "linux/lib/gio/modules/giomodule.cache" ||
          record.file === "linux/share/glib-2.0/schemas/gschemas.compiled" ||
          record.file === "linux/share/mime/mime.cache" ||
          record.file === "linux/share/icons/hicolor/index.theme" ||
          /^linux\/(?:gtk-immodules|gdk-pixbuf-loaders)\.cache\.in$/.test(
            record.file,
          )
        ),
    )
  )
    throw new Error(`Invalid private Linux library records in ${directory}`);
  return records;
};

/**
 * Records the origin and notices of every library selected for distribution.
 * @param libraries - Selected and staged baseline libraries.
 * @param runtimeDirectory - Payload containing the private libraries.
 * @param sources - Verified upstream builds installed outside dpkg.
 * @param extraFiles - Dynamic modules and cache templates already staged.
 * @returns Relocatable provenance entries, also saved with the runtime payload.
 */
export const recordLinuxBundleProvenance = async (
  libraries: readonly LinuxElfInfo[],
  runtimeDirectory: string,
  sources: readonly LinuxBaselineSource[],
  extraFiles: readonly LinuxBundledSourceFile[],
): Promise<LinuxBundledLibrary[]> => {
  const notices = join(runtimeDirectory, "licenses/linux");
  await mkdir(notices, { recursive: true });
  const environment = { ...process.env, LC_ALL: "C" };
  const records: LinuxBundledLibrary[] = [];
  const files = [
    ...libraries.map((binary) => ({ path: binary.path, file: binary.soname! })),
    ...extraFiles,
  ];
  for (const binary of files) {
    const upstream = sources.find((source) => {
      const path = relative(source.prefix, binary.path);
      return !isAbsolute(path) && path !== ".." && !path.startsWith("../");
    });
    let source: string;
    let version: string;
    let url: string;
    let licensePath: string;
    if (upstream) {
      source = upstream.name;
      version = upstream.version;
      url = upstream.url;
      licensePath = upstream.license;
    } else {
      let owner: string | undefined;
      // usr-merged systems retain pre-merge paths in the dpkg database.
      const paths = [binary.path];
      if (binary.path.startsWith("/usr/lib/")) paths.push(binary.path.slice(4));
      for (const path of paths) {
        try {
          const { stdout } = await execute("dpkg-query", ["-S", path], {
            env: environment,
          });
          owner = stdout
            .split("\n")
            .find((line) => line.endsWith(`: ${path}`))
            ?.split(": ")[0];
          if (owner) break;
        } catch (error) {
          if ((error as { code?: unknown }).code !== 1) throw error;
        }
      }
      if (!owner)
        throw new Error(
          `Cannot identify the source package of bundled library: ${binary.path}`,
        );
      const { stdout } = await execute(
        "dpkg-query",
        ["-W", "-f=${source:Package}\t${source:Version}", owner],
        { env: environment },
      );
      [source, version] = stdout.trim().split("\t") as [string, string];
      if (!source || !version)
        throw new Error(`Incomplete source package information for ${owner}`);
      const os = await readFile("/etc/os-release", "utf8");
      url = /^ID=ubuntu$/m.test(os)
        ? "https://archive.ubuntu.com/ubuntu/"
        : "https://deb.debian.org/debian/";
      licensePath = join("/usr/share/doc", owner.split(":")[0]!, "copyright");
    }
    const copyright = await readFile(licensePath);
    const noticeName = `${createHash("sha256").update(copyright).digest("hex")}.txt`;
    await writeFile(join(notices, noticeName), copyright);
    records.push({
      file: binary.file,
      sha256: createHash("sha256")
        .update(await readFile(join(runtimeDirectory, binary.file)))
        .digest("hex"),
      source,
      version,
      url,
      sourceSha256: upstream?.sha256,
      license: `licenses/linux/${noticeName}`,
    });
  }
  await writeFile(
    join(runtimeDirectory, "linux-bundle.json"),
    `${JSON.stringify(records, null, 2)}\n`,
  );
  return records;
};

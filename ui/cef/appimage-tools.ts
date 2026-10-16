// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFile } from "node:child_process";
import { createHash, randomUUID } from "node:crypto";
import {
  chmod,
  mkdir,
  readFile,
  rename,
  rm,
  writeFile,
} from "node:fs/promises";
import { homedir } from "node:os";
import { join, resolve } from "node:path";
import { promisify } from "node:util";

import type { MuonProgressCallback } from "../common/progress.js";
import type { LinuxDependencyTarget } from "./linux-dependencies.js";

const execute = promisify(execFile);
const architectures = {
  "linux-amd64": "x86_64",
  "linux-armhf": "armhf",
  "linux-arm64": "aarch64",
} as const;
const toolVersion = "1.9.1";
const runtimeVersion = "20251108";
const toolHashes = {
  x86_64: "ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0",
  armhf: "42b61cba5495d8aaf418a5c9a015a49b85ad92efabcbd3c341f1540440e4e23d",
  aarch64: "f0837e7448a0c1e4e650a93bb3e85802546e60654ef287576f46c71c126a9158",
} as const;
const runtimeHashes = {
  x86_64: "2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d",
  armhf: "e9060d37577b8a29914ec12d8740add24e19ff29012fb1fa0f60daf62db0688d",
  aarch64: "00cbdfcf917cc6c0ff6d3347d59e0ca1f7f45a6df1a428a0d6d8a78664d87444",
} as const;

/** Verified build tools, using the host tool and the selected target runtime. */
export interface LinuxAppImageTools {
  /** appimagetool AppImage for the build host. */
  executable: string;
  /** Static type-2 runtime embedded in the output AppImage. */
  runtime: string;
  /** AppImage architecture name passed to appimagetool. */
  architecture: string;
}

const unavailable = (message: string): Error =>
  Object.assign(new Error(message), {
    code: "MUON_APPIMAGE_TOOLS_UNAVAILABLE",
  });

const ensureAsset = async (
  directory: string,
  repository: string,
  version: string,
  name: string,
  expectedHash: string,
  progress: MuonProgressCallback | undefined,
): Promise<string> => {
  const parent = join(directory, repository, version);
  const path = join(parent, name);
  const url = `https://github.com/AppImage/${repository}/releases/download/${version}/${name}`;
  let data: Buffer | undefined;
  try {
    data = await readFile(path);
  } catch (error) {
    if ((error as NodeJS.ErrnoException).code !== "ENOENT") throw error;
  }
  const cached = data !== undefined;
  if (data === undefined) {
    progress?.({ phase: "pack", status: `Downloading ${name} (${version})` });
    try {
      const response = await fetch(url);
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      data = Buffer.from(await response.arrayBuffer());
    } catch (error) {
      throw new Error(
        `Cannot download ${name} ${version}. Download ${url} to ${path} and retry.`,
        { cause: error },
      );
    }
  }
  if (createHash("sha256").update(data).digest("hex") !== expectedHash)
    throw new Error(`SHA-256 mismatch for ${name} ${version}: ${path}`);
  if (!cached) {
    await mkdir(parent, { recursive: true });
    const temporary = `${path}.${randomUUID()}.tmp`;
    try {
      await writeFile(temporary, data, { flag: "wx", mode: 0o755 });
      await rename(temporary, path);
    } finally {
      await rm(temporary, { force: true });
    }
  }
  await chmod(path, 0o755);
  return path;
};

/**
 * Acquires checksum-pinned AppImage build tools, reusing the local cache offline.
 * @param target - Linux CPU to package; it may differ from the build host CPU.
 * @param environment - Cache location and tool search environment.
 * @param progress - Optional packaging progress sink.
 * @returns Verified host appimagetool and target type-2 runtime paths.
 * @remarks appimagetool's default runtime download is bypassed with --runtime-file.
 * The published AppImages bundle mksquashfs and desktop-file-validate.
 */
export const ensureLinuxAppImageTools = async (
  target: LinuxDependencyTarget,
  environment: NodeJS.ProcessEnv,
  progress: MuonProgressCallback | undefined,
): Promise<LinuxAppImageTools> => {
  const hostArchitectures: Partial<
    Record<NodeJS.Architecture, keyof typeof toolHashes>
  > = {
    x64: "x86_64",
    arm: "armhf",
    arm64: "aarch64",
  };
  const hostArchitecture = hostArchitectures[process.arch];
  if (process.platform !== "linux" || hostArchitecture === undefined)
    throw unavailable(
      "AppImage packaging requires a Linux amd64, armhf or arm64 build host.",
    );
  try {
    await execute("file", ["--version"], { env: environment });
  } catch (error) {
    if ((error as NodeJS.ErrnoException).code !== "ENOENT") throw error;
    throw unavailable(
      "file is required for AppImage packaging. Install file and coreutils (Debian/Ubuntu: apt install file coreutils).",
    );
  }
  const directory = resolve(
    environment.MUON_APPIMAGE_TOOLS_DIR ??
      join(
        environment.XDG_CACHE_HOME ??
          join(environment.HOME ?? homedir(), ".cache"),
        "muon",
        "appimage",
      ),
  );
  const architecture = architectures[target];
  const executable = await ensureAsset(
    directory,
    "appimagetool",
    toolVersion,
    `appimagetool-${hostArchitecture}.AppImage`,
    toolHashes[hostArchitecture],
    progress,
  );
  const runtime = await ensureAsset(
    directory,
    "type2-runtime",
    runtimeVersion,
    `runtime-${architecture}`,
    runtimeHashes[architecture],
    progress,
  );
  return { executable, runtime, architecture };
};

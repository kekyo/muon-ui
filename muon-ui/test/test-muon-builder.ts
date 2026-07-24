// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFile } from "node:child_process";
import { mkdir, writeFile } from "node:fs/promises";
import { join, resolve } from "node:path";
import { promisify } from "node:util";

const execFileAsync = promisify(execFile);

interface RuntimeInfoHeaderOptions {
  archiveFileName: string;
  archiveUrl: string;
  archiveSha1: string;
  archiveSize: number;
  executableName: string;
  corePayload: readonly string[];
}

export interface TestMuonBuilderBinaries {
  prepareExecutablePath: string;
  launcherExecutablePath: string;
}

const cString = (value: string): string => JSON.stringify(value);

export const createRuntimeInfoHeader = ({
  archiveFileName,
  archiveUrl,
  archiveSha1,
  archiveSize,
  executableName,
  corePayload,
}: RuntimeInfoHeaderOptions): string => {
  const payload = corePayload.map((item) => `    ${cString(item)}`).join(",\n");
  return `#ifndef MUON_RUNTIME_INFO_GENERATED_H
#define MUON_RUNTIME_INFO_GENERATED_H

#include <stddef.h>

#define MUON_RUNTIME_INFO_AVAILABLE 1

typedef struct {
  const char *file_name;
  const char *url;
  const char *sha1;
  unsigned long long size;
} MuonRuntimeCefArtifactInfo;

typedef struct {
  const char *name;
  const char *executable_name;
  const char *target;
  const char *cef_target;
  const char *muon_core_version;
  const char *muon_core_git_commit_hash;
  const char *muon_core_build_date;
  const char *muon_core_git_commit_date;
  const char *cef_reference_version;
  const char *cef_reference_distribution;
  int cef_reference_api_version;
  const char *cef_reference_api_hash;
  MuonRuntimeCefArtifactInfo cef_reference_artifact;
  const char *const *core_payload;
  size_t core_payload_count;
} MuonRuntimeInfo;

static const char *const kMuonRuntimeCorePayload[] = {
${payload}
};

static const MuonRuntimeInfo kMuonRuntimeInfo = {
    "muon-core",
    ${cString(executableName)},
    "linux-amd64",
    "linux64",
    "fake-muon-core",
    "fake-git-commit",
    "2026-07-07T00:00:00+09:00",
    "2026-07-01T00:00:00+09:00",
    "fake-cef",
    "minimal",
    14700,
    "fake-api-hash",
    {
        ${cString(archiveFileName)},
        ${cString(archiveUrl)},
        ${cString(archiveSha1)},
        ${archiveSize},
    },
    kMuonRuntimeCorePayload,
    ${corePayload.length},
};

#endif
`;
};

export const buildTestMuonBuilder = async (
  root: string,
  runtimeInfoHeader: string,
): Promise<TestMuonBuilderBinaries> => {
  const prepareRoot = resolve("..", "muon-builder");
  const outDir = join(root, "muon-builder-build");
  const generatedDir = join(outDir, "generated");
  const versionHeader = join(generatedDir, "version.h");
  const runtimeInfoHeaderPath = join(
    generatedDir,
    "muon_runtime_info_generated.h",
  );
  await mkdir(generatedDir, { recursive: true });
  await writeFile(
    versionHeader,
    `#ifndef MUON_BUILDER_VERSION_H
#define MUON_BUILDER_VERSION_H
#define MUON_BUILDER_VERSION "test"
#define MUON_BUILDER_GIT_COMMIT_HASH "00000000"
#endif
`,
  );
  await writeFile(runtimeInfoHeaderPath, runtimeInfoHeader);

  const yyjsonSourceDir = join(
    prepareRoot,
    ".deps",
    "src",
    "yyjson-0.12.0",
    "src",
  );
  const bzip2SourceDir = join(prepareRoot, ".deps", "src", "bzip2-1.0.8");
  const bzip2Lib = join(
    prepareRoot,
    ".deps",
    "build",
    "bzip2-linux64",
    "libbz2.a",
  );
  const zlibInstallDir = join(
    prepareRoot,
    ".deps",
    "build",
    "zlib-linux64",
    "install",
  );
  const zlibIncludeDir = join(zlibInstallDir, "include");
  const zlibLib = join(zlibInstallDir, "lib", "libz.a");
  const libarchiveIncludeDir = join(
    prepareRoot,
    ".deps",
    "src",
    "libarchive-3.8.7",
    "libarchive",
  );
  const libarchiveLib = join(
    prepareRoot,
    ".deps",
    "build",
    "libarchive-linux64",
    "libarchive",
    "libarchive.a",
  );
  await execFileAsync("bash", [join(prepareRoot, "build_yyjson.sh")]);
  await execFileAsync("bash", [
    join(prepareRoot, "build_bzip2.sh"),
    "linux64",
    "gcc",
    "ar",
    "ranlib",
  ]);
  await execFileAsync("bash", [
    join(prepareRoot, "build_zlib.sh"),
    "linux64",
    "gcc",
    "ar",
    "ranlib",
  ]);
  await execFileAsync("bash", [
    join(prepareRoot, "build_libarchive.sh"),
    "linux64",
    "gcc",
    "ar",
    "ranlib",
    bzip2Lib,
    zlibIncludeDir,
    zlibLib,
  ]);

  const prepareExecutablePath = join(outDir, "muon-builder");
  const launcherExecutablePath = join(outDir, "muon-launcher");
  await execFileAsync("make", [
    "-C",
    prepareRoot,
    "-B",
    "CC=gcc",
    "AR=ar",
    `OUT_DIR=${outDir}`,
    `YYJSON_SOURCE_DIR=${yyjsonSourceDir}`,
    `PREPARE_TARGET=${prepareExecutablePath}`,
    `LAUNCHER_TARGET=${launcherExecutablePath}`,
    `VERSION_HEADER=${versionHeader}`,
    `RUNTIME_INFO_HEADER=${runtimeInfoHeaderPath}`,
    `CPPFLAGS=-I${generatedDir} -I${yyjsonSourceDir} -I${libarchiveIncludeDir} -I${bzip2SourceDir} -I${zlibIncludeDir} -DLIBARCHIVE_STATIC -DMUON_PREPARE_TARGET_NAME=\\"linux-amd64\\"`,
    "LAUNCHER_CPPFLAGS=",
    "CFLAGS=-std=c99 -O0 -g -Wall -Wextra -pedantic",
    "LDFLAGS=-static",
    "LAUNCHER_LDFLAGS=",
    `LDLIBS=${libarchiveLib} ${bzip2Lib} ${zlibLib}`,
    "LAUNCHER_LDLIBS=-lxcb",
  ]);

  return {
    prepareExecutablePath,
    launcherExecutablePath,
  };
};

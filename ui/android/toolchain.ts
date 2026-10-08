// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { constants } from "node:fs";
import { access, mkdir, readFile } from "node:fs/promises";
import { homedir } from "node:os";
import { delimiter, dirname, join, resolve } from "node:path";
import { runAndroidCommand } from "./command.js";

/** Versions fixed by the shipped Android component set. */
export interface MuonAndroidToolchain {
  /** Exact Android SDK platform package directory. */ readonly platform: string;
  /** Component schema. */ readonly schemaVersion: 1;
  /** Android Gradle plugin version. */ readonly agp: string;
  /** Wrapper version. */ readonly gradle: string;
  /** API used for compilation. */ readonly compileSdk: number;
  /** API targeted by generated applications. */ readonly targetSdk: number;
  /** Minimum installation API; see documented verification range. */ readonly minSdk: number;
  /** Required SDK Build Tools version. */ readonly buildTools: string;
  /** Minimum JDK runtime version. */ readonly javaMin: number;
  /** Highest supported JDK runtime version. */ readonly javaMax: number;
  /** Version of dev.muon:runtime. */ readonly runtimeVersion: string;
  /** ABIs included in the standard runtime. */ readonly abis: readonly string[];
}

/** Inputs for the Android environment diagnostic. */
export interface MuonAndroidPrepareOptions {
  /** Directory containing templates, maven, renderer and toolchain.json. */ componentsDirectory: string;
  /** Explicit SDK location, otherwise resolved from the environment or adb. */ sdkPath:
    string | undefined;
  /** Child environment. */ environment: NodeJS.ProcessEnv;
  /** Whether to download/verify the pinned Gradle distribution using its Wrapper. */ prepareGradle: boolean;
}

/** Validated tools for an Android build; no NDK or CMake is needed. */
export interface MuonAndroidPrepareResult {
  /** Discriminates Android from desktop preparation. */ readonly target: "android";
  /** Absolute SDK directory. */ readonly sdkPath: string;
  /** Detected JDK major version. */ readonly javaVersion: number;
  /** Absolute Java executable. */ readonly javaPath: string;
  /** Shipped version requirements. */ readonly toolchain: MuonAndroidToolchain;
  /** Component directory used for this build. */ readonly componentsDirectory: string;
}

const accessible = async (path: string, mode: number): Promise<boolean> => {
  try {
    await access(path, mode);
    return true;
  } catch {
    return false;
  }
};

const findExecutable = async (
  name: string,
  environment: NodeJS.ProcessEnv,
): Promise<string | undefined> => {
  for (const directory of (environment.PATH ?? "")
    .split(delimiter)
    .filter(Boolean)) {
    const path = join(directory, name);
    if (await accessible(path, constants.X_OK)) return path;
  }
  return undefined;
};

/**
 * Diagnoses a Linux x64 SDK/JDK installation and optionally prepares Gradle.
 * @param options - Component location, SDK override and process environment.
 * @returns Validated tool paths and pinned versions.
 * @remarks Does not install SDK packages, accept licenses, or require a device.
 */
export const prepareAndroid = async (
  options: MuonAndroidPrepareOptions,
): Promise<MuonAndroidPrepareResult> => {
  if (process.platform !== "linux" || process.arch !== "x64") {
    throw new Error("Android builds currently support Linux x64 hosts.");
  }
  const componentsDirectory = resolve(options.componentsDirectory);
  const toolchain = JSON.parse(
    await readFile(join(componentsDirectory, "toolchain.json"), "utf8"),
  ) as MuonAndroidToolchain;
  if (toolchain.schemaVersion !== 1)
    throw new Error("Unsupported Muon Android toolchain schema.");
  const adb = await findExecutable("adb", options.environment);
  const candidate =
    options.sdkPath ??
    options.environment.ANDROID_HOME ??
    options.environment.ANDROID_SDK_ROOT ??
    (adb === undefined ? undefined : dirname(dirname(adb)));
  if (!candidate)
    throw new Error(
      "Android SDK not found. Set ANDROID_HOME or android.sdkPath.",
    );
  const sdkPath = resolve(candidate);
  for (const [file, packageName] of [
    [
      `platforms/${toolchain.platform}/android.jar`,
      `platforms;${toolchain.platform}`,
    ],
    ...["aapt2", "zipalign", "apksigner"].map((name) => [
      `build-tools/${toolchain.buildTools}/${name}`,
      `build-tools;${toolchain.buildTools}`,
    ]),
  ]) {
    if (!(await accessible(join(sdkPath, file!), constants.R_OK))) {
      throw new Error(
        `Android SDK ${sdkPath} is missing ${packageName}. Install this package using the Android CLI: android sdk install '${packageName}'. See https://developer.android.com/tools/agents/android-cli/commands/sdk`,
      );
    }
  }
  const platformProperties = await readFile(
    join(sdkPath, "platforms", toolchain.platform, "source.properties"),
    "utf8",
  );
  if (
    Number(
      /^AndroidVersion.ApiLevel\s*=\s*(\S+)/mu.exec(platformProperties)?.[1],
    ) !== toolchain.compileSdk
  ) {
    throw new Error(
      `Android SDK platform version differs from the required API ${toolchain.compileSdk}: ${sdkPath}`,
    );
  }
  const buildProperties = await readFile(
    join(sdkPath, "build-tools", toolchain.buildTools, "source.properties"),
    "utf8",
  );
  if (
    /^Pkg.Revision\s*=\s*(\S+)/mu.exec(buildProperties)?.[1] !==
    toolchain.buildTools
  ) {
    throw new Error(
      `Android SDK Build Tools version differs from ${toolchain.buildTools}: ${sdkPath}`,
    );
  }
  const javaPath = options.environment.JAVA_HOME
    ? join(options.environment.JAVA_HOME, "bin/java")
    : await findExecutable("java", options.environment);
  if (!javaPath || !(await accessible(javaPath, constants.X_OK))) {
    throw new Error(
      `JDK ${toolchain.javaMin}–${toolchain.javaMax} is required. Set JAVA_HOME or add java to PATH.`,
    );
  }
  const versionText = await runAndroidCommand(
    javaPath,
    ["-version"],
    componentsDirectory,
    options.environment,
    undefined,
  );
  const javaVersion = Number(
    /(?:openjdk|java) version "(\d+)[."]/u.exec(versionText)?.[1],
  );
  if (
    !Number.isInteger(javaVersion) ||
    javaVersion < toolchain.javaMin ||
    javaVersion > toolchain.javaMax
  ) {
    throw new Error(
      `JDK ${toolchain.javaMin}–${toolchain.javaMax} is required; detected ${versionText.trim()}.`,
    );
  }
  if (options.prepareGradle) {
    const cache = join(homedir(), ".cache/muon/android", toolchain.gradle);
    await mkdir(cache, { recursive: true });
    const result = await runAndroidCommand(
      join(componentsDirectory, "templates/gradlew"),
      ["--version"],
      cache,
      options.environment,
      undefined,
    );
    if (!result.includes(`Gradle ${toolchain.gradle}`))
      throw new Error(
        "Gradle Wrapper version differs from the shipped toolchain.",
      );
  }
  return {
    target: "android",
    sdkPath,
    javaVersion,
    javaPath,
    toolchain,
    componentsDirectory,
  };
};

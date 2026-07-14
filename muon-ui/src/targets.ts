// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

/**
 * Public muon target platform identifier.
 */
export type MuonTarget =
  | "linux-amd64"
  | "linux-armhf"
  | "linux-arm64"
  | "windows-i686"
  | "windows-amd64";

/**
 * Internal CEF target platform identifier.
 */
export type MuonCefTarget =
  | "linux64"
  | "linuxarm"
  | "linuxarm64"
  | "windows32"
  | "windows64";

/**
 * muon target operating system family.
 */
export type MuonTargetOperatingSystem = "linux" | "windows";

/**
 * Metadata for one supported muon target.
 */
export interface MuonTargetDescriptor {
  /**
   * Public target identifier accepted by muon CLI and Vite options.
   */
  id: MuonTarget;
  /**
   * Internal CEF target identifier used for CEF catalog and native builds.
   */
  cefTarget: MuonCefTarget;
  /**
   * Operating system family.
   */
  os: MuonTargetOperatingSystem;
  /**
   * Public architecture name used in package artifact names.
   */
  arch: "amd64" | "armhf" | "arm64" | "i686";
  /**
   * Fixed dist directory path for generated app distributions.
   */
  distributionDirectoryName: string;
  /**
   * muon-core executable file name for this target.
   */
  runtimeExecutableName: string;
  /**
   * muon-launcher executable file name for this target.
   */
  launcherExecutableName: string;
  /**
   * Optional privileged runtime helper executable file name.
   */
  runtimeHelperExecutableName?: string;
  /**
   * Launcher file suffix for app launcher executables.
   */
  launcherExtension: string;
  /**
   * Required runtime files copied into generated app distributions.
   */
  runtimeFiles: readonly string[];
  /**
   * Optional runtime files copied when they exist.
   */
  optionalRuntimeFilePatterns?: readonly RegExp[];
}

/**
 * Supported public muon targets in deterministic build order.
 */
export const allMuonTargets = [
  "linux-amd64",
  "linux-armhf",
  "linux-arm64",
  "windows-i686",
  "windows-amd64",
] as const satisfies readonly MuonTarget[];

const targetDescriptors = {
  "linux-amd64": {
    id: "linux-amd64",
    cefTarget: "linux64",
    os: "linux",
    arch: "amd64",
    distributionDirectoryName: "dist-muon/linux-amd64",
    runtimeExecutableName: "muon-core",
    launcherExecutableName: "muon-launcher",
    runtimeHelperExecutableName: "muon-runtime-helper",
    launcherExtension: "",
    runtimeFiles: ["muon-core", "libmuon-ui.so", "libcardio.so"],
  },
  "linux-armhf": {
    id: "linux-armhf",
    cefTarget: "linuxarm",
    os: "linux",
    arch: "armhf",
    distributionDirectoryName: "dist-muon/linux-armhf",
    runtimeExecutableName: "muon-core",
    launcherExecutableName: "muon-launcher",
    runtimeHelperExecutableName: "muon-runtime-helper",
    launcherExtension: "",
    runtimeFiles: ["muon-core", "libmuon-ui.so", "libcardio.so"],
  },
  "linux-arm64": {
    id: "linux-arm64",
    cefTarget: "linuxarm64",
    os: "linux",
    arch: "arm64",
    distributionDirectoryName: "dist-muon/linux-arm64",
    runtimeExecutableName: "muon-core",
    launcherExecutableName: "muon-launcher",
    runtimeHelperExecutableName: "muon-runtime-helper",
    launcherExtension: "",
    runtimeFiles: ["muon-core", "libmuon-ui.so", "libcardio.so"],
  },
  "windows-i686": {
    id: "windows-i686",
    cefTarget: "windows32",
    os: "windows",
    arch: "i686",
    distributionDirectoryName: "dist-muon/windows-i686",
    runtimeExecutableName: "muon-core.exe",
    launcherExecutableName: "muon-launcher.exe",
    launcherExtension: ".exe",
    runtimeFiles: ["muon-core.exe", "libmuon-ui.dll", "libcardio.dll"],
    optionalRuntimeFilePatterns: [
      /^libgcc_s_.*-1\.dll$/,
      /^libstdc\+\+-6\.dll$/,
      /^libwinpthread-1\.dll$/,
    ],
  },
  "windows-amd64": {
    id: "windows-amd64",
    cefTarget: "windows64",
    os: "windows",
    arch: "amd64",
    distributionDirectoryName: "dist-muon/windows-amd64",
    runtimeExecutableName: "muon-core.exe",
    launcherExecutableName: "muon-launcher.exe",
    launcherExtension: ".exe",
    runtimeFiles: ["muon-core.exe", "libmuon-ui.dll", "libcardio.dll"],
    optionalRuntimeFilePatterns: [
      /^libgcc_s_.*-1\.dll$/,
      /^libstdc\+\+-6\.dll$/,
      /^libwinpthread-1\.dll$/,
    ],
  },
} as const satisfies Record<MuonTarget, MuonTargetDescriptor>;

const targetByCefTarget = Object.fromEntries(
  allMuonTargets.map((target) => [targetDescriptors[target].cefTarget, target]),
) as Record<MuonCefTarget, MuonTarget>;

/**
 * Returns metadata for a public muon target.
 *
 * @param target Public muon target.
 * @returns Target descriptor.
 */
export const getMuonTargetDescriptor = (
  target: MuonTarget,
): MuonTargetDescriptor => targetDescriptors[target];

/**
 * Returns the runtime app identifier embedded for one target distribution.
 *
 * @param appId Base sanitized application identifier.
 * @param target Public muon target.
 * @returns Target runtime application identifier.
 *
 * @remarks Windows targets append their public architecture name so 32-bit and
 * 64-bit packages do not share runtime state or installer identities.
 */
export const getMuonTargetRuntimeAppId = (
  appId: string,
  target: MuonTarget,
): string => {
  const descriptor = getMuonTargetDescriptor(target);
  return descriptor.os === "windows" ? `${appId}.${descriptor.arch}` : appId;
};

/**
 * Normalizes a user supplied public muon target.
 *
 * @param target Target value supplied by the user.
 * @param label Error label used in diagnostics.
 * @returns Public muon target.
 */
export const normalizeMuonTarget = (
  target: string,
  label = "muon target",
): MuonTarget => {
  const normalized = target.trim().toLowerCase();
  if (allMuonTargets.includes(normalized as MuonTarget)) {
    return normalized as MuonTarget;
  }
  throw new Error(`Unsupported ${label}: ${target}`);
};

/**
 * Resolves a Node platform and architecture pair to a public muon target.
 *
 * @param platform Node platform.
 * @param architecture Node architecture.
 * @returns Public muon target.
 */
export const getDefaultMuonTarget = (
  platform: NodeJS.Platform,
  architecture: NodeJS.Architecture,
): MuonTarget => {
  if (platform === "win32") {
    if (architecture === "ia32") {
      return "windows-i686";
    }
    if (architecture === "x64") {
      return "windows-amd64";
    }
  }
  if (platform === "linux") {
    if (architecture === "x64") {
      return "linux-amd64";
    }
    if (architecture === "arm") {
      return "linux-armhf";
    }
    if (architecture === "arm64") {
      return "linux-arm64";
    }
  }
  throw new Error(
    `Unsupported muon target: platform=${platform}, arch=${architecture}`,
  );
};

/**
 * Resolves an internal CEF target to a public muon target.
 *
 * @param cefTarget Internal CEF target.
 * @returns Public muon target.
 */
export const getMuonTargetForCefTarget = (
  cefTarget: MuonCefTarget,
): MuonTarget => targetByCefTarget[cefTarget];

// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

import {
  chmod,
  copyFile,
  cp,
  mkdir,
  readdir,
  rm,
  stat,
} from "node:fs/promises";
import { dirname, resolve } from "node:path";

const targetDescriptors = {
  "linux-amd64": {
    prepareExecutableName: "muon-builder",
    launcherExecutableName: "muon-launcher",
    runtimeHelperExecutableName: "muon-runtime-helper",
    devSourceDirectory: ".run/dev-linux-amd64-debug",
    distSourceDirectory: "dist-linux-amd64",
    runtimeSourceDirectory: "dist-linux-amd64",
    runtimePayload: [
      "muon-core",
      "libmuon-ui.so",
      "libcardio.so",
      "CREDITS.md",
    ],
  },
  "linux-armhf": {
    prepareExecutableName: "muon-builder",
    launcherExecutableName: "muon-launcher",
    runtimeHelperExecutableName: "muon-runtime-helper",
    devSourceDirectory: ".run/dev-linux-armhf-debug",
    distSourceDirectory: "dist-linux-armhf",
    runtimeSourceDirectory: "dist-linux-armhf",
    runtimePayload: [
      "muon-core",
      "libmuon-ui.so",
      "libcardio.so",
      "CREDITS.md",
    ],
  },
  "linux-arm64": {
    prepareExecutableName: "muon-builder",
    launcherExecutableName: "muon-launcher",
    runtimeHelperExecutableName: "muon-runtime-helper",
    devSourceDirectory: ".run/dev-linux-arm64-debug",
    distSourceDirectory: "dist-linux-arm64",
    runtimeSourceDirectory: "dist-linux-arm64",
    runtimePayload: [
      "muon-core",
      "libmuon-ui.so",
      "libcardio.so",
      "CREDITS.md",
    ],
  },
  "windows-i686": {
    prepareExecutableName: "muon-builder.exe",
    launcherExecutableName: "muon-launcher.exe",
    runtimeHelperExecutableName: undefined,
    devSourceDirectory: ".run/dev-windows-i686-debug",
    distSourceDirectory: "dist-windows-i686",
    runtimeSourceDirectory: "dist-windows-i686",
    runtimePayload: [
      "muon-core.exe",
      "libmuon-ui.dll",
      "libcardio.dll",
      "CREDITS.md",
    ],
    runtimeOptionalPayloadPatterns: [
      /^libgcc_s_.*-1\.dll$/,
      /^libstdc\+\+-6\.dll$/,
      /^libwinpthread-1\.dll$/,
    ],
  },
  "windows-amd64": {
    prepareExecutableName: "muon-builder.exe",
    launcherExecutableName: "muon-launcher.exe",
    runtimeHelperExecutableName: undefined,
    devSourceDirectory: ".run/dev-windows-amd64-debug",
    distSourceDirectory: "dist-windows-amd64",
    runtimeSourceDirectory: "dist-windows-amd64",
    runtimePayload: [
      "muon-core.exe",
      "libmuon-ui.dll",
      "libcardio.dll",
      "CREDITS.md",
    ],
    runtimeOptionalPayloadPatterns: [
      /^libgcc_s_.*-1\.dll$/,
      /^libstdc\+\+-6\.dll$/,
      /^libwinpthread-1\.dll$/,
    ],
  },
};

const allTargets = [
  "linux-amd64",
  "linux-armhf",
  "linux-arm64",
  "windows-i686",
  "windows-amd64",
];

const normalizeTarget = (target) => {
  const normalized = target.trim().toLowerCase();
  if (allTargets.includes(normalized)) {
    return normalized;
  }
  throw new Error(`Unsupported muon-builder stage target: ${target}`);
};

const getDefaultTarget = () => {
  if (process.platform === "linux" && process.arch === "x64") {
    return "linux-amd64";
  }
  if (process.platform === "linux" && process.arch === "arm") {
    return "linux-armhf";
  }
  if (process.platform === "linux" && process.arch === "arm64") {
    return "linux-arm64";
  }
  if (process.platform === "win32" && process.arch === "ia32") {
    return "windows-i686";
  }
  if (process.platform === "win32" && process.arch === "x64") {
    return "windows-amd64";
  }
  throw new Error(
    `Unsupported muon-builder stage host: platform=${process.platform}, arch=${process.arch}`,
  );
};

const parseTargets = () => {
  const [, , ...args] = process.argv;
  const source = args.includes("--dist") ? "dist" : "dev";
  if (args.includes("--all")) {
    return {
      targets: allTargets,
      source: "dist",
    };
  }
  const targetIndex = args.indexOf("--target");
  if (targetIndex >= 0) {
    const target = args[targetIndex + 1];
    if (target === undefined) {
      throw new Error("--target requires a value.");
    }
    return {
      targets: [normalizeTarget(target)],
      source,
    };
  }
  return {
    targets: [getDefaultTarget()],
    source,
  };
};

const getSourceDirectory = (target, source) => {
  const descriptor = targetDescriptors[target];
  return source === "dist"
    ? descriptor.distSourceDirectory
    : descriptor.devSourceDirectory;
};

const stageTarget = async (target, source) => {
  const descriptor = targetDescriptors[target];
  const prepareSourcePath = resolve(
    "..",
    "muon-builder",
    getSourceDirectory(target, source),
    descriptor.prepareExecutableName,
  );
  const launcherSourcePath = resolve(
    "..",
    "muon-builder",
    getSourceDirectory(target, source),
    descriptor.launcherExecutableName,
  );
  const runtimeHelperSourcePath =
    descriptor.runtimeHelperExecutableName === undefined
      ? undefined
      : resolve(
          "..",
          "muon-builder",
          getSourceDirectory(target, source),
          descriptor.runtimeHelperExecutableName,
        );
  const nativeDestinationPath = resolve(
    "dist",
    "native",
    target,
    descriptor.prepareExecutableName,
  );
  const launcherDestinationPath = resolve(
    "dist",
    "native",
    target,
    descriptor.launcherExecutableName,
  );
  const runtimeHelperDestinationPath =
    descriptor.runtimeHelperExecutableName === undefined
      ? undefined
      : resolve(
          "dist",
          "native",
          target,
          descriptor.runtimeHelperExecutableName,
        );
  try {
    await stat(prepareSourcePath);
    await stat(launcherSourcePath);
    if (runtimeHelperSourcePath !== undefined) {
      await stat(runtimeHelperSourcePath);
    }
  } catch {
    const buildCommand =
      source === "dist"
        ? `npm run build:target --workspace muon-builder -- dist Release ${target}`
        : `npm run build:target --workspace muon-builder -- dev Debug ${target}`;
    throw new Error(
      `Missing native binaries for ${target}. Run "${buildCommand}" first.`,
    );
  }

  await rm(dirname(nativeDestinationPath), { recursive: true, force: true });
  await mkdir(dirname(nativeDestinationPath), { recursive: true });
  await copyFile(prepareSourcePath, nativeDestinationPath);
  await copyFile(launcherSourcePath, launcherDestinationPath);
  if (
    runtimeHelperSourcePath !== undefined &&
    runtimeHelperDestinationPath !== undefined
  ) {
    await copyFile(runtimeHelperSourcePath, runtimeHelperDestinationPath);
  }
  if (process.platform !== "win32") {
    await chmod(nativeDestinationPath, 0o755);
    await chmod(launcherDestinationPath, 0o755);
    if (runtimeHelperDestinationPath !== undefined) {
      await chmod(runtimeHelperDestinationPath, 0o755);
    }
  }
};

const stageDefaultWindowsIcon = async () => {
  const sourcePath = resolve("..", "images", "muon-256.png");
  const destinationPath = resolve("dist", "native", "muon-256.png");
  await mkdir(dirname(destinationPath), { recursive: true });
  await copyFile(sourcePath, destinationPath);
};

const stageRuntimeTarget = async (target) => {
  const descriptor = targetDescriptors[target];
  const sourcePath = resolve(
    "..",
    "muon-core",
    descriptor.runtimeSourceDirectory,
  );
  try {
    await stat(sourcePath);
  } catch {
    throw new Error(
      `Missing ${sourcePath}. Run "npm run build:target --workspace muon-core -- dist Release ${target}" first.`,
    );
  }
  const destinationPath = resolve("dist", "runtime", target);
  await rm(destinationPath, { recursive: true, force: true });
  await mkdir(destinationPath, { recursive: true });
  const copyRuntimeItem = async (item) => {
    if (typeof item !== "string" || item === "" || item.includes("..")) {
      throw new Error(`Invalid runtime payload item for ${target}: ${item}`);
    }
    await cp(resolve(sourcePath, item), resolve(destinationPath, item), {
      recursive: true,
      preserveTimestamps: true,
    });
  };
  for (const item of descriptor.runtimePayload) {
    await copyRuntimeItem(item);
  }
  if (descriptor.runtimeOptionalPayloadPatterns !== undefined) {
    const entries = await readdir(sourcePath);
    for (const item of entries) {
      if (
        descriptor.runtimeOptionalPayloadPatterns.some((pattern) =>
          pattern.test(item),
        )
      ) {
        await copyRuntimeItem(item);
      }
    }
  }
};

const { targets, source } = parseTargets();
for (const target of targets) {
  await stageTarget(target, source);
  if (source === "dist") {
    await stageRuntimeTarget(target);
  }
}
await stageDefaultWindowsIcon();

if (process.platform !== "win32") {
  await chmod(resolve("dist", "cli.cjs"), 0o755);
}

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
  linux64: {
    prepareExecutableName: "muon-prepare",
    bootstrapExecutableName: "muon-bootstrap",
    devSourceDirectory: ".run/dev-linux64-debug",
    distSourceDirectory: "dist-linux64",
    runtimeSourceDirectory: "dist-linux64",
    runtimePayload: [
      "muon-core",
      "libmuon-ui.so",
      "libcardio.so",
      "LICENSE_muon",
    ],
  },
  linuxarm: {
    prepareExecutableName: "muon-prepare",
    bootstrapExecutableName: "muon-bootstrap",
    devSourceDirectory: ".run/dev-linuxarm-debug",
    distSourceDirectory: "dist-linuxarm",
    runtimeSourceDirectory: "dist-linuxarm",
    runtimePayload: [
      "muon-core",
      "libmuon-ui.so",
      "libcardio.so",
      "LICENSE_muon",
    ],
  },
  linuxarm64: {
    prepareExecutableName: "muon-prepare",
    bootstrapExecutableName: "muon-bootstrap",
    devSourceDirectory: ".run/dev-linuxarm64-debug",
    distSourceDirectory: "dist-linuxarm64",
    runtimeSourceDirectory: "dist-linuxarm64",
    runtimePayload: [
      "muon-core",
      "libmuon-ui.so",
      "libcardio.so",
      "LICENSE_muon",
    ],
  },
  windows32: {
    prepareExecutableName: "muon-prepare.exe",
    bootstrapExecutableName: "muon-bootstrap.exe",
    devSourceDirectory: ".run/dev-windows32-debug",
    distSourceDirectory: "dist-windows32",
    runtimeSourceDirectory: "dist-windows32",
    runtimePayload: [
      "muon-core.exe",
      "libmuon-ui.dll",
      "libcardio.dll",
      "LICENSE_muon",
    ],
    runtimeOptionalPayloadPatterns: [
      /^libgcc_s_.*-1\.dll$/,
      /^libstdc\+\+-6\.dll$/,
      /^libwinpthread-1\.dll$/,
    ],
  },
  windows64: {
    prepareExecutableName: "muon-prepare.exe",
    bootstrapExecutableName: "muon-bootstrap.exe",
    devSourceDirectory: ".run/dev-windows64-debug",
    distSourceDirectory: "dist-windows64",
    runtimeSourceDirectory: "dist-windows64",
    runtimePayload: [
      "muon-core.exe",
      "libmuon-ui.dll",
      "libcardio.dll",
      "LICENSE_muon",
    ],
    runtimeOptionalPayloadPatterns: [
      /^libgcc_s_.*-1\.dll$/,
      /^libstdc\+\+-6\.dll$/,
      /^libwinpthread-1\.dll$/,
    ],
  },
};

const normalizeTarget = (target) => {
  if (target === "linux64" || target === "amd64" || target === "x64") {
    return "linux64";
  }
  if (
    target === "linuxarm" ||
    target === "armv7l" ||
    target === "armv7" ||
    target === "armhf" ||
    target === "arm"
  ) {
    return "linuxarm";
  }
  if (target === "linuxarm64" || target === "arm64" || target === "aarch64") {
    return "linuxarm64";
  }
  if (
    target === "linux32" ||
    target === "i686" ||
    target === "i386" ||
    target === "ia32" ||
    target === "x86"
  ) {
    throw new Error(
      `Unsupported muon-prepare stage target: ${target}. Linux 32-bit CEF builds are discontinued after CEF 101.`,
    );
  }
  if (target === "mingw32" || target === "win32" || target === "windows32") {
    return "windows32";
  }
  if (target === "mingw64" || target === "win64" || target === "windows64") {
    return "windows64";
  }
  throw new Error(`Unsupported muon-prepare stage target: ${target}`);
};

const getDefaultTarget = () => {
  if (process.platform === "linux" && process.arch === "x64") {
    return "linux64";
  }
  if (process.platform === "linux" && process.arch === "arm") {
    return "linuxarm";
  }
  if (process.platform === "linux" && process.arch === "arm64") {
    return "linuxarm64";
  }
  if (process.platform === "win32" && process.arch === "ia32") {
    return "windows32";
  }
  if (process.platform === "win32" && process.arch === "x64") {
    return "windows64";
  }
  throw new Error(
    `Unsupported muon-prepare stage host: platform=${process.platform}, arch=${process.arch}`,
  );
};

const parseTargets = () => {
  const [, , ...args] = process.argv;
  const source = args.includes("--dist") ? "dist" : "dev";
  if (args.includes("--all")) {
    return {
      targets: ["linux64", "linuxarm", "linuxarm64", "windows32", "windows64"],
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
    "muon-prepare",
    getSourceDirectory(target, source),
    descriptor.prepareExecutableName,
  );
  const bootstrapSourcePath = resolve(
    "..",
    "muon-prepare",
    getSourceDirectory(target, source),
    descriptor.bootstrapExecutableName,
  );
  const nativeDestinationPath = resolve(
    "dist",
    "native",
    target,
    descriptor.prepareExecutableName,
  );
  const bootstrapDestinationPath = resolve(
    "dist",
    "native",
    target,
    descriptor.bootstrapExecutableName,
  );
  try {
    await stat(prepareSourcePath);
    await stat(bootstrapSourcePath);
  } catch {
    const buildCommand =
      source === "dist"
        ? `npm run build:target --workspace muon-prepare -- dist Release ${target}`
        : `npm run build:target --workspace muon-prepare -- dev Debug ${target}`;
    throw new Error(
      `Missing native binaries for ${target}. Run "${buildCommand}" first.`,
    );
  }

  await mkdir(dirname(nativeDestinationPath), { recursive: true });
  await copyFile(prepareSourcePath, nativeDestinationPath);
  await copyFile(bootstrapSourcePath, bootstrapDestinationPath);
  if (process.platform !== "win32") {
    await chmod(nativeDestinationPath, 0o755);
    await chmod(bootstrapDestinationPath, 0o755);
  }
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

if (process.platform !== "win32") {
  await chmod(resolve("dist", "cli.cjs"), 0o755);
}

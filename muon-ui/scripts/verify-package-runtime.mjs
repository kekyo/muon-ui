// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

import { access, readdir } from "node:fs/promises";
import { join } from "node:path";

const packageRuntimeTargets = {
  "linux-amd64": {
    nativePrepare: "muon-builder",
    nativeLauncher: "muon-launcher",
    coreExecutable: "muon-core",
    uiLibrary: "libmuon-ui.so",
    cardioLibrary: "libcardio.so",
  },
  "linux-armhf": {
    nativePrepare: "muon-builder",
    nativeLauncher: "muon-launcher",
    coreExecutable: "muon-core",
    uiLibrary: "libmuon-ui.so",
    cardioLibrary: "libcardio.so",
  },
  "linux-arm64": {
    nativePrepare: "muon-builder",
    nativeLauncher: "muon-launcher",
    coreExecutable: "muon-core",
    uiLibrary: "libmuon-ui.so",
    cardioLibrary: "libcardio.so",
  },
  "windows-i686": {
    nativePrepare: "muon-builder.exe",
    nativeLauncher: "muon-launcher.exe",
    coreExecutable: "muon-core.exe",
    uiLibrary: "libmuon-ui.dll",
    cardioLibrary: "libcardio.dll",
  },
  "windows-amd64": {
    nativePrepare: "muon-builder.exe",
    nativeLauncher: "muon-launcher.exe",
    coreExecutable: "muon-core.exe",
    uiLibrary: "libmuon-ui.dll",
    cardioLibrary: "libcardio.dll",
  },
};

const assertExists = async (path) => {
  try {
    await access(path);
  } catch {
    throw new Error(`Expected package runtime file is missing: ${path}`);
  }
};

const assertMissing = async (path) => {
  try {
    await access(path);
  } catch {
    return;
  }
  throw new Error(`Unexpected package runtime file exists: ${path}`);
};

const assertHasMatchingFile = async (directory, pattern) => {
  const entries = await readdir(directory);
  if (!entries.some((entry) => pattern.test(entry))) {
    throw new Error(
      `Expected package runtime file matching ${pattern} is missing in: ${directory}`,
    );
  }
};

await assertMissing(join("dist", "muon-builder"));
await assertMissing(join("dist", "muon-launcher"));
await assertMissing(join("dist", "muon-builder.exe"));
await assertMissing(join("dist", "muon-launcher.exe"));
await assertMissing(join("dist", ["muon", "prepare"].join("-")));
await assertMissing(join("dist", `${["muon", "prepare"].join("-")}.exe`));
await assertMissing(join("dist", "native", "linux32"));
await assertMissing(join("dist", "runtime", "linux32"));
await assertMissing(join("dist", "native", "muon-launcher.ico"));
await assertMissing(join("dist", "native", "muon-launcher.png"));
await assertExists(join("dist", "native", "muon-256.png"));

for (const [target, descriptor] of Object.entries(packageRuntimeTargets)) {
  const nativePath = join("dist", "native", target);
  const runtimePath = join("dist", "runtime", target);
  const expectedPayload = [
    descriptor.coreExecutable,
    descriptor.uiLibrary,
    descriptor.cardioLibrary,
    "CREDITS.md",
  ];

  await assertExists(join(nativePath, descriptor.nativePrepare));
  await assertExists(join(nativePath, descriptor.nativeLauncher));
  for (const item of expectedPayload) {
    await assertExists(join(runtimePath, item));
  }
  if (target.startsWith("windows-")) {
    await assertHasMatchingFile(runtimePath, /^libgcc_s_.*-1\.dll$/);
    await assertExists(join(runtimePath, "libstdc++-6.dll"));
  }
  await assertMissing(join(runtimePath, "THIRD_PARTY_NOTICES.md"));
  await assertMissing(join(runtimePath, "muon-runtime.json"));
  await assertMissing(join(runtimePath, "muon.json"));
  await assertMissing(join(runtimePath, "assets"));
}

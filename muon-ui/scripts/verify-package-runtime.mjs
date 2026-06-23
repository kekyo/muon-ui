// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

import { access, readdir } from "node:fs/promises";
import { join } from "node:path";

const packageRuntimeTargets = {
  linux64: {
    nativePrepare: "muon-prepare",
    nativeBootstrap: "muon-bootstrap",
    coreExecutable: "muon-core",
    uiLibrary: "libmuon-ui.so",
    cardioLibrary: "libcardio.so",
  },
  linuxarm: {
    nativePrepare: "muon-prepare",
    nativeBootstrap: "muon-bootstrap",
    coreExecutable: "muon-core",
    uiLibrary: "libmuon-ui.so",
    cardioLibrary: "libcardio.so",
  },
  linuxarm64: {
    nativePrepare: "muon-prepare",
    nativeBootstrap: "muon-bootstrap",
    coreExecutable: "muon-core",
    uiLibrary: "libmuon-ui.so",
    cardioLibrary: "libcardio.so",
  },
  windows32: {
    nativePrepare: "muon-prepare.exe",
    nativeBootstrap: "muon-bootstrap.exe",
    coreExecutable: "muon-core.exe",
    uiLibrary: "libmuon-ui.dll",
    cardioLibrary: "libcardio.dll",
  },
  windows64: {
    nativePrepare: "muon-prepare.exe",
    nativeBootstrap: "muon-bootstrap.exe",
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

await assertMissing(join("dist", "muon-prepare"));
await assertMissing(join("dist", "muon-bootstrap"));
await assertMissing(join("dist", "muon-prepare.exe"));
await assertMissing(join("dist", "muon-bootstrap.exe"));
await assertMissing(join("dist", "native", "linux32"));
await assertMissing(join("dist", "runtime", "linux32"));

for (const [target, descriptor] of Object.entries(packageRuntimeTargets)) {
  const nativePath = join("dist", "native", target);
  const runtimePath = join("dist", "runtime", target);
  const expectedPayload = [
    descriptor.coreExecutable,
    descriptor.uiLibrary,
    descriptor.cardioLibrary,
    "LICENSE_muon",
  ];

  await assertExists(join(nativePath, descriptor.nativePrepare));
  await assertExists(join(nativePath, descriptor.nativeBootstrap));
  for (const item of expectedPayload) {
    await assertExists(join(runtimePath, item));
  }
  if (target === "windows32" || target === "windows64") {
    await assertHasMatchingFile(runtimePath, /^libgcc_s_.*-1\.dll$/);
    await assertExists(join(runtimePath, "libstdc++-6.dll"));
  }
  await assertMissing(join(runtimePath, "THIRD_PARTY_NOTICES.md"));
  await assertMissing(join(runtimePath, "muon-runtime.json"));
  await assertMissing(join(runtimePath, "muon.json"));
  await assertMissing(join(runtimePath, "assets"));
}

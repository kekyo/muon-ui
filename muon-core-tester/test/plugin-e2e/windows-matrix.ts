// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

export type WindowsE2eTargetName = "windows-i686" | "windows-amd64";
export type WindowsE2ePlatform = "win32" | "win64";

export interface WindowsRuntimeTarget {
  debugRuntimeDirectory: string;
  platform: WindowsE2ePlatform;
  releaseRuntimeDirectory: string;
  target: WindowsE2eTargetName;
}

export interface WindowsE2eMatrixEntry<T> extends WindowsRuntimeTarget {
  caseNames: readonly T[];
}

export const windowsRuntimeTargets = [
  {
    debugRuntimeDirectory: "muon-core/.run/test-windows-i686-debug",
    platform: "win32",
    releaseRuntimeDirectory: "muon-core/.run/test-windows-i686-release",
    target: "windows-i686",
  },
  {
    debugRuntimeDirectory: "muon-core/.run/test-windows-amd64-debug",
    platform: "win64",
    releaseRuntimeDirectory: "muon-core/.run/test-windows-amd64-release",
    target: "windows-amd64",
  },
] as const satisfies readonly WindowsRuntimeTarget[];

export const createWindowsE2eMatrix = <T>(
  caseNames: readonly T[],
): WindowsE2eMatrixEntry<T>[] =>
  windowsRuntimeTargets.map((target) => ({
    ...target,
    caseNames,
  }));

export const resolveWindowsRuntimeTarget = (
  targetName: string | undefined,
): WindowsRuntimeTarget => {
  const normalized = targetName?.trim();
  const target =
    normalized === undefined || normalized === ""
      ? "windows-amd64"
      : normalized;
  const runtimeTarget = windowsRuntimeTargets.find(
    (entry) => entry.target === target || entry.platform === target,
  );
  if (runtimeTarget === undefined) {
    throw new Error(
      "MUON_E2E_WINDOWS_TARGET must be windows-i686, win32, windows-amd64, or win64",
    );
  }
  return runtimeTarget;
};

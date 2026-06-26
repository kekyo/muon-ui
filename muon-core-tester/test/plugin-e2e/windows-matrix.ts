// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

export type WindowsE2eTargetName = "windows32" | "windows64";
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
    debugRuntimeDirectory: "muon-core/.run/test-windows32-debug",
    platform: "win32",
    releaseRuntimeDirectory: "muon-core/.run/test-windows32-release",
    target: "windows32",
  },
  {
    debugRuntimeDirectory: "muon-core/.run/test-windows64-debug",
    platform: "win64",
    releaseRuntimeDirectory: "muon-core/.run/test-windows64-release",
    target: "windows64",
  },
] as const satisfies readonly WindowsRuntimeTarget[];

export const createWindowsE2eMatrix = <T>(
  caseNames: readonly T[],
): WindowsE2eMatrixEntry<T>[] =>
  windowsRuntimeTargets.map((target) => ({
    ...target,
    caseNames,
  }));

// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { allMuonTargets, type MuonDesktopTarget } from "../cef/targets.js";
export * from "../cef/targets.js";

/** Public build target. Android is selected explicitly. */
export type MuonTarget = MuonDesktopTarget | "android";

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
  if (
    normalized === "android" ||
    allMuonTargets.includes(normalized as MuonDesktopTarget)
  ) {
    return normalized as MuonTarget;
  }
  throw new Error(`Unsupported ${label}: ${target}`);
};

// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import type { MuonDesktopTarget } from "../common/targets.js";
import { resolveMuonNotificationConfig } from "../common/notification-config.js";

/**
 * Selects notification imports implemented by every selected desktop target.
 * @param value - Shared application notification configuration.
 * @param targets - Desktop targets that will use the generated web bundle.
 * @returns Enabled native notification paths for Linux and Windows.
 */
export const resolveMuonCefNotificationFunctionPaths = (
  value: unknown,
  targets: readonly MuonDesktopTarget[],
): readonly string[] => {
  const config = resolveMuonNotificationConfig(value);
  if (
    !config.enabled ||
    targets.length === 0 ||
    targets.some(
      (target) =>
        !target.startsWith("linux-") && !target.startsWith("windows-"),
    )
  )
    return [];
  return [
    "post",
    "cancel",
    "getStatus",
    "getCapabilities",
    "getPendingEvents",
    "watchEvents",
    "acknowledgeEvent",
  ].map((name) => `muon.notifications.${name}`);
};

// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { notificationIdentifier } from "../../core/common/renderer/notifications.js";

/** Notification features selected by the application, independently of OS permission. */
export interface MuonNotificationConfig {
  /** Whether the application includes and exposes notification operations. */
  readonly enabled: boolean;
  /** Purpose used by post() when its category is omitted. */
  readonly defaultCategory?: string;
}

/**
 * Validates the shared notification configuration.
 * @param value - The top-level notifications section, or undefined.
 * @returns Explicit normalized enablement; omitted configuration is disabled.
 */
export const resolveMuonNotificationConfig = (
  value: unknown,
): MuonNotificationConfig => {
  if (value === undefined) return { enabled: false };
  if (value === null || typeof value !== "object" || Array.isArray(value))
    throw new Error("notifications must be an object.");
  const source = value as Record<string, unknown>;
  for (const key of Object.keys(source)) {
    if (key !== "enabled" && key !== "defaultCategory")
      throw new Error(`Unsupported notifications setting: ${key}`);
  }
  if (source.enabled !== undefined && typeof source.enabled !== "boolean")
    throw new Error("notifications.enabled must be a boolean.");
  return {
    enabled: source.enabled === true,
    ...(source.defaultCategory === undefined
      ? {}
      : {
          defaultCategory: notificationIdentifier(
            source.defaultCategory,
            "notifications.defaultCategory",
          ),
        }),
  };
};

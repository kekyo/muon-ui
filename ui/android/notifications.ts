// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { resolve } from "node:path";
import sharp from "sharp";
import type { MuonNotificationConfig } from "../common/notification-config.js";
import {
  normalizeMuonAndroidNotificationChannel,
  type MuonAndroidNotificationChannelOptions,
} from "../../core/android/renderer/notification-channels.js";

/** Android-only notification inputs after resolving configuration paths. */
export interface MuonAndroidNotificationConfig {
  /** Static categories; OS channels are registered only on API 26 or later. */
  readonly channels: readonly Required<MuonAndroidNotificationChannelOptions>[];
  /** Absolute monochrome PNG source; undefined when notifications are disabled. */
  readonly smallIcon: string | undefined;
}

/**
 * Resolves Android-specific channels and validates their shared default reference.
 * @param value - android.notifications configuration, or undefined.
 * @param shared - Shared notification enablement and default category.
 * @param directory - Base directory for the supplied icon path.
 * @returns Normalized static channels and icon source.
 */
export const resolveAndroidNotificationConfig = (
  value: unknown,
  shared: MuonNotificationConfig,
  directory: string,
): MuonAndroidNotificationConfig => {
  if (
    value !== undefined &&
    (!value || typeof value !== "object" || Array.isArray(value))
  )
    throw new Error("android.notifications must be an object.");
  const input = (value ?? {}) as Record<string, unknown>;
  for (const key of Object.keys(input)) {
    if (!["channels", "smallIcon"].includes(key))
      throw new Error(`Unsupported android.notifications setting: ${key}`);
  }
  if (input.channels !== undefined && !Array.isArray(input.channels))
    throw new Error("android.notifications.channels must be an array.");
  const channels = ((input.channels ?? []) as unknown[]).map(
    normalizeMuonAndroidNotificationChannel,
  );
  if (new Set(channels.map((channel) => channel.id)).size !== channels.length)
    throw new Error("Duplicate Android notification channel ID.");
  if (
    shared.enabled &&
    shared.defaultCategory !== undefined &&
    !channels.some((channel) => channel.id === shared.defaultCategory)
  )
    throw new Error(
      "notifications.defaultCategory must reference a static Android channel.",
    );
  if (
    input.smallIcon !== undefined &&
    (typeof input.smallIcon !== "string" ||
      input.smallIcon.trim() === "" ||
      input.smallIcon.includes("\0"))
  )
    throw new Error(
      "android.notifications.smallIcon must be a nonempty PNG path.",
    );
  if (shared.enabled && input.smallIcon === undefined)
    throw new Error(
      "android.notifications.smallIcon is required when notifications are enabled.",
    );
  return {
    channels,
    smallIcon: shared.enabled
      ? resolve(directory, input.smallIcon as string)
      : undefined,
  };
};

/** One generated 24 dp notification icon resource. */
export interface MuonAndroidNotificationIcon {
  /** Android drawable resource density. */
  readonly density: string;
  /** Square dimensions in pixels. */
  readonly size: number;
  /** Decodable PNG resource with a white alpha silhouette. */
  readonly png: Buffer;
}

/**
 * Converts a white-on-transparent PNG into Android notification resources.
 * @param source - Square PNG bytes, 24 to 1024 pixels, with visible and transparent pixels.
 * @returns Icons for mdpi through xxxhdpi densities at 24 dp.
 * @remarks Launcher icons, opaque images and colored silhouettes are rejected.
 */
export const createAndroidNotificationIcon = async (
  source: Buffer,
): Promise<readonly MuonAndroidNotificationIcon[]> => {
  const metadata = await sharp(source, {
    limitInputPixels: 1024 * 1024,
  }).metadata();
  if (
    metadata.format !== "png" ||
    !metadata.hasAlpha ||
    metadata.width !== metadata.height ||
    !metadata.width ||
    metadata.width < 24 ||
    metadata.width > 1024 ||
    (metadata.pages ?? 1) !== 1
  )
    throw new Error(
      "Notification small icon must be a square transparent PNG from 24 to 1024 pixels.",
    );
  const { data, info } = await sharp(source)
    .toColourspace("srgb")
    .ensureAlpha()
    .raw()
    .toBuffer({ resolveWithObject: true });
  let visible = false;
  let transparent = false;
  for (let offset = 0; offset < data.length; offset += info.channels) {
    const alpha = data[offset + 3]!;
    transparent ||= alpha === 0;
    visible ||= alpha > 0;
    if (
      alpha > 0 &&
      (data[offset] !== 255 ||
        data[offset + 1] !== 255 ||
        data[offset + 2] !== 255)
    )
      throw new Error(
        "Notification small icon must contain a white silhouette on transparency.",
      );
  }
  if (!visible || !transparent)
    throw new Error(
      "Notification small icon must contain both visible and transparent pixels.",
    );
  const densities = [
    ["mdpi", 24],
    ["hdpi", 36],
    ["xhdpi", 48],
    ["xxhdpi", 72],
    ["xxxhdpi", 96],
  ] as const;
  return await Promise.all(
    densities.map(async ([density, size]) => ({
      density,
      size,
      png: await sharp(source).resize(size, size).png().toBuffer(),
    })),
  );
};

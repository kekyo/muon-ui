// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import {
  notificationIdentifier,
  type MuonNotificationSupport,
} from '../../common/renderer/notifications.js';

/** Initial importance for an Android notification channel. */
export type MuonAndroidNotificationImportance =
  'min' | 'low' | 'default' | 'high';

/** Channel definition registered before posting to its category. */
export interface MuonAndroidNotificationChannelOptions {
  /** Stable purpose key, up to 128 code units; also used as notification category. */
  readonly id: string;
  /** Nonempty user-visible name, up to 128 code units. */
  readonly name: string;
  /** User-visible explanation, up to 1024 code units; defaults to an empty string. */
  readonly description?: string;
  /** Initial importance; existing channels retain their OS/user-controlled settings. */
  readonly importance: MuonAndroidNotificationImportance;
}

/** Effective channel settings read from Android. */
export interface MuonAndroidNotificationChannel {
  /** Stable channel and purpose ID. */
  readonly id: string;
  /** Name currently registered with Android. */
  readonly name: string;
  /** Description currently registered with Android. */
  readonly description: string;
  /** OS-controlled importance; none means the channel is disabled. */
  readonly importance: MuonAndroidNotificationImportance | 'none';
  /** Whether this channel is enabled, independently of app-wide permission. */
  readonly enabled: boolean;
}

/** Features specific to Android notifications. */
export interface MuonAndroidNotificationCapabilities {
  /** OS channels require API 26; older versions only support static Muon categories. */
  readonly channels: MuonNotificationSupport;
}

/**
 * Validates and normalizes an Android channel definition.
 * @param value - Channel supplied by the application or its configuration.
 * @returns A checked definition with an explicit description.
 */
export const normalizeMuonAndroidNotificationChannel = (
  value: unknown
): Required<MuonAndroidNotificationChannelOptions> => {
  if (!value || typeof value !== 'object' || Array.isArray(value))
    throw new TypeError('Android notification channel must be an object');
  const input = value as Record<string, unknown>;
  for (const key of Object.keys(input)) {
    if (!['id', 'name', 'description', 'importance'].includes(key))
      throw new TypeError(
        `Unknown Android notification channel setting: ${key}`
      );
  }
  const id = notificationIdentifier(input.id, 'Channel id');
  const name = notificationIdentifier(input.name, 'Channel name');
  const description = input.description === undefined ? '' : input.description;
  if (
    typeof description !== 'string' ||
    description.length > 1024 ||
    description.includes('\0')
  )
    throw new TypeError(
      'Channel description must be a string of at most 1024 code units without NUL'
    );
  if (!['min', 'low', 'default', 'high'].includes(input.importance as string))
    throw new TypeError('Channel importance must be min, low, default or high');
  return {
    id,
    name,
    description,
    importance: input.importance as MuonAndroidNotificationImportance,
  };
};

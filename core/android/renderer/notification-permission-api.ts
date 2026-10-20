// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import type { MuonWebViewRpcClient } from './webview-rpc.js';
import { notificationIdentifier } from '../../common/renderer/notifications.js';
import {
  normalizeMuonAndroidNotificationChannel,
  type MuonAndroidNotificationCapabilities,
  type MuonAndroidNotificationChannel,
  type MuonAndroidNotificationChannelOptions,
} from './notification-channels.js';
export type {
  MuonAndroidNotificationCapabilities,
  MuonAndroidNotificationChannel,
  MuonAndroidNotificationChannelOptions,
  MuonAndroidNotificationImportance,
} from './notification-channels.js';

/** Current notification permission observations; not a permanent-denial diagnosis. */
export interface MuonAndroidNotificationPermissionState {
  /** API <33 needs no runtime permission; denied includes never requested. */
  readonly runtimePermission: 'not-required' | 'granted' | 'denied';
  /** Whether Android currently permits notifications for this application. */
  readonly notificationsEnabled: boolean;
  /** Whether Muon invoked the OS request API before; does not prove a dialog appeared. */
  readonly requestedBefore: boolean;
  /** Android's current recommendation to explain the permission before requesting. */
  readonly shouldShowRationale: boolean;
  /**
   * Whether directing the user to settings may help; not proof of permanent denial.
   * @remarks True when notifications are disabled and runtime permission is not
   * needed or already granted, or when a previous request has no rationale flag.
   */
  readonly settingsSuggested: boolean;
}

/** Observations after a notification permission request settles. */
export interface MuonAndroidNotificationPermissionResult {
  /** Fresh OS permission and app-wide notification state. */
  readonly state: MuonAndroidNotificationPermissionState;
  /**
   * Limited outcome provided by the OS callback.
   * @remarks Interrupted means an empty permission result. Not-granted does
   * not distinguish denial from every dismissal or suppressed dialog.
   */
  readonly result: 'granted' | 'not-granted' | 'interrupted' | 'not-required';
}

/** Android notification permission operations, exposed only when notifications are enabled. */
export interface MuonAndroidNotificationsApi {
  /**
   * Reads availability of Android-only notification features.
   * @returns Channel support independently of user permission.
   */
  readonly getCapabilities: () => Promise<MuonAndroidNotificationCapabilities>;
  /**
   * Registers a channel, preserving existing OS and user settings.
   * @param options - New channel definition; existing channels are left unchanged.
   * @returns Current effective channel settings; rejects on API 24/25.
   */
  readonly ensureChannel: (
    options: MuonAndroidNotificationChannelOptions
  ) => Promise<MuonAndroidNotificationChannel>;
  /**
   * Reads the current OS channel settings.
   * @param id - Category/channel ID to query.
   * @returns The channel or null when absent; rejects on API 24/25.
   */
  readonly getChannel: (
    id: string
  ) => Promise<MuonAndroidNotificationChannel | null>;
  /**
   * Reads current OS state without requesting permission.
   * @returns Fresh runtime permission and app-wide notification observations.
   */
  readonly getPermissionState: () => Promise<MuonAndroidNotificationPermissionState>;
  /**
   * Explicitly requests notification permission from the foreground Activity.
   * @returns State after the limited OS outcome; API <33 returns not-required.
   * @remarks Only one request can be pending. Rejects if the Activity is not
   * available or foreground, or when its page context is destroyed. No request
   * runs automatically at startup or when a notification is posted.
   */
  readonly requestPermission: () => Promise<MuonAndroidNotificationPermissionResult>;
}

/**
 * Creates Android notification permission operations for an authorized page.
 * @param client - Page-owned RPC client.
 * @param capabilityId - Token authorizing Android notification operations.
 * @returns Permission operations with native JSON conversion.
 */
export const createMuonAndroidNotificationsApi = (
  client: MuonWebViewRpcClient,
  capabilityId: string
): MuonAndroidNotificationsApi => {
  const call = async <T>(
    name: string,
    args: readonly unknown[]
  ): Promise<T> => {
    const value = await client.call(
      capabilityId,
      `muon.android.notifications.${name}`,
      args
    );
    if (typeof value !== 'string')
      throw new TypeError('Android native JSON result is invalid');
    return JSON.parse(value) as T;
  };
  return Object.freeze({
    getCapabilities: async () =>
      await call<MuonAndroidNotificationCapabilities>('getCapabilities', []),
    ensureChannel: async (options: MuonAndroidNotificationChannelOptions) =>
      await call<MuonAndroidNotificationChannel>('ensureChannel', [
        JSON.stringify(normalizeMuonAndroidNotificationChannel(options)),
      ]),
    getChannel: async (id: string) =>
      await call<MuonAndroidNotificationChannel | null>('getChannel', [
        notificationIdentifier(id, 'Channel id'),
      ]),
    getPermissionState: async () =>
      await call<MuonAndroidNotificationPermissionState>(
        'getPermissionState',
        []
      ),
    requestPermission: async () =>
      await call<MuonAndroidNotificationPermissionResult>(
        'requestPermission',
        []
      ),
  });
};

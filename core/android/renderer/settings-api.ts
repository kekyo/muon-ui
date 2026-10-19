// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import type { MuonWebViewRpcClient } from './webview-rpc.js';

/** System settings screens scoped to this application. */
export type MuonAndroidSettingsTarget = 'app-details' | 'notifications';

/** Explicit fallback when a dedicated settings screen is unavailable. */
export interface MuonAndroidSettingsOptions {
  /** Permit opening app information instead of the requested screen. */
  readonly fallback?: 'app-details';
}

/** Accepted settings launch, without a claim about the user's changes. */
export interface MuonAndroidSettingsResult {
  /** Screen requested by the caller. */
  readonly requestedTarget: MuonAndroidSettingsTarget;
  /** Screen for which Android accepted the launch request. */
  readonly target: MuonAndroidSettingsTarget;
  /** Whether the explicitly permitted fallback was used. */
  readonly fallbackUsed: boolean;
}

/** Access to the application's Android settings screens. */
export interface MuonAndroidSettingsApi {
  /**
   * Checks whether Android can resolve this screen for the current app.
   * @param target - Screen to check, without applying fallback.
   * @returns Current availability; a later launch can still fail.
   */
  readonly isAvailable: (target: MuonAndroidSettingsTarget) => Promise<boolean>;
  /**
   * Opens an Android settings screen from the foreground Activity.
   * @param target - Screen requested for this app.
   * @param options - Explicit fallback policy; no fallback by default.
   * @returns The accepted destination, not a settings-change result.
   * @remarks Read permission and system bar state again when the app resumes.
   * Rejects when the Activity or destination is unavailable.
   */
  readonly open: (
    target: MuonAndroidSettingsTarget,
    options?: MuonAndroidSettingsOptions
  ) => Promise<MuonAndroidSettingsResult>;
}

const request = (
  target: MuonAndroidSettingsTarget,
  options: MuonAndroidSettingsOptions | undefined
): string => {
  if (target !== 'app-details' && target !== 'notifications')
    throw new TypeError('Unsupported Android settings target');
  if (
    options !== undefined &&
    (options === null || typeof options !== 'object' || Array.isArray(options))
  )
    throw new TypeError('Android settings options must be an object');
  for (const key of Object.keys(options ?? {})) {
    if (key !== 'fallback')
      throw new TypeError(`Unknown Android settings option: ${key}`);
  }
  if (options?.fallback !== undefined && options.fallback !== 'app-details')
    throw new TypeError('Android settings fallback must be app-details');
  return JSON.stringify({ target, ...options });
};

/**
 * Creates settings operations for an authorized page.
 * @param client - Page-owned RPC client.
 * @param capabilityId - Token authorizing the exposed settings functions.
 * @returns Functions converting public arguments to native requests.
 */
export const createMuonAndroidSettingsApi = (
  client: MuonWebViewRpcClient,
  capabilityId: string
): MuonAndroidSettingsApi =>
  Object.freeze({
    isAvailable: async (
      target: MuonAndroidSettingsTarget
    ): Promise<boolean> => {
      const value = await client.call(
        capabilityId,
        'muon.android.settings.isAvailable',
        [request(target, undefined)]
      );
      if (typeof value !== 'boolean')
        throw new TypeError('Android settings availability is invalid');
      return value;
    },
    open: async (
      target: MuonAndroidSettingsTarget,
      options?: MuonAndroidSettingsOptions
    ): Promise<MuonAndroidSettingsResult> => {
      const value = await client.call(
        capabilityId,
        'muon.android.settings.open',
        [request(target, options)]
      );
      if (typeof value !== 'string')
        throw new TypeError('Android native JSON result is invalid');
      return JSON.parse(value) as MuonAndroidSettingsResult;
    },
  });

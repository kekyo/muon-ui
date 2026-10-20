// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { createMuonNotificationsApi } from '../../common/renderer/notifications.js';

/**
 * Adapts the shared notification API to CEF's native argument ABI.
 * @param call - Authorized CEF transport for serialized native operations.
 * @returns The shared public API using CEF's page-owned watch lease.
 * @remarks The browser process replaces the owner placeholder with the
 * authenticated renderer owner; caller-supplied owner identities are ignored.
 */
export const createMuonCefNotificationsApi = (
  call: Parameters<typeof createMuonNotificationsApi>[0]
): ReturnType<typeof createMuonNotificationsApi> =>
  createMuonNotificationsApi(
    async (name, args) =>
      await call(name, name === 'watchEvents' ? [...args, ''] : args)
  );

/**
 * Installs permitted public operations over a CEF native namespace.
 * @param namespace - Native functions supplied by the built-in metadata.
 * @param isAllowed - Tests the current application's function allow policy.
 * @returns After the permitted public operations are installed.
 * @remarks Releasing a watch lease completes its held next request. CEF does
 * not pass the shared renderer's AbortSignal through its typed C ABI.
 */
export const installMuonCefNotificationsApi = (
  namespace: Record<string, unknown>,
  isAllowed: (name: string) => boolean
): void => {
  const native = new Map<
    string,
    (...args: readonly unknown[]) => Promise<unknown>
  >();
  for (const name of Object.getOwnPropertyNames(namespace)) {
    const value = namespace[name];
    if (name.startsWith('__') && typeof value === 'function') {
      native.set(
        name.slice(2),
        value as (...args: readonly unknown[]) => Promise<unknown>
      );
    }
  }
  const api = createMuonCefNotificationsApi(async (name, args) => {
    const call = native.get(name);
    if (!call || !isAllowed(name))
      throw new Error(`Notification function is not allowed: ${name}`);
    return await call(...args);
  });
  for (const [name, value] of Object.entries(api)) {
    if (isAllowed(name))
      Object.defineProperty(namespace, name, { value, enumerable: true });
  }
};

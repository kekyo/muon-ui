// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import {
  createMuonServicesApi,
  type MuonServiceOperation,
} from '../../common/renderer/services.ts';
export { createMuonServicesApi };

/**
 * Installs public service operations over CEF native transport metadata.
 * @param namespace - Native service function namespace.
 * @param isAllowed - Checks public function permissions for this page.
 * @returns After available operations have been installed.
 * @remarks The browser process supplies the authenticated owner argument.
 * Acquisition grants communication and release only for the owned handle.
 */
export const installMuonCefServicesApi = (
  namespace: Record<string, unknown>,
  isAllowed: (name: string) => boolean
): void => {
  const native = new Map<
    MuonServiceOperation,
    (json: string, owner: string) => Promise<unknown>
  >();
  for (const name of [
    'acquire',
    'release',
    'getState',
    'setAutostart',
    'watchState',
    'rpc',
  ] as const) {
    const value = namespace['__' + name];
    if (typeof value === 'function')
      native.set(
        name,
        value as (json: string, owner: string) => Promise<unknown>
      );
  }
  const api = createMuonServicesApi(async (name, json) => {
    const call = native.get(name);
    if (
      !call ||
      !isAllowed(name === 'release' || name === 'rpc' ? 'acquire' : name)
    )
      throw new Error('Service function is not allowed: ' + name);
    return await call(json, '');
  });
  for (const [name, value] of Object.entries(api))
    if (isAllowed(name))
      Object.defineProperty(namespace, name, { value, enumerable: true });
};

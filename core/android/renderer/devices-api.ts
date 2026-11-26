// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import type { MuonWebViewRpcClient } from './webview-rpc.js';

/** Android USB device as observed during this attachment. */
export interface MuonAndroidUsbDevice {
  /** Opaque ID that expires when this attachment ends. */
  readonly connectionId: string;
  /** USB vendor and product identifiers from the device descriptor. */
  readonly vendorId: number;
  /** USB product identifier. */
  readonly productId: number;
  /** Manufacturer-provided product name, when readable. */
  readonly productName: string | null;
  /** Serial number, available only after permission and when supplied. */
  readonly serialNumber: string | null;
  /** USB device class. Composite devices can report zero. */
  readonly deviceClass: number;
  /** Classes of the interfaces offered by the attached device. */
  readonly interfaceClasses: readonly number[];
  /** Whether the application currently has USB permission for this device. */
  readonly permitted: boolean;
}

/** Runtime permissions declared by the application manifest. */
export interface MuonAndroidPermissionsApi {
  /** Read current grants for the supplied Android permission names.
   * @param names - Fully qualified names from the application manifest.
   * @returns A map from each requested name to its current grant.
   */
  readonly getState: (
    names: readonly string[]
  ) => Promise<Readonly<Record<string, boolean>>>;
  /** Request runtime grants together from the foreground Activity.
   * @param names - Fully qualified names from the application manifest.
   * @returns Current grants after the OS responds. Rejects on interruption.
   */
  readonly request: (
    names: readonly string[]
  ) => Promise<Readonly<Record<string, boolean>>>;
}

/** Observe USB attachments and explicitly ask the user to grant access. */
export interface MuonAndroidUsbApi {
  /** List current devices without starting a foreground service.
   * @returns One entry per attachment, including devices without permission.
   */
  readonly listDevices: () => Promise<readonly MuonAndroidUsbDevice[]>;
  /** Ask Android to grant the application access to one attachment.
   * @param connectionId - Opaque ID returned by listDevices.
   * @returns The grant state. Rejects if the device detached.
   */
  readonly requestPermission: (connectionId: string) => Promise<boolean>;
}

const readJson = async <T>(
  client: MuonWebViewRpcClient,
  id: string,
  path: string,
  args: readonly string[]
): Promise<T> => {
  const value = await client.call(id, path, args);
  if (typeof value !== 'string')
    throw new TypeError('Android device result must be JSON');
  return JSON.parse(value) as T;
};

/** Create the runtime permission API for one page.
 * @param client - Document-owned RPC client.
 * @param id - Granted permission capability.
 * @returns Permission queries and explicit foreground requests.
 */
export const createMuonAndroidPermissionsApi = (
  client: MuonWebViewRpcClient,
  id: string
): MuonAndroidPermissionsApi => {
  const call = async (method: string, names: readonly string[]) => {
    if (
      !Array.isArray(names) ||
      !names.length ||
      names.some((name) => typeof name !== 'string' || !name.length)
    )
      throw new TypeError('Expected one or more Android permission names');
    return await readJson<Readonly<Record<string, boolean>>>(
      client,
      id,
      `muon.android.permissions.${method}`,
      [JSON.stringify(names)]
    );
  };
  return Object.freeze({
    getState: (names: readonly string[]) => call('getState', names),
    request: (names: readonly string[]) => call('request', names),
  });
};

/** Create the USB host API for one page.
 * @param client - Document-owned RPC client.
 * @param id - Granted USB capability.
 * @returns Attachment enumeration and explicit permission requests.
 */
export const createMuonAndroidUsbApi = (
  client: MuonWebViewRpcClient,
  id: string
): MuonAndroidUsbApi =>
  Object.freeze({
    listDevices: async () =>
      await readJson<readonly MuonAndroidUsbDevice[]>(
        client,
        id,
        'muon.android.usb.listDevices',
        []
      ),
    requestPermission: async (connectionId: string) => {
      if (typeof connectionId !== 'string' || !connectionId)
        throw new TypeError('Expected a USB attachment ID');
      return await readJson<boolean>(
        client,
        id,
        'muon.android.usb.requestPermission',
        [connectionId]
      );
    },
  });

// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

/** A copied service observation that does not keep its runtime alive. */
export interface MuonServiceState {
  /** Logical identity declared in muon.json. */
  readonly id: string;
  /** Actual runtime state, independently of the autostart preference. */
  readonly state: 'stopped' | 'starting' | 'running' | 'stopping' | 'failed';
  /** Decimal generation identity; "0" means the runtime has never started. */
  readonly generation: string;
  /** Decimal identity of the last committed autostart setting operation. */
  readonly revision: string;
  /** Effective persistent preference, falling back to the application default. */
  readonly autostart: boolean;
  /** Whether the host currently owns the single resident reference. */
  readonly resident: boolean;
  /** Number of page references, including acquisitions still starting. */
  readonly clientReferences: number;
  /** Last runtime failure, or an empty string when there is no failure. */
  readonly error: string;
}

/** One page-owned reference to an acquired service generation. */
export interface MuonServiceHandle {
  /** Configured service identity. */
  readonly id: string;
  /** Decimal generation identity, preserved without conversion to a JS number. */
  readonly generation: string;
  /**
   * Releases only this reference, leaving other pages and resident policy intact.
   * @returns Completion of reference release; repeated calls share this promise.
   * @remarks Page destruction also reclaims references through the native owner.
   * Releasing the final reference requests shutdown but does not await disposal.
   */
  readonly release: () => Promise<void>;
}

/** Configured application services with lifetimes independent of their UI pages. */
export interface MuonServicesApi {
  /**
   * Reserves a reference and starts the service when necessary.
   * @param id - Logical service identity declared by the application.
   * @returns A handle after startup succeeds; rejects on startup failure.
   * @remarks Every successful call owns a distinct reference. Reopening a page
   * can acquire an existing generation retained by the host or another page.
   */
  readonly acquire: (id: string) => Promise<MuonServiceHandle>;
  /**
   * Reads current runtime and desired lifetime without creating a reference.
   * @param id - Logical service identity declared by the application.
   * @returns A copied state observation.
   */
  readonly getState: (id: string) => Promise<MuonServiceState>;
}

/** Internal native operation names, interpreted with the authenticated page owner. */
export type MuonServiceOperation = 'acquire' | 'release' | 'getState';

const checkedId = (id: string): string => {
  if (typeof id !== 'string' || !/^[A-Za-z][A-Za-z0-9_.-]{0,63}$/.test(id))
    throw new TypeError('Invalid service identity');
  return id;
};

const readObject = (json: unknown): Record<string, unknown> => {
  if (typeof json !== 'string') throw new Error('Invalid native service response');
  const value: unknown = JSON.parse(json);
  if (value === null || typeof value !== 'object' || Array.isArray(value))
    throw new Error('Invalid native service response');
  return value as Record<string, unknown>;
};

const decimal = (value: unknown, allowZero: boolean): value is string =>
  typeof value === 'string' &&
  /^(0|[1-9][0-9]{0,19})$/.test(value) &&
  (allowZero || value !== '0') &&
  BigInt(value) <= 18446744073709551615n;

/**
 * Creates the page facade shared by desktop and Android native transports.
 * @param call - Authorized native operation accepting one JSON string and
 * returning its JSON result, with transport/runtime errors rejected.
 * @returns Operations whose handles hide their native reference identity.
 * @remarks Native transports own page destruction and process-disconnect cleanup;
 * this facade never relies on garbage collection or unload event delivery.
 */
export const createMuonServicesApi = (
  call: (name: MuonServiceOperation, json: string) => Promise<unknown>
): MuonServicesApi => ({
  acquire: async (id) => {
    const serviceId = checkedId(id);
    const value = readObject(await call('acquire', JSON.stringify({ serviceId })));
    if (
      value.serviceId !== serviceId ||
      !decimal(value.refId, false) ||
      !decimal(value.generation, false)
    )
      throw new Error('Invalid native service reference');
    const generation = value.generation;
    const reference = JSON.stringify({
      serviceId,
      refId: value.refId,
      generation,
    });
    let releasing: Promise<void> | undefined;
    return Object.freeze({
      id: serviceId,
      generation,
      release: () => {
        releasing ??= (async () => {
          await call('release', reference);
        })();
        return releasing;
      },
    });
  },
  getState: async (id) => {
    const serviceId = checkedId(id);
    const value = readObject(await call('getState', JSON.stringify({ serviceId })));
    if (
      value.id !== serviceId ||
      !['stopped', 'starting', 'running', 'stopping', 'failed'].includes(
        value.state as string
      ) ||
      !decimal(value.generation, true) ||
      !decimal(value.revision, true) ||
      typeof value.autostart !== 'boolean' ||
      typeof value.resident !== 'boolean' ||
      !Number.isSafeInteger(value.clientReferences) ||
      Number(value.clientReferences) < 0 ||
      typeof value.error !== 'string'
    )
      throw new Error('Invalid native service state');
    return Object.freeze(value) as unknown as MuonServiceState;
  },
});

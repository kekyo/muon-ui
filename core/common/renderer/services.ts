// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { createServiceConnection } from '../../../sdk/src/service-connection.ts';
import type { ServiceConnection } from '../../../sdk/src/service-connection.ts';
import { parseServiceInterface } from '../../../sdk/src/service-interface.ts';
import type { ServiceInterface } from '../../../sdk/src/service-interface.ts';

/** An OS-retained exit of the shared host, not a failure of the current service. */
export interface MuonServiceHostExit {
  /** Platform that supplied the historical observation. */
  readonly platform: 'android';
  /** Exact process name of the previous shared service host. */
  readonly processName: string;
  /** PID of the process that ended; unrelated to the current generation. */
  readonly pid: number;
  /** OS wall-clock time in milliseconds since Unix epoch, as a decimal string. */
  readonly timestampMs: string;
  /** Android reason name in lower-case hyphenated form; unknown codes use "unknown". */
  readonly reason: string;
  /** Original ApplicationExitInfo reason code, including future unknown values. */
  readonly reasonCode: number;
  /** Original exit status or signal number reported by Android. */
  readonly status: number;
  /** Optional OS diagnostic text, without a stable or localized format. */
  readonly description: string | null;
}

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
  /**
   * Most recent retained OS record for this shared host, read at host startup.
   * @remarks Null on unsupported platforms, before Android API 30, or when no
   * record can be read. A record does not imply that this logical service was
   * running then, and never changes the current state, generation, or error.
   */
  readonly lastHostExit: MuonServiceHostExit | null;
}

/** One page-owned reference to an acquired service generation. */
export interface MuonServiceHandle {
  /** Configured service identity. */
  readonly id: string;
  /** Decimal generation identity, preserved without conversion to a JS number. */
  readonly generation: string;
  /** Validated runtime method and event definitions for this generation. */
  readonly interface: ServiceInterface;
  /** Invokes a declared method; callbacks and cancellation belong to this call. */
  readonly call: ServiceConnection['call'];
  /** Creates a FIFO event subscription owned by this reference. */
  readonly subscribe: ServiceConnection['subscribe'];
  /**
   * Cancels this handle's calls and subscriptions and releases its reference.
   * @returns Completion of reference release; repeated calls share this promise.
   * @remarks Page destruction also reclaims references through the native owner.
   * Releasing the final reference requests shutdown but does not await disposal.
   */
  readonly release: () => Promise<void>;
}

/** A non-owning state observer, automatically removed when its page disappears. */
export interface MuonServiceWatcher {
  /**
   * Stops observation and settles any pending state request.
   * @returns Completion of removal; repeated calls share this promise.
   * @remarks Does not release service references or change autostart settings.
   */
  readonly close: () => Promise<void>;
}

/**
 * Receives the initial service state and subsequent coalesced observations.
 * @param state - Copied current state; retaining it does not keep a service alive.
 * @returns Optional asynchronous processing, during which changes are coalesced.
 */
export type MuonServiceStateListener = (state: MuonServiceState) => void | Promise<void>;

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
  /**
   * Persists the user preference and updates the host's resident reference.
   * @param id - Logical service identity declared by the application.
   * @param enabled - Whether the host should retain one resident reference.
   * @returns After persistence and policy update, without waiting for runtime stop.
   * @remarks OFF preserves every other page's reference. Startup failures remain
   * visible in getState/watchState independently of this saved preference.
   */
  readonly setAutostart: (id: string, enabled: boolean) => Promise<void>;
  /**
   * Observes service state without acquiring a runtime reference.
   * @param id - Logical service identity declared by the application.
   * @param listener - Receives initial state after this method returns, then changes.
   * @returns A page-owned observer with idempotent asynchronous close.
   * @remarks Slow listeners receive the latest state, not a history of transitions.
   * Listener failures are logged; a disconnected host closes the observation.
   */
  readonly watchState: (id: string, listener: MuonServiceStateListener) => Promise<MuonServiceWatcher>;
}

/** Internal native operation names, interpreted with the authenticated page owner. */
export type MuonServiceOperation = 'acquire' | 'release' | 'getState' | 'setAutostart' | 'watchState' | 'rpc';

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

const checkedHostExit = (value: unknown): MuonServiceHostExit | null => {
  if (value === undefined || value === null) return null;
  if (typeof value !== 'object' || Array.isArray(value)) throw new Error('Invalid service host exit');
  const exit = value as Record<string, unknown>;
  if (exit.platform !== 'android' || typeof exit.processName !== 'string' || !exit.processName ||
    !Number.isInteger(exit.pid) || Number(exit.pid) <= 0 || Number(exit.pid) > 2147483647 ||
    !decimal(exit.timestampMs, true) || typeof exit.reason !== 'string' || !exit.reason ||
    !Number.isInteger(exit.reasonCode) || Number(exit.reasonCode) < 0 || Number(exit.reasonCode) > 2147483647 ||
    !Number.isInteger(exit.status) || Number(exit.status) < -2147483648 || Number(exit.status) > 2147483647 ||
    (exit.description !== null && typeof exit.description !== 'string'))
    throw new Error('Invalid service host exit');
  return Object.freeze({
    platform: 'android', processName: exit.processName, pid: Number(exit.pid),
    timestampMs: exit.timestampMs, reason: exit.reason, reasonCode: Number(exit.reasonCode),
    status: Number(exit.status), description: exit.description,
  });
};

const checkedState = (value: unknown, serviceId: string): MuonServiceState => {
  if (value === null || typeof value !== 'object' || Array.isArray(value))
    throw new Error('Invalid native service state');
  const state = value as Record<string, unknown>;
  if (
    state.id !== serviceId ||
    !['stopped', 'starting', 'running', 'stopping', 'failed'].includes(state.state as string) ||
    !decimal(state.generation, true) || !decimal(state.revision, true) ||
    typeof state.autostart !== 'boolean' || typeof state.resident !== 'boolean' ||
    !Number.isSafeInteger(state.clientReferences) || Number(state.clientReferences) < 0 ||
    typeof state.error !== 'string'
  ) throw new Error('Invalid native service state');
  return Object.freeze({ ...state, lastHostExit: checkedHostExit(state.lastHostExit) }) as unknown as MuonServiceState;
};

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
    const identity = {
      serviceId,
      refId: value.refId,
      generation,
    };
    const reference = JSON.stringify(identity);
    const rpc = async (action: string, payload: Readonly<Record<string, unknown>>): Promise<unknown> => {
      const json = await call('rpc', JSON.stringify({ ...identity, action, payload }));
      if (typeof json !== 'string') throw new Error('Invalid native service response');
      return JSON.parse(json);
    };
    let definition: ServiceInterface;
    try {
      definition = parseServiceInterface(await rpc('describe', {}));
    } catch (error) {
      // A successful acquisition must not leak when its interface is invalid.
      try { await call('release', reference); }
      catch { /* Native owner destruction also reclaims a disconnected reference. */ }
      throw error;
    }
    const connection = createServiceConnection(definition, rpc);
    let releasing: Promise<void> | undefined;
    return Object.freeze({
      id: serviceId,
      generation,
      interface: definition,
      call: connection.call,
      subscribe: connection.subscribe,
      release: () => {
        releasing ??= (async () => {
          // Stop local admission before releasing remotely. Native release also
          // settles pending receives, including calls still waiting for an ACK.
          const closed = connection.close();
          await Promise.all([closed, call('release', reference)]);
        })();
        return releasing;
      },
    });
  },
  getState: async (id) => {
    const serviceId = checkedId(id);
    return checkedState(readObject(await call('getState', JSON.stringify({ serviceId }))), serviceId);
  },
  setAutostart: async (id, enabled) => {
    const serviceId = checkedId(id);
    if (typeof enabled !== 'boolean') throw new TypeError('enabled must be boolean');
    await call('setAutostart', JSON.stringify({ serviceId, enabled }));
  },
  watchState: async (id, listener) => {
    const serviceId = checkedId(id);
    if (typeof listener !== 'function') throw new TypeError('listener must be a function');
    const initial = readObject(await call('watchState', JSON.stringify({ serviceId, operation: 'acquire' })));
    if (!decimal(initial.token, false)) throw new Error('Invalid native service observation');
    const token = initial.token;
    const state = checkedState(initial.state, serviceId);
    let closed = false;
    let closing: Promise<void> | undefined;
    const close = (): Promise<void> => {
      closed = true;
      closing ??= (async () => {
        await call('watchState', JSON.stringify({ serviceId, operation: 'release', token }));
      })();
      return closing;
    };
    // Defer the initial callback until its owner has received the close handle.
    // Each next request waits at the host; there is no timer-based state polling.
    setTimeout(async () => {
      let current = state;
      try {
        while (!closed) {
          try { await listener(current); }
          catch (error) { console.error('Muon service state listener failed', error); }
          if (closed) break;
          const result = readObject(await call('watchState', JSON.stringify({ serviceId, operation: 'next', token })));
          if (closed || result.state === null) break;
          current = checkedState(result.state, serviceId);
        }
      } catch (error) {
        if (!closed) console.error('Muon service state observation failed', error);
      } finally {
        try { await close(); }
        catch (error) { console.error('Muon service state observation cleanup failed', error); }
      }
    }, 0);
    return Object.freeze({ close });
  },
});

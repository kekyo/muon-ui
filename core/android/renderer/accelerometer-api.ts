// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import type { MuonWebViewRpcClient } from './webview-rpc.js';

/** Latest gravity-inclusive reading in the device's natural orientation. */
export interface MuonAndroidAccelerationSample {
  /** Measurement time in microseconds since boot, including sleep. */
  readonly elapsedUs: number;
  /** Native callback time on the same clock, in microseconds. */
  readonly receivedElapsedUs: number;
  /** Acceleration to the right in metres per second squared. */
  readonly x: number;
  /** Acceleration toward the top in metres per second squared. */
  readonly y: number;
  /** Acceleration out of the screen in metres per second squared. */
  readonly z: number;
  /** Android sensor accuracy indicator. */
  readonly accuracy: number;
}

/** Current ownership, sampling state and most recent foreground measurement. */
export interface MuonAndroidAccelerationSnapshot {
  /** Monotonically increasing change number, scoped to this Activity. */
  readonly revision: number;
  /** A requested and suspended sensor resumes when the Activity returns. */
  readonly status: {
    /** Current registration state. */
    readonly state:
      'stopped' | 'suspended' | 'active' | 'unavailable' | 'failed';
    /** Stable failure code when acquisition cannot proceed. */
    readonly error: string | null;
    /** Requested sampling interval in microseconds. */
    readonly intervalUs: number;
  };
  /** Latest sample since the last state transition, or null before the first reading. */
  readonly sample: MuonAndroidAccelerationSample | null;
}

/** Page-owned foreground access to the device accelerometer. */
export interface MuonAndroidAccelerometerApi {
  /** Read current ownership without registering a listener.
   * @returns The latest snapshot.
   */
  readonly getState: () => Promise<MuonAndroidAccelerationSnapshot>;
  /** Request readings at approximately 10 Hz while the Activity is resumed.
   * @returns The resulting state. Repeated requests share one registration.
   */
  readonly start: () => Promise<MuonAndroidAccelerationSnapshot>;
  /** Release this document's registration, including its request to resume later.
   * @returns The resulting snapshot.
   */
  readonly stop: () => Promise<MuonAndroidAccelerationSnapshot>;
  /** Wait for the next state transition or reading. A slow reader receives the latest snapshot.
   * @param revision - Last revision observed by this document.
   * @param options - Optional cancellation of this wait.
   * @returns The latest snapshot after the supplied revision.
   */
  readonly waitForChange: (
    revision: number,
    options?: { signal?: AbortSignal }
  ) => Promise<MuonAndroidAccelerationSnapshot>;
}

/** Build an accelerometer client for the document's capability.
 * @param client - Document-owned RPC transport.
 * @param id - Capability supplied by the runtime.
 * @returns Foreground accelerometer operations.
 */
export const createMuonAndroidAccelerometerApi = (
  client: MuonWebViewRpcClient,
  id: string
): MuonAndroidAccelerometerApi => {
  const call = async (
    name: string,
    args: readonly unknown[],
    options: { signal?: AbortSignal } | undefined
  ) => {
    const value = await client.call(
      id,
      'muon.android.accelerometer.' + name,
      args,
      options
    );
    if (typeof value !== 'string')
      throw new TypeError('Expected Android accelerometer snapshot');
    return JSON.parse(value) as MuonAndroidAccelerationSnapshot;
  };
  return Object.freeze({
    getState: () => call('getState', [], undefined),
    start: () => call('start', [], undefined),
    stop: () => call('stop', [], undefined),
    waitForChange: async (
      revision: number,
      options?: { signal?: AbortSignal }
    ) => {
      if (!Number.isSafeInteger(revision) || revision < 0)
        throw new TypeError('Invalid accelerometer revision');
      return await call('waitForChange', [revision], options);
    },
  });
};

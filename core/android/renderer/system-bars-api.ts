// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import type { MuonWebViewRpcClient } from './webview-rpc.js';
import {
  normalizeMuonAndroidSystemBarsOptions,
  type MuonAndroidContentLayout,
  type MuonAndroidSystemBarIcons,
  type MuonAndroidSystemBarsOptions,
} from '../system-bars-config.js';
export type {
  MuonAndroidContentLayout,
  MuonAndroidSystemBarIcons,
  MuonAndroidSystemBarOptions,
  MuonAndroidSystemBarsOptions,
} from '../system-bars-config.js';

/** Requested layout and requested and OS-reported Android system bar appearance. */
export interface MuonAndroidSystemBarsState {
  /** Application requests, unaffected by transient system bar reveals. */
  readonly requested: {
    /** Requested web content placement. */
    readonly contentLayout: MuonAndroidContentLayout;
    /** Status bar request. */
    readonly statusBar: {
      /** Whether the application requests the bar to remain visible. */
      readonly visible: boolean;
      /** Requested icon color, or null when inherited from the theme. */
      readonly icons: MuonAndroidSystemBarIcons | null;
    };
    /** Navigation bar request. */
    readonly navigationBar: {
      /** Whether the application requests the bar to remain visible. */
      readonly visible: boolean;
      /** Requested icon color, or null when inherited from the theme. */
      readonly icons: MuonAndroidSystemBarIcons | null;
    };
  };
  /**
   * Visibility and icon appearance reported by Android's public window APIs.
   * @remarks Null means insets are not available yet. On API 29 and earlier,
   * AndroidX reports an approximation of the platform visibility. Transient
   * bars revealed by swiping may still be reported as hidden; this state does
   * not guarantee detection of their on-screen appearance.
   * @see https://developer.android.com/reference/android/view/WindowInsets#isVisible(int)
   * @see https://android.googlesource.com/platform/frameworks/base/+/refs/heads/main/core/java/android/view/InsetsController.java
   */
  readonly actual: {
    /** Status bar state reported by Android. */
    readonly statusBar: {
      /** Whether Android reports the bar as visible; null before insets are available. */
      readonly visible: boolean | null;
      /** Icon appearance reported by Android, or null before insets are available. */
      readonly icons: MuonAndroidSystemBarIcons | null;
    };
    /** Navigation bar state reported by Android. */
    readonly navigationBar: {
      /** Whether Android reports the bar as visible; null before insets are available. */
      readonly visible: boolean | null;
      /** Icon appearance reported by Android, or null before insets are available. */
      readonly icons: MuonAndroidSystemBarIcons | null;
    };
  };
}

/** Android system bar operations. */
export interface MuonAndroidSystemBarsApi {
  /**
   * Updates bar visibility, icon colors, and web content placement.
   * @param options - Partial configuration; omitted settings retain their request.
   * @returns Completion of the request, before any OS animation finishes.
   * @remarks Unsupported keys and invalid values are rejected. Hidden bars
   * remain available through the system's transient swipe gesture.
   */
  readonly configure: (options: MuonAndroidSystemBarsOptions) => Promise<void>;
  /**
   * Reads requested layout and bar appearance without changing the request.
   * @returns Current system bar state, including unknown observations.
   */
  readonly getState: () => Promise<MuonAndroidSystemBarsState>;
  /**
   * Read the current geometry after the first window insets become available.
   * @returns Insets in CSS pixels at the current page scale.
   */
  readonly getInsets: () => Promise<MuonAndroidSystemBarsInsets>;
  /**
   * Subscribe to the initial geometry and subsequent changes without polling.
   * @param listener - Receives CSS-pixel snapshots serially.
   * @returns A handle that releases the subscription and cancels its pending wait.
   * @remarks The first callback runs after the caller receives the handle.
   * Changes may be coalesced while the listener is running. Listener
   * errors are logged and do not stop later updates. Transport errors are logged
   * and close the subscription. Page destruction also releases subscriptions.
   */
  readonly watchInsets: (
    listener: MuonAndroidInsetsListener
  ) => Promise<MuonAndroidInsetsWatcher>;
}

/** Insets measured from each edge, in CSS pixels. */
export interface MuonAndroidInsets {
  /** Distance from the top edge. */
  readonly top: number;
  /** Distance from the right edge. */
  readonly right: number;
  /** Distance from the bottom edge. */
  readonly bottom: number;
  /** Distance from the left edge. */
  readonly left: number;
}

/** Native insets and the remaining web content clearance at the current scale. */
export interface MuonAndroidSystemBarsInsets {
  /** Monotonic revision within the current WebView host. */
  readonly revision: number;
  /** Unit of all edge distances after applying the WebView page scale. */
  readonly unit: 'css-px';
  /** Current web content placement policy. */
  readonly contentLayout: MuonAndroidContentLayout;
  /** Insets occupied by visible system bars. */
  readonly systemBars: MuonAndroidInsets;
  /** Insets needed to avoid the display cutout. */
  readonly displayCutout: MuonAndroidInsets;
  /** Areas reserved for system navigation gestures. */
  readonly systemGestures: MuonAndroidInsets;
  /** Software keyboard insets, independent of the safe area. */
  readonly ime: MuonAndroidInsets;
  /** Per-edge maximum of systemBars and displayCutout. */
  readonly safeArea: MuonAndroidInsets;
  /** Remaining clearance for web content; zero in safe-area layout. */
  readonly contentSafeArea: MuonAndroidInsets;
}

/** Receives one insets snapshot, with updates serialized until it settles. */
export type MuonAndroidInsetsListener = (
  insets: MuonAndroidSystemBarsInsets
) => void | Promise<void>;

/** Owns a page-local Android insets subscription. */
export interface MuonAndroidInsetsWatcher {
  /**
   * Release the subscription without waiting for another geometry change.
   * @returns Completion of release; repeated calls share the same operation.
   * @remarks May be awaited from inside a listener. It prevents further listener
   * calls, but does not wait for a listener that is already executing.
   */
  readonly close: () => Promise<void>;
}

const parseJson = <T>(value: unknown): T => {
  if (typeof value !== 'string')
    throw new TypeError('Android native JSON result is invalid');
  return JSON.parse(value) as T;
};

/**
 * Create the Android system bar facade for one authorized page.
 * @param client - Page-owned RPC client.
 * @param capabilityId - Token authorizing the exposed system bar functions.
 * @returns System bar operations with public-to-native argument conversion.
 */
export const createMuonAndroidSystemBarsApi = (
  client: MuonWebViewRpcClient,
  capabilityId: string
): MuonAndroidSystemBarsApi =>
  Object.freeze({
    configure: async (options: MuonAndroidSystemBarsOptions): Promise<void> => {
      await client.call(capabilityId, 'muon.android.systemBars.configure', [
        JSON.stringify(normalizeMuonAndroidSystemBarsOptions(options)),
      ]);
    },
    getState: async (): Promise<MuonAndroidSystemBarsState> =>
      parseJson(
        await client.call(capabilityId, 'muon.android.systemBars.getState', [])
      ),
    getInsets: async (): Promise<MuonAndroidSystemBarsInsets> =>
      parseJson(
        await client.call(capabilityId, 'muon.android.systemBars.getInsets', [])
      ),
    watchInsets: async (
      listener: MuonAndroidInsetsListener
    ): Promise<MuonAndroidInsetsWatcher> => {
      if (typeof listener !== 'function')
        throw new TypeError('listener must be a function');
      const path = 'muon.android.systemBars.watchInsets';
      const lease = parseJson<{
        token: string;
        insets: MuonAndroidSystemBarsInsets;
      }>(
        await client.call(capabilityId, path, [
          JSON.stringify({ operation: 'acquire' }),
        ])
      );
      const pending = new AbortController();
      let closed = false;
      let closePromise: Promise<void> | undefined;
      const close = async (): Promise<void> => {
        if (closePromise === undefined) {
          closed = true;
          pending.abort();
          closePromise = (async () => {
            try {
              await client.call(capabilityId, path, [
                JSON.stringify({ operation: 'release', token: lease.token }),
              ]);
            } catch {
              // Native context destruction also releases every outstanding lease.
            }
          })();
        }
        await closePromise;
      };
      // Defer initial delivery until the caller has received the watcher handle.
      // Subsequent native event waits use no timer. The revision closes the gap
      // between delivering a snapshot and registering the following wait.
      setTimeout(async () => {
        let insets: MuonAndroidSystemBarsInsets | null = lease.insets;
        try {
          while (!closed && insets !== null) {
            try {
              await listener(insets);
            } catch (error) {
              console.error('Muon insets listener failed', error);
            }
            if (closed) break;
            const result: { insets: MuonAndroidSystemBarsInsets | null } =
              parseJson(
                await client.call(
                  capabilityId,
                  path,
                  [
                    JSON.stringify({
                      operation: 'next',
                      token: lease.token,
                      afterRevision: insets.revision,
                    }),
                  ],
                  { signal: pending.signal }
                )
              );
            insets = result.insets;
          }
        } catch (error) {
          if (!closed) console.error('Muon insets subscription failed', error);
        } finally {
          await close();
        }
      }, 0);
      return Object.freeze({ close });
    },
  });

/**
 * Optional globals supplied by the QuickJS service host.
 *
 * @remarks
 * Add `import type {} from 'muon-plugin-sdk/service-globals'` to a service
 * source, or list this entry in `compilerOptions.types`. This entry supplies
 * declarations only; it does not install globals or change an existing host.
 * Ordinary imports from `muon-plugin-sdk/services` do not enable these types.
 */
export {};

// Existing DOM/Node timer declarations retain their own handle type. QuickJS
// does not expose setInterval, so its ES2022-only consumer uses numeric handles.
type ServiceGlobalTimerHandle = typeof globalThis extends {
  setInterval: (...args: never[]) => infer Handle;
}
  ? Handle
  : number;

// Promise<void>'s resolver accepts void, which a timer may supply by omission.
type ServiceTimerArguments<Arguments extends unknown[]> = [
  void,
] extends Arguments
  ? Partial<Arguments>
  : Arguments;

declare global {
  /**
   * Schedule a function once on the service's event loop.
   * @typeParam Arguments - Values passed to the scheduled function.
   * @param callback - Function to invoke after the delay.
   * @param delay - Delay in milliseconds; defaults to zero. The QuickJS host
   * clamps negative/non-finite values to zero and caps the delay at 2147483647.
   * @param args - Up to 32 values passed to the callback.
   * @returns A numeric timer handle in QuickJS, or the existing host's handle.
   * @remarks QuickJS permits at most 1024 pending timers per service instance.
   * Timers belong to that instance and are discarded when its heap is disposed.
   */
  function setTimeout<Arguments extends unknown[]>(
    callback: (...args: Arguments) => void,
    delay?: number,
    ...args: ServiceTimerArguments<Arguments>
  ): ServiceGlobalTimerHandle;

  /**
   * Cancel a pending one-shot timer.
   * @param handle - Timer handle; an unknown or undefined handle is harmless.
   */
  function clearTimeout(
    handle: ServiceGlobalTimerHandle | number | undefined
  ): void;

  /** Diagnostic methods available to a QuickJS service. */
  interface Console {
    /** Write debug diagnostics. @param values - Values converted to strings and joined with spaces. */
    debug(...values: unknown[]): void;
    /** Write general diagnostics. @param values - Values converted to strings and joined with spaces. */
    log(...values: unknown[]): void;
    /** Write informational diagnostics. @param values - Values converted to strings and joined with spaces. */
    info(...values: unknown[]): void;
    /** Write warning diagnostics. @param values - Values converted to strings and joined with spaces. */
    warn(...values: unknown[]): void;
    /** Write error diagnostics. @param values - Values converted to strings and joined with spaces. */
    error(...values: unknown[]): void;
  }

  /** Diagnostics forwarded to the service host, tagged with the service ID. */
  var console: Console;
}

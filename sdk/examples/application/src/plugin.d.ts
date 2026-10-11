declare module 'muon:counter' {
  /**
   * Adds one using the C++/cardio plugin.
   * @param value - Integer whose increment is representable by int32_t.
   * @returns The incremented value after native completion.
   */
  export const increment: (value: number) => Promise<number>;
}
declare module 'muon:vendor_counter' {
  /**
   * Adds one using the C plugin and its external native library.
   * @param value - Integer whose increment is representable by int32_t.
   * @returns The incremented value after native completion.
   */
  export const increment: (value: number) => Promise<number>;
}

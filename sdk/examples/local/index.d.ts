/** Functions exposed by the counter native plugin through muon's bridge. */
export interface counterPlugin {
  /**
   * Adds one asynchronously to the supplied 32-bit integer.
   * @param value - Integer whose increment is representable by int32_t.
   * @returns The incremented value after native completion.
   */
  increment(value: number): Promise<number>;
}

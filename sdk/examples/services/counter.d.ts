declare module 'muon:counter' {
  /** Adds one using the explicitly configured native plugin. */
  export const increment: (value: number) => Promise<number>;
}

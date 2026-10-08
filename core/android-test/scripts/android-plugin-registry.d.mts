/** Android ABI packaged by the prototype application. */
export type AndroidPluginAbi = 'x86_64' | 'arm64-v8a';

/** Canonical string configuration supplied to one plugin. */
export interface NormalizedAndroidPluginConfigEntry {
  /** Plugin-defined configuration key. */
  readonly key: string;
  /** Plugin-defined configuration value. */
  readonly value: string;
}

/** Canonical build and runtime definition of one packaged Android plugin. */
export interface NormalizedAndroidPlugin {
  /** Logical name used in diagnostics and policy configuration. */
  readonly name: string;
  /** ELF soname used by the Android package loader. */
  readonly soname: string;
  /** Repository-relative C++ source used by this integration fixture. */
  readonly source: string;
  /** Final package path for each supported ABI. */
  readonly artifacts: Readonly<Record<AndroidPluginAbi, string>>;
  /** Allowed public plugin function globs. */
  readonly allow: readonly string[];
  /** Plugin configuration sorted by key. */
  readonly config: readonly NormalizedAndroidPluginConfigEntry[];
}

/** Canonical Android plugin registry consumed by all generated outputs. */
export interface NormalizedAndroidPluginRegistry {
  /** Registry schema version. */
  readonly schemaVersion: 1;
  /** Plugins in deterministic startup order. */
  readonly plugins: readonly NormalizedAndroidPlugin[];
}

/** Result returned by an Android ELF artifact inspector. */
export interface AndroidPluginArtifactInspection {
  /** ELF machine description reported by llvm-readelf. */
  readonly machine: string;
  /** DT_SONAME value. */
  readonly soname: string;
  /** Exported dynamic symbol names. */
  readonly exportedSymbols: readonly string[];
  /** Alignment of every ELF LOAD segment. */
  readonly loadAlignments: readonly number[];
}

/** Context passed to an Android plugin artifact inspector. */
export interface AndroidPluginArtifactInspectionContext {
  /** ABI currently being validated. */
  readonly abi: AndroidPluginAbi;
  /** Registry-declared package path. */
  readonly artifactPath: string;
  /** Plugin owning the artifact. */
  readonly plugin: NormalizedAndroidPlugin;
}

/** Inspects one Android plugin artifact synchronously. */
export type AndroidPluginArtifactInspector = (
  context: AndroidPluginArtifactInspectionContext
) => AndroidPluginArtifactInspection;

/**
 * Validates and canonicalizes the muon-owned Android plugin build manifest.
 *
 * @param input Untrusted parsed manifest contents.
 * @returns Canonical registry preserving plugin startup order.
 */
export const normalizeAndroidPluginRegistry: (
  input: unknown
) => NormalizedAndroidPluginRegistry;

/**
 * Verifies all ABI artifacts described by a normalized Android registry.
 *
 * @param registry Canonical registry to inspect.
 * @param inspect Synchronous package artifact inspector.
 */
export const validateAndroidPluginArtifacts: (
  registry: NormalizedAndroidPluginRegistry,
  inspect: AndroidPluginArtifactInspector
) => void;

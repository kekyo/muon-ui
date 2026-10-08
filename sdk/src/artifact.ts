import { createHash } from 'node:crypto';
import {
  copyFile,
  mkdir,
  readFile,
  realpath,
  writeFile,
} from 'node:fs/promises';
import {
  basename,
  dirname,
  isAbsolute,
  join,
  relative,
  resolve,
  sep,
} from 'node:path';
import { inspectPluginBinary, type PluginBinaryInfo } from './binary.js';
import {
  validatePluginLibraryDependencies,
  type PluginLibraryInspection,
} from './dependencies.js';
export { inspectPluginBinary } from './binary.js';
export { validatePluginLibraryDependencies } from './dependencies.js';
import {
  getPluginTarget,
  muonPluginAbiVersion,
  muonPluginNdkVersion,
  muonPluginAndroidApi,
  muonPluginCardioHeaderSha256,
  type MuonPluginTarget,
} from './targets.js';

/** One packaged native file, authenticated after all producer processing. */
export interface PluginArtifactFile {
  /** Path relative to the manifest directory. */ readonly path: string;
  /** SHA-256 digest of the final bytes. */ readonly sha256: string;
}
/** Runtime requirements declared by the producer. */
export interface PluginRuntimeRequirements {
  /** Whether the plugin uses the shared host cardio runtime. */ readonly cardio: boolean;
  /** Cardio build profile identifier, absent when not used. */ readonly cardioAbi?: string;
  /** C++ runtime, absent for pure C. */ readonly cpp?:
    'libstdc++' | 'c++_shared';
  /** NDK release, required for Android. */ readonly ndk?: string;
  /** Minimum Android API, required for Android. */ readonly androidApi?: number;
}
/** Native inputs for one explicitly supported target. */
export interface PluginArtifactTarget {
  /** Loadable plugin entry library. */ readonly library: PluginArtifactFile;
  /** Ordinary shared libraries; these do not export plugin entry points. */ readonly dependencies: readonly PluginArtifactFile[];
  /** Development-only link inputs, such as MinGW import libraries. */ readonly linkLibraries: readonly PluginArtifactFile[];
  /** ABI requirements that must match the host runtime. */ readonly runtime: PluginRuntimeRequirements;
}
/** Common artifact format shared by local builds and NPM packages. */
export interface PluginArtifactManifest {
  /** Manifest schema revision. */ readonly schemaVersion: 1;
  /** Public muon C ABI revision. */ readonly abiVersion: number;
  /** Runtime registration name; independent of the NPM package name. */ readonly name: string;
  /** Complete producer catalog of public function paths. */ readonly functions: readonly string[];
  /** Public include files relative to the manifest. */ readonly headers: readonly string[];
  /** Public TypeScript declarations relative to the manifest. */ readonly types: readonly string[];
  /** License notices relative to the manifest. */ readonly licenses: readonly string[];
  /** Binaries selected by application target, never by the build host. */ readonly targets: Partial<
    Record<MuonPluginTarget, PluginArtifactTarget>
  >;
}
/** Verified absolute paths for one artifact target. */
export interface ResolvedPluginArtifactTarget {
  /** Plugin shared library. */ readonly library: string;
  /** Ordinary runtime dependencies. */ readonly dependencies: readonly string[];
  /** Loader metadata for the plugin. */ readonly binary: PluginBinaryInfo;
}
/** Validated plugin package. */
export interface ResolvedPluginArtifact {
  /** Absolute manifest location. */ readonly path: string;
  /** Producer metadata. */ readonly manifest: PluginArtifactManifest;
  /** Only the requested targets. */ readonly targets: Partial<
    Record<MuonPluginTarget, ResolvedPluginArtifactTarget>
  >;
}

const digest = (data: Buffer): string =>
  createHash('sha256').update(data).digest('hex');
const record = (value: unknown): Record<string, unknown> => {
  if (!value || typeof value !== 'object' || Array.isArray(value))
    throw new Error('Plugin manifest must contain objects.');
  return value as Record<string, unknown>;
};
const paths = (value: unknown): string[] => {
  if (
    !Array.isArray(value) ||
    !value.every((item) => typeof item === 'string' && item.length)
  )
    throw new Error('Plugin manifest must contain path arrays.');
  return value;
};
const packagePath = async (
  directory: string,
  path: string
): Promise<string> => {
  if (
    !path ||
    isAbsolute(path) ||
    path.includes('\\') ||
    path.includes('\0') ||
    path.split('/').some((part) => part === '..' || part === '')
  )
    throw new Error(`Invalid plugin artifact path: ${path}`);
  const root = await realpath(directory);
  const absolute = await realpath(resolve(root, path));
  const local = relative(root, absolute);
  if (local === '..' || local.startsWith('..' + sep) || isAbsolute(local))
    throw new Error(`Plugin artifact escapes package: ${path}`);
  return absolute;
};

/**
 * Reads and validates prebuilt artifacts without requiring authoring tools.
 * @param path - Manifest file path.
 * @param targets - Required application targets.
 * @returns Verified binaries and the producer catalog.
 */
export const readPluginArtifact = async (
  path: string,
  targets: readonly MuonPluginTarget[]
): Promise<ResolvedPluginArtifact> => {
  path = resolve(path);
  const directory = dirname(path);
  const raw = record(JSON.parse(await readFile(path, 'utf8')));
  if (raw.schemaVersion !== 1 || raw.abiVersion !== muonPluginAbiVersion)
    throw new Error(`Unsupported plugin schema or ABI: ${path}`);
  if (
    typeof raw.name !== 'string' ||
    !/^[A-Za-z_][A-Za-z0-9_]*$/.test(raw.name) ||
    raw.name === 'internal'
  )
    throw new Error('Invalid plugin registration name.');
  const functions = paths(raw.functions);
  if (
    !functions.length ||
    new Set(functions).size !== functions.length ||
    functions.some(
      (item) => !/^[A-Za-z_$][\w$]*(?:\.[A-Za-z_$][\w$]*){2,}$/.test(item)
    )
  )
    throw new Error('Invalid or duplicate plugin function catalog.');
  for (const group of ['headers', 'types', 'licenses'])
    for (const item of paths(raw[group])) await packagePath(directory, item);
  const entries = record(raw.targets);
  for (const target of Object.keys(entries))
    getPluginTarget(target as MuonPluginTarget);
  const resolved: ResolvedPluginArtifact['targets'] = {};
  for (const target of targets) {
    getPluginTarget(target);
    if (!entries[target])
      throw new Error(`Plugin ${raw.name} does not support ${target}.`);
    const entry = record(entries[target]);
    const runtime = record(entry.runtime);
    const descriptor = getPluginTarget(target);
    if (
      typeof runtime.cardio !== 'boolean' ||
      (runtime.cardio &&
        runtime.cardioAbi !==
          `muon-${muonPluginAbiVersion}-${muonPluginCardioHeaderSha256}-${target}`)
    )
      throw new Error('Plugin cardio ABI does not match this runtime.');
    if (
      runtime.cpp !== undefined &&
      runtime.cpp !== (descriptor.os === 'android' ? 'c++_shared' : 'libstdc++')
    )
      throw new Error('Plugin C++ runtime does not match this target.');
    if (
      descriptor.os === 'android' &&
      (runtime.ndk !== muonPluginNdkVersion ||
        runtime.androidApi !== muonPluginAndroidApi)
    )
      throw new Error('Plugin NDK or Android API does not match this runtime.');
    const read = async (
      value: unknown,
      plugin: boolean
    ): Promise<{ path: string; info: PluginBinaryInfo }> => {
      const file = record(value);
      if (
        typeof file.path !== 'string' ||
        typeof file.sha256 !== 'string' ||
        !/^[a-f0-9]{64}$/.test(file.sha256)
      )
        throw new Error('Invalid plugin file or SHA-256.');
      const absolute = await packagePath(directory, file.path);
      const data = await readFile(absolute);
      if (digest(data) !== file.sha256)
        throw new Error(`Plugin SHA-256 mismatch: ${file.path}`);
      const info = inspectPluginBinary(data, target);
      if (plugin && !info.exports.includes('muon_init_plugin'))
        throw new Error(`${file.path} must export muon_init_plugin.`);
      if (descriptor.os !== 'windows' && info.soname !== basename(absolute))
        throw new Error(
          `${file.path} ELF SONAME must match its packaged name.`
        );
      return { path: absolute, info };
    };
    const library = await read(entry.library, true);
    if (
      !Array.isArray(entry.dependencies) ||
      !Array.isArray(entry.linkLibraries)
    )
      throw new Error('Plugin dependencies and linkLibraries must be arrays.');
    const dependencies: string[] = [];
    const inspections: PluginLibraryInspection[] = [
      { path: library.path, info: library.info },
    ];
    for (const dependency of entry.dependencies) {
      const inspected = await read(dependency, false);
      dependencies.push(inspected.path);
      inspections.push(inspected);
    }
    validatePluginLibraryDependencies(inspections, target);
    for (const value of entry.linkLibraries) {
      const file = record(value);
      if (
        typeof file.path !== 'string' ||
        digest(await readFile(await packagePath(directory, file.path))) !==
          file.sha256
      )
        throw new Error('Plugin link input SHA-256 mismatch.');
    }
    resolved[target] = {
      library: library.path,
      dependencies,
      binary: library.info,
    };
  }
  return {
    path,
    manifest: raw as unknown as PluginArtifactManifest,
    targets: resolved,
  };
};

/** Inputs for one target produced by any build system. */
export interface PackagePluginTargetInput {
  /** Final library path. */ readonly library: string;
  /** Final ordinary runtime dependency paths. */ readonly dependencies?: readonly string[];
  /** Import/static libraries for native consumers. */ readonly linkLibraries?: readonly string[];
  /** Implementation language, defaults to C++20. */ readonly language?:
    'c' | 'c++';
  /** Shared cardio use, defaults to true for C++. */ readonly cardio?: boolean;
}
/** Producer inputs for the common artifact format. */
export interface PackagePluginOptions {
  /** Package output directory, independent of source directories. */ readonly outputDirectory: string;
  /** Plugin registration name. */ readonly name: string;
  /** Complete public function catalog. */ readonly functions: readonly string[];
  /** Header files to publish. */ readonly headers?: readonly string[];
  /** Type declarations to publish. */ readonly types?: readonly string[];
  /** License files to publish. */ readonly licenses?: readonly string[];
  /** Build outputs by explicit target. */ readonly targets: Partial<
    Record<MuonPluginTarget, PackagePluginTargetInput>
  >;
}

/**
 * Copies final build outputs and writes their verified artifact manifest.
 * @param options - Producer metadata and final native files.
 * @returns Absolute muon-plugin.json path suitable for local use or NPM export.
 * @remarks All strip and signing operations must precede this operation.
 */
export const packagePlugin = async (
  options: PackagePluginOptions
): Promise<string> => {
  const output = resolve(options.outputDirectory);
  await mkdir(output, { recursive: true });
  const copy = async (
    path: string,
    directory: string
  ): Promise<PluginArtifactFile> => {
    const destination = join(directory, basename(path));
    const absolute = join(output, destination);
    await mkdir(dirname(absolute), { recursive: true });
    if (resolve(path) !== absolute) await copyFile(path, absolute);
    return {
      path: destination.split(sep).join('/'),
      sha256: digest(await readFile(absolute)),
    };
  };
  const resources = async (
    inputs: readonly string[],
    directory: string
  ): Promise<string[]> => {
    if (new Set(inputs.map((path) => basename(path))).size !== inputs.length)
      throw new Error(`Duplicate plugin ${directory} filename.`);
    const result: string[] = [];
    for (const path of inputs) result.push((await copy(path, directory)).path);
    return result;
  };
  const targets: PluginArtifactManifest['targets'] = {};
  for (const [key, input] of Object.entries(options.targets)) {
    const target = key as MuonPluginTarget;
    const descriptor = getPluginTarget(target);
    const native = `native/${target}`;
    const dependencies: PluginArtifactFile[] = [];
    const linkLibraries: PluginArtifactFile[] = [];
    if (
      new Set(
        [input.library, ...(input.dependencies ?? [])].map((path) =>
          basename(path).toLowerCase()
        )
      ).size !==
      1 + (input.dependencies?.length ?? 0)
    )
      throw new Error('Duplicate plugin native filename.');
    for (const path of input.dependencies ?? [])
      dependencies.push(await copy(path, native));
    for (const path of input.linkLibraries ?? [])
      linkLibraries.push(await copy(path, `${native}/link`));
    const cardio = input.cardio ?? input.language !== 'c';
    targets[target] = {
      library: await copy(input.library, native),
      dependencies,
      linkLibraries,
      runtime: {
        cardio,
        ...(cardio
          ? {
              cardioAbi: `muon-${muonPluginAbiVersion}-${muonPluginCardioHeaderSha256}-${target}`,
            }
          : {}),
        ...(input.language === 'c'
          ? {}
          : { cpp: descriptor.os === 'android' ? 'c++_shared' : 'libstdc++' }),
        ...(descriptor.os === 'android'
          ? { ndk: muonPluginNdkVersion, androidApi: muonPluginAndroidApi }
          : {}),
      },
    };
  }
  const manifest: PluginArtifactManifest = {
    schemaVersion: 1,
    abiVersion: muonPluginAbiVersion,
    name: options.name,
    functions: options.functions,
    headers: await resources(options.headers ?? [], 'include'),
    types: await resources(options.types ?? [], 'types'),
    licenses: await resources(options.licenses ?? [], 'licenses'),
    targets,
  };
  const path = join(output, 'muon-plugin.json');
  await writeFile(path, JSON.stringify(manifest, null, 2) + '\n');
  await readPluginArtifact(path, Object.keys(targets) as MuonPluginTarget[]);
  return path;
};

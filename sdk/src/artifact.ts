import { createHash } from 'node:crypto';
import {
  copyFile,
  cp,
  mkdir,
  mkdtemp,
  readFile,
  realpath,
  rm,
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
  validateServiceLibraryDependencies,
  type PluginLibraryInspection,
} from './dependencies.js';
export { inspectPluginBinary } from './binary.js';
export { muonPluginAndroidApi } from './targets.js';
export {
  validatePluginLibraryDependencies,
  validateServiceLibraryDependencies,
} from './dependencies.js';
import {
  getPluginTarget,
  muonPluginAbiVersion,
  muonServiceAbiVersion,
  muonPluginNdkVersion,
  muonPluginAndroidApi,
  muonPluginCardioHeaderSha256,
  type MuonPluginTarget,
} from './targets.js';

/** One packaged native file, authenticated after all producer processing. */
export interface PluginArtifactFile {
  /** Path relative to the manifest directory. */ readonly path: string;
  /** SHA-256 digest of the final bytes. */ readonly sha256: string;
  /** Explicit Android dependency whose ELF has no DT_SONAME and loads by this file's basename. */ readonly androidSoname?: 'absent';
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
  /** Artifact role; omission means a page plugin. */ readonly kind?: 'plugin';
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
/** Native service package containing one factory for independent instances. */
export interface ServiceArtifactManifest extends Omit<
  PluginArtifactManifest,
  'kind' | 'functions'
> {
  /** Factory artifact name, independent of configured logical service IDs. */ readonly name: string;
  /** Required discriminator for service-only artifacts. */ readonly kind: 'service';
  /** Services do not publish a page plugin function catalog. */ readonly functions?: never;
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
/** Validated service factory package, independent of page plugin registration. */
export interface ResolvedServiceArtifact extends Omit<
  ResolvedPluginArtifact,
  'manifest'
> {
  /** Producer metadata for the service factory. */ readonly manifest: ServiceArtifactManifest;
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
): Promise<ResolvedPluginArtifact> =>
  await readArtifact<PluginArtifactManifest>(path, targets, 'plugin');

/**
 * Validates a native service artifact without requiring authoring tools.
 * @param path - Manifest file path.
 * @param targets - Required application targets.
 * @returns Verified service factory binaries and producer metadata.
 * @remarks Requires kind service and the muon_get_service_factory export.
 */
export const readServiceArtifact = async (
  path: string,
  targets: readonly MuonPluginTarget[]
): Promise<ResolvedServiceArtifact> =>
  await readArtifact<ServiceArtifactManifest>(path, targets, 'service');

const readArtifact = async <
  Manifest extends PluginArtifactManifest | ServiceArtifactManifest,
>(
  path: string,
  targets: readonly MuonPluginTarget[],
  kind: 'plugin' | 'service'
): Promise<
  Omit<ResolvedPluginArtifact, 'manifest'> & { manifest: Manifest }
> => {
  path = resolve(path);
  const directory = dirname(path);
  const raw = record(JSON.parse(await readFile(path, 'utf8')));
  if ((raw.kind === undefined ? 'plugin' : raw.kind) !== kind)
    throw new Error(`Expected ${kind} artifact kind: ${path}`);
  if (
    raw.schemaVersion !== 1 ||
    raw.abiVersion !==
      (kind === 'service' ? muonServiceAbiVersion : muonPluginAbiVersion)
  )
    throw new Error(`Unsupported plugin schema or ABI: ${path}`);
  if (
    typeof raw.name !== 'string' ||
    !/^[A-Za-z_][A-Za-z0-9_]*$/.test(raw.name) ||
    raw.name === 'internal'
  )
    throw new Error('Invalid plugin registration name.');
  if (kind === 'service') {
    if ('functions' in raw)
      throw new Error('Service artifacts must not contain functions.');
  } else {
    const functions = paths(raw.functions);
    if (
      !functions.length ||
      new Set(functions).size !== functions.length ||
      functions.some(
        (item) => !/^[A-Za-z_$][\w$]*(?:\.[A-Za-z_$][\w$]*){2,}$/.test(item)
      )
    )
      throw new Error('Invalid or duplicate plugin function catalog.');
  }
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
        typeof runtime.androidApi !== 'number' ||
        !Number.isSafeInteger(runtime.androidApi) ||
        runtime.androidApi < muonPluginAndroidApi)
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
      const entryPoint =
        kind === 'service' ? 'muon_get_service_factory' : 'muon_init_plugin';
      if (plugin && !info.exports.includes(entryPoint))
        throw new Error(`${file.path} must export ${entryPoint}.`);
      if (file.androidSoname !== undefined) {
        if (
          file.androidSoname !== 'absent' ||
          descriptor.os !== 'android' ||
          plugin ||
          info.soname !== undefined ||
          !/^lib[A-Za-z0-9_.+-]+\.so$/.test(basename(absolute))
        )
          throw new Error(
            `${file.path} has an invalid Android SONAME declaration.`
          );
      } else if (
        descriptor.os !== 'windows' &&
        info.soname !== basename(absolute)
      )
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
    if (
      new Set(inspections.map((item) => basename(item.path).toLowerCase()))
        .size !== inspections.length
    )
      throw new Error('Duplicate plugin native filename.');
    (kind === 'service'
      ? validateServiceLibraryDependencies
      : validatePluginLibraryDependencies)(inspections, target);
    for (const value of entry.linkLibraries) {
      const file = record(value);
      if (file.androidSoname !== undefined)
        throw new Error('A link input cannot declare an Android SONAME.');
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
    manifest: raw as unknown as Manifest,
    targets: resolved,
  };
};

/** Inputs for one target produced by any build system. */
export interface PackagePluginTargetInput {
  /** Minimum API used to compile this Android target; defaults to the shared runtime floor. */ readonly androidApi?: number;
  /** Final library path. */ readonly library: string;
  /** Final ordinary runtime dependency paths. */ readonly dependencies?: readonly string[];
  /** Packaged basenames of externally built Android dependencies lacking DT_SONAME. */ readonly androidDependenciesWithoutSoname?: readonly string[];
  /** Import/static libraries for native consumers. */ readonly linkLibraries?: readonly string[];
  /** Implementation language, defaults to C++20. */ readonly language?:
    'c' | 'c++';
  /** Shared cardio use, defaults to true for C++. */ readonly cardio?: boolean;
}
/** Producer inputs for the common artifact format. */
export interface PackagePluginOptions {
  /** Artifact role; omission means a page plugin. */ readonly kind?: 'plugin';
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
/** Final native service inputs; each target library exports one factory. */
export interface PackageServiceOptions extends Omit<
  PackagePluginOptions,
  'kind' | 'functions'
> {
  /** Factory artifact name, independent of configured logical service IDs. */ readonly name: string;
  /** Required discriminator for the service factory package. */ readonly kind: 'service';
  /** A service factory has no page plugin function catalog. */ readonly functions?: never;
}

/**
 * Copies final build outputs and writes their verified artifact manifest.
 * @param options - Producer metadata and final native files.
 * @returns Absolute muon-plugin.json path suitable for local use or NPM export.
 * @remarks All strip and signing operations must precede this operation.
 */
export const packagePlugin = async (
  options: PackagePluginOptions
): Promise<string> => await packageArtifact(options, 'plugin');

/**
 * Packages final service binaries and validates their factory ABI and dependencies.
 * @param options - Service metadata and final native files.
 * @returns Absolute muon-plugin.json path with kind service.
 * @remarks Finish stripping and signing before packaging; hashes cover final bytes.
 */
export const packageService = async (
  options: PackageServiceOptions
): Promise<string> => await packageArtifact(options, 'service');

const packageArtifact = async (
  options: PackagePluginOptions | PackageServiceOptions,
  kind: 'plugin' | 'service'
): Promise<string> => {
  if ((options.kind === undefined ? 'plugin' : options.kind) !== kind)
    throw new Error(`Expected ${kind} artifact kind.`);
  if (kind === 'service' && 'functions' in options)
    throw new Error('Service artifacts must not contain functions.');
  const destinationRoot = resolve(options.outputDirectory);
  await mkdir(dirname(destinationRoot), { recursive: true });
  const output = await mkdtemp(join(dirname(destinationRoot), '.muon-plugin-'));
  // Validate the entire result before replacing a previously usable package.
  // Failed validation must allow authors to fix their configuration and retry.
  try {
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
      for (const path of inputs)
        result.push((await copy(path, directory)).path);
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
      const names = input.androidDependenciesWithoutSoname ?? [];
      const absent = new Set(names);
      const dependencyNames = (input.dependencies ?? []).map((path) =>
        basename(path)
      );
      if (
        !Array.isArray(names) ||
        absent.size !== names.length ||
        names.some(
          (name) =>
            descriptor.os !== 'android' || !dependencyNames.includes(name)
        )
      )
        throw new Error(
          'Android dependencies without a SONAME must name distinct packaged dependency files.'
        );
      for (const path of input.dependencies ?? []) {
        const file = await copy(path, native);
        dependencies.push(
          absent.has(basename(path))
            ? { ...file, androidSoname: 'absent' }
            : file
        );
      }
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
            : {
                cpp: descriptor.os === 'android' ? 'c++_shared' : 'libstdc++',
              }),
          ...(descriptor.os === 'android'
            ? {
                ndk: muonPluginNdkVersion,
                androidApi:
                  input.androidApi === undefined
                    ? muonPluginAndroidApi
                    : input.androidApi,
              }
            : {}),
        },
      };
    }
    const manifest: PluginArtifactManifest | ServiceArtifactManifest = {
      schemaVersion: 1,
      abiVersion:
        kind === 'service' ? muonServiceAbiVersion : muonPluginAbiVersion,
      name: options.name,
      ...(options.kind === 'service'
        ? { kind: 'service' as const }
        : {
            ...(options.kind ? { kind: options.kind } : {}),
            functions: options.functions,
          }),
      headers: await resources(options.headers ?? [], 'include'),
      types: await resources(options.types ?? [], 'types'),
      licenses: await resources(options.licenses ?? [], 'licenses'),
      targets,
    };
    const path = join(output, 'muon-plugin.json');
    await writeFile(path, JSON.stringify(manifest, null, 2) + '\n');
    await readArtifact(path, Object.keys(targets) as MuonPluginTarget[], kind);
    await cp(output, destinationRoot, { recursive: true });
    return join(destinationRoot, 'muon-plugin.json');
  } finally {
    await rm(output, { recursive: true, force: true });
  }
};

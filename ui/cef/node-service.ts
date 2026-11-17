// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import {
  cp,
  mkdir,
  readFile,
  realpath,
  stat,
  writeFile,
} from "node:fs/promises";
import { createRequire, isBuiltin } from "node:module";
import {
  basename,
  dirname,
  isAbsolute,
  join,
  relative,
  resolve,
} from "node:path";
import {
  assertMuonNodeProjectStagingIsSafe,
  type ResolvedMuonNodeProject,
} from "./node-project.ts";
import {
  createMuonServiceInputPlugin,
  type MuonServiceInputObserver,
} from "../common/service-inputs.ts";

/** Compiled project modules and installed runtime dependencies of a service. */
export interface MuonNodeServiceBuild {
  /** Project-relative compiled entry, always an ES module. */
  readonly entry: string;
  /** Project-relative output files preserving module-relative asset paths. */
  readonly files: ReadonlyMap<string, string | Uint8Array>;
  /** External npm imports to copy when installed, including dev dependencies. */
  readonly packages: readonly string[];
  /** Static npm imports that must exist even when declared as optional dependencies. */
  readonly requiredPackages: readonly string[];
}

const isInside = (root: string, path: string): boolean => {
  const part = relative(root, path);
  return (
    part !== ".." &&
    !part.startsWith("../") &&
    !part.startsWith("..\\") &&
    !isAbsolute(part)
  );
};

const packageName = (id: string): string =>
  id
    .split("/")
    .slice(0, id.startsWith("@") ? 2 : 1)
    .join("/");

/**
 * Compiles a Node service with Vite while retaining Node's npm and asset semantics.
 * @param project - Validated service project and engine requirement.
 * @param entry - Source entry resolved relative to that project.
 * @param onInput - Optional development observer for module and project inputs.
 * @returns Portable ES modules and external package names.
 */
export const buildMuonNodeService = async (
  project: ResolvedMuonNodeProject,
  entry: string,
  onInput?: MuonServiceInputObserver,
): Promise<MuonNodeServiceBuild> => {
  const source = await realpath(resolve(project.sourcePath, entry));
  if (!isInside(project.sourcePath, source))
    throw new Error("Node service entry must be inside its project.");
  const vite = await import("vite");
  if (Number(vite.version.split(".")[0]) < 8)
    throw new Error("Node services require Vite 8 or later.");
  const packages = new Set<string>();
  const result = await vite.build({
    configFile: false,
    root: project.sourcePath,
    publicDir: false,
    logLevel: "warn",
    plugins: [createMuonServiceInputPlugin(source, onInput)],
    build: {
      write: false,
      ssr: source,
      target: "node20",
      minify: false,
      modulePreload: false,
      emitAssets: true,
      rolldownOptions: {
        platform: "node",
        external: (id) => {
          if (isBuiltin(id)) return true;
          if (
            !isAbsolute(id) &&
            !id.startsWith(".") &&
            !id.startsWith("\0") &&
            !id.startsWith("#")
          ) {
            packages.add(packageName(id));
            return true;
          }
          return false;
        },
        output: {
          format: "es",
          preserveModules: true,
          preserveModulesRoot: project.sourcePath,
          entryFileNames: "[name].mjs",
          chunkFileNames: "[name].mjs",
          assetFileNames: "[name][extname]",
        },
        onwarn: (warning) => {
          throw new Error(`Node service build: ${warning.message}`);
        },
      },
    },
  });
  const outputs = Array.isArray(result) ? result : [result];
  if (outputs.length !== 1 || !("output" in outputs[0]!))
    throw new Error("Node service build did not produce JavaScript.");
  let compiledEntry: string | undefined;
  const files = new Map<string, string | Uint8Array>();
  const requiredPackages = new Set<string>();
  const emitted = new Set(outputs[0]!.output.map((file) => file.fileName));
  for (const file of outputs[0]!.output) {
    if (
      !isInside(project.sourcePath, resolve(project.sourcePath, file.fileName))
    )
      throw new Error("Node service output must remain inside its project.");
    if (file.type === "chunk") {
      // npm packages stay external. Local modules need their original directory
      // to keep import.meta.url and adjacent runtime assets meaningful.
      for (const module of file.moduleIds)
        if (isAbsolute(module) && !isInside(project.sourcePath, module))
          throw new Error(
            `Node service source must be inside its project: ${module}`,
          );
      if (file.facadeModuleId === source) compiledEntry = "./" + file.fileName;
      for (const imported of file.imports)
        if (!emitted.has(imported) && packages.has(packageName(imported)))
          requiredPackages.add(packageName(imported));
      files.set(file.fileName, file.code);
    } else files.set(file.fileName, file.source);
  }
  if (compiledEntry === undefined)
    throw new Error("Node service build did not produce its entry module.");
  return {
    entry: compiledEntry,
    files,
    packages: [...packages],
    requiredPackages: [...requiredPackages],
  };
};

const copyProjectFiles = async (
  source: string,
  destination: string,
): Promise<void> => {
  await cp(source, destination, {
    recursive: true,
    dereference: true,
    preserveTimestamps: true,
    filter: (path) =>
      path === source ||
      !["node_modules", ".git", ".muon"].includes(basename(path)),
  });
};

const findInstalledPackage = async (
  name: string,
  from: string,
  onInput: MuonServiceInputObserver | undefined,
): Promise<string | undefined> => {
  if (
    !/^(?:@[A-Za-z0-9_.-]+\/)?[A-Za-z0-9_.-]+$/u.test(name) ||
    name.split("/").some((part) => part === "." || part === "..")
  )
    throw new Error(`Invalid Node service dependency name: ${name}`);
  for (const path of createRequire(join(from, "package.json")).resolve.paths(
    name,
  ) ?? []) {
    const candidate = join(path, name);
    onInput?.({ path: candidate, kind: "tree" });
    try {
      if ((await stat(join(candidate, "package.json"))).isFile())
        return await realpath(candidate);
    } catch (error) {
      if (
        (error as NodeJS.ErrnoException).code !== "ENOENT" &&
        (error as NodeJS.ErrnoException).code !== "ENOTDIR"
      )
        throw error;
    }
  }
  return undefined;
};

/**
 * Copies a Node service project, installed production dependencies and compiled modules.
 * @param project - Validated project containing runtime assets.
 * @param build - Compiled modules and their external npm imports.
 * @param destination - Fresh service directory in a development snapshot or distribution.
 * @param onInput - Optional development observer for installed runtime packages.
 * @returns Completion after every runtime input is independent of the source tree.
 * @remarks Installed dependency versions are preserved. This never runs npm install
 * or package lifecycle scripts; missing required dependencies fail preparation.
 */
export const stageMuonNodeService = async (
  project: ResolvedMuonNodeProject,
  build: MuonNodeServiceBuild,
  destination: string,
  onInput?: MuonServiceInputObserver,
): Promise<void> => {
  await assertMuonNodeProjectStagingIsSafe(project, destination);
  await copyProjectFiles(project.sourcePath, destination);
  const copied = new Map<string, string>();
  const pending = [
    {
      source: project.sourcePath,
      output: destination,
      extra: build.packages,
      required: build.requiredPackages,
    },
  ];
  // Reserve all direct dependencies before traversing their children, so a
  // transitive version cannot displace the application's own selected version.
  for (let index = 0; index < pending.length; ++index) {
    const { source, output, extra, required } = pending[index]!;
    const manifest = JSON.parse(
      await readFile(join(source, "package.json"), "utf8"),
    ) as {
      dependencies?: Record<string, string>;
      optionalDependencies?: Record<string, string>;
      peerDependencies?: Record<string, string>;
      peerDependenciesMeta?: Record<string, { optional?: boolean }>;
    };
    const names = new Set([
      ...Object.keys(manifest.dependencies ?? {}),
      ...Object.keys(manifest.optionalDependencies ?? {}),
      ...Object.keys(manifest.peerDependencies ?? {}),
      ...extra,
    ]);
    for (const name of names) {
      const installed = await findInstalledPackage(name, source, onInput);
      if (installed === undefined) {
        if (
          !required.includes(name) &&
          (Object.hasOwn(manifest.optionalDependencies ?? {}, name) ||
            manifest.peerDependenciesMeta?.[name]?.optional)
        )
          continue;
        throw new Error(
          `Node service dependency ${name} is not installed for ${source}. Run npm install in the project.`,
        );
      }
      onInput?.({ path: installed, kind: "tree" });
      // Resolve against the copied ancestry before adding a nested version. This
      // retains npm's version selection and terminates circular dependency graphs.
      let available = false;
      for (const path of createRequire(
        join(output, "package.json"),
      ).resolve.paths(name) ?? []) {
        const existing = copied.get(join(path, name));
        if (existing !== undefined) {
          available = existing === installed;
          break;
        }
      }
      if (available) continue;
      const rootPath = join(destination, "node_modules", name);
      const rootVersion = copied.get(rootPath);
      const path =
        isInside(join(source, "node_modules"), installed) ||
        (rootVersion !== undefined && rootVersion !== installed)
          ? join(output, "node_modules", name)
          : rootPath;
      copied.set(path, installed);
      await copyProjectFiles(installed, path);
      pending.push({
        source: installed,
        output: path,
        extra: [],
        required: [],
      });
    }
  }
  for (const [path, data] of build.files) {
    const target = join(destination, path);
    await mkdir(dirname(target), { recursive: true });
    await writeFile(target, data);
  }
};

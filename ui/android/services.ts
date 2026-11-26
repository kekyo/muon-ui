// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { readFile } from "node:fs/promises";
import { basename } from "node:path";
import {
  inspectPluginBinary,
  muonPluginAndroidApi,
} from "muon-plugin-sdk/artifact";
import type {
  MuonAndroidServiceConfig,
  MuonResolvedService,
} from "../common/service-config.ts";
import type { MuonAndroidAbi, MuonAndroidPackagedPlugin } from "./build.ts";
import { assertAndroidArtifactApi, resolveAndroidPlugins } from "./plugins.ts";

/** Common Android settings for one independently declared service component. */
interface MuonAndroidPackagedServiceBase {
  /** Portable service I/O grants. */ readonly io?: import("../common/service-io.ts").MuonServiceIoPolicy;
  /** Application-local service identity. */ readonly id: string;
  /** Initial resident setting. */ readonly autostart: boolean;
  /** Configuration passed to this instance. */ readonly config: Readonly<
    Record<string, string>
  >;
  /** Independent component and purpose for this service. */ readonly android: MuonAndroidServiceConfig & {
    /** Fully qualified generated Java Service class. */ readonly component: string;
  };
}

/** Verified native factory libraries packaged for a logical service. */
export interface MuonAndroidPackagedNativeService extends MuonAndroidPackagedServiceBase {
  /** Native service execution. */ readonly runtime: "native";
  /** Shared factory library name in the APK. */ readonly soname: string;
  /** Absolute library paths selected for the APK. */ readonly libraries: Readonly<
    Partial<Record<MuonAndroidAbi, string>>
  >;
  /** Additional ordinary libraries selected for the APK. */ readonly dependencies: Readonly<
    Partial<Record<MuonAndroidAbi, readonly string[]>>
  >;
}

/** Bundled JavaScript executed in an independent QuickJS heap. */
export interface MuonAndroidPackagedQuickJsService extends MuonAndroidPackagedServiceBase {
  /** QuickJS service execution. */ readonly runtime: "quickjs";
  /** Validated bundled JavaScript source. */ readonly bundle: string;
  /** APK asset path, relative to the assets root. */ readonly asset: string;
  /** Explicit native modules and this service's independent grants. */ readonly plugins: readonly MuonAndroidPackagedPlugin[];
}

/** Verified runtime inputs and Android settings for one logical service. */
export type MuonAndroidPackagedService =
  MuonAndroidPackagedNativeService | MuonAndroidPackagedQuickJsService;

const reserved = new Set([
  "libcardio.so",
  "libc++_shared.so",
  "libmuon_android_rpc.so",
  "libmuon_android_services.so",
]);

/** Resolve manifest network access and exact cleartext hosts from the service I/O grants.
 * @param services Packaged service policies.
 * @returns Whether INTERNET is needed and which HTTP hosts may use cleartext.
 */
export const getAndroidServiceNetwork = (
  services: readonly Pick<MuonAndroidPackagedServiceBase, "io">[],
) => {
  const origins = services.flatMap(
    (service) => service.io?.http?.origins ?? [],
  );
  const listens = services.some((service) => service.io?.server?.listen);
  const hosts = origins
    .filter((origin) => origin.startsWith("http://"))
    .map((origin) => new URL(origin).hostname);
  if (listens) hosts.push("127.0.0.1");
  return {
    internet: listens || origins.length > 0,
    cleartextHosts: [...new Set(hosts)],
  };
};

/**
 * Resolves verified service artifacts into independently declared Android components.
 * @param definitions - Common service declarations already validated by the SDK.
 * @param abis - ABIs included in the APK.
 * @param plugins - Page plugins sharing Android's native library namespace.
 * @param minSdk - Application installation floor, defaulting to the runtime minimum.
 * @returns Package inputs after rejecting ambiguous shared library filenames.
 */
export const resolveAndroidServices = async (
  definitions: readonly MuonResolvedService[],
  abis: readonly MuonAndroidAbi[],
  plugins: readonly MuonAndroidPackagedPlugin[],
  minSdk = muonPluginAndroidApi,
): Promise<readonly MuonAndroidPackagedService[]> => {
  const services: MuonAndroidPackagedService[] = [];
  for (const definition of definitions) {
    if (definition.runtime === "node")
      throw new Error(
        `Service ${definition.id}: node services are not supported on Android.`,
      );
    if (definition.android === undefined)
      throw new Error(
        `Service ${definition.id} requires Android foreground settings.`,
      );
    const common = {
      id: definition.id,
      autostart: definition.autostart,
      config: definition.config,
      ...(definition.io ? { io: definition.io } : {}),
      android: {
        ...definition.android,
        component: `dev.muon.generated.MuonService_${Buffer.from(definition.id, "utf8").toString("hex")}`,
      },
    };
    if (definition.runtime === "quickjs") {
      const native = await resolveAndroidPlugins(
        definition.plugins.map((plugin) => {
          assertAndroidArtifactApi(plugin.artifact.manifest, abis, minSdk);
          const libraries: Partial<Record<MuonAndroidAbi, string>> = {};
          const dependencies: Partial<
            Record<MuonAndroidAbi, readonly string[]>
          > = {};
          for (const abi of abis) {
            const target = plugin.artifact.targets[`android-${abi}`];
            if (!target)
              throw new Error(
                `Service plugin ${plugin.name} is missing its ${abi} library.`,
              );
            libraries[abi] = target.library;
            dependencies[abi] = target.dependencies;
          }
          return {
            name: plugin.name,
            soname: basename(libraries[abis[0]!]!),
            libraries,
            dependencies,
            allow: plugin.allow,
            expectedFunctions: plugin.allow,
            config: plugin.config,
          };
        }),
        abis,
        ".",
      );
      services.push({
        ...common,
        runtime: "quickjs",
        bundle: definition.bundle,
        asset: `muon/services/${definition.id}.js`,
        plugins: native,
      });
      continue;
    }
    assertAndroidArtifactApi(definition.artifact.manifest, abis, minSdk);
    const libraries: Partial<Record<MuonAndroidAbi, string>> = {};
    const dependencies: Partial<Record<MuonAndroidAbi, readonly string[]>> = {};
    const names = new Set<string>();
    for (const abi of abis) {
      const target = definition.artifact.targets[`android-${abi}`];
      if (!target)
        throw new Error(
          `Service ${definition.id} is missing its ${abi} library.`,
        );
      const name = basename(target.library);
      const info = inspectPluginBinary(
        await readFile(target.library),
        `android-${abi}`,
      );
      if (info.soname !== name)
        throw new Error(`${name} ELF SONAME must match its packaged name.`);
      names.add(name);
      libraries[abi] = target.library;
      dependencies[abi] = target.dependencies;
    }
    if (names.size !== 1)
      throw new Error(
        `Service ${definition.id} requires the same library filename for every ABI.`,
      );
    services.push({
      ...common,
      runtime: "native",
      soname: [...names][0]!,
      libraries,
      dependencies,
    });
  }
  for (const abi of abis) {
    const files = new Map<string, Buffer>();
    for (const entry of [
      ...plugins,
      ...getAndroidServicePlugins(services),
      ...services.filter((service) => service.runtime === "native"),
    ]) {
      for (const [index, path] of [
        entry.libraries[abi]!,
        ...(entry.dependencies?.[abi] ?? []),
      ].entries()) {
        const name = index === 0 ? entry.soname : basename(path);
        if (reserved.has(name))
          throw new Error(`Android library name is reserved: ${name}`);
        const bytes = await readFile(path);
        const previous = files.get(name);
        if (previous && !previous.equals(bytes))
          throw new Error(`Conflicting Android library: ${name}`);
        files.set(name, bytes);
      }
    }
  }
  return services;
};

/**
 * Coalesces verified native modules for the private Android service process.
 * @param services - Packaged service inputs with owner-specific capabilities.
 * @returns One loader entry per plugin, with the union of its function grants.
 * @remarks Artifact and configuration equality were checked by the common
 * service resolver; page plugins retain their separate registry and lifetime.
 */
export const getAndroidServicePlugins = (
  services: readonly MuonAndroidPackagedService[],
): readonly MuonAndroidPackagedPlugin[] => {
  const entries = new Map<string, MuonAndroidPackagedPlugin>();
  for (const service of services) {
    if (service.runtime !== "quickjs") continue;
    for (const plugin of service.plugins) {
      const previous = entries.get(plugin.name);
      const allow = [...new Set([...(previous?.allow ?? []), ...plugin.allow])];
      entries.set(plugin.name, { ...plugin, allow, expectedFunctions: allow });
    }
  }
  return [...entries.values()];
};

/**
 * Returns manifest permissions for foreground service types, notifications and startup after boot.
 * @param services - Independently configured Android services.
 * @returns Unique permission names, or an empty list for an app with no Android services.
 * Runtime permission grants remain user-controlled.
 */
export const getAndroidServicePermissions = (
  services: readonly MuonAndroidPackagedService[],
): readonly string[] => {
  if (!services.length) return [];
  const permissions = new Set([
    "android.permission.FOREGROUND_SERVICE",
    "android.permission.POST_NOTIFICATIONS",
    "android.permission.RECEIVE_BOOT_COMPLETED",
  ]);
  for (const service of services)
    for (const type of service.android.foregroundServiceType.split("|")) {
      if (type !== "shortService")
        permissions.add(
          `android.permission.FOREGROUND_SERVICE_${type.replaceAll(/[A-Z]/gu, (letter) => `_${letter}`).toUpperCase()}`,
        );
    }
  return [...permissions];
};

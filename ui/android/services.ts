// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { readFile } from "node:fs/promises";
import { basename } from "node:path";
import { inspectPluginBinary } from "muon-plugin-sdk/artifact";
import type {
  MuonAndroidServiceConfig,
  MuonResolvedService,
} from "../common/service-config.ts";
import type { MuonAndroidAbi, MuonAndroidPackagedPlugin } from "./build.ts";

/** Verified native libraries and Android execution settings for one logical service. */
export interface MuonAndroidPackagedService {
  /** Application-local identity passed to the native factory. */ readonly id: string;
  /** Initial resident setting. */ readonly autostart: boolean;
  /** Configuration passed to this instance. */ readonly config: Readonly<
    Record<string, string>
  >;
  /** Shared factory library name in the APK. */ readonly soname: string;
  /** Absolute library paths selected for the APK. */ readonly libraries: Readonly<
    Partial<Record<MuonAndroidAbi, string>>
  >;
  /** Additional ordinary libraries selected for the APK. */ readonly dependencies: Readonly<
    Partial<Record<MuonAndroidAbi, readonly string[]>>
  >;
  /** Independent component and purpose for this service. */ readonly android: MuonAndroidServiceConfig & {
    /** Fully qualified generated Java Service class. */ readonly component: string;
  };
}

const reserved = new Set([
  "libcardio.so",
  "libc++_shared.so",
  "libmuon_android_rpc.so",
  "libmuon_android_services.so",
]);

/**
 * Resolves verified service artifacts into independently declared Android components.
 * @param definitions - Common service declarations already validated by the SDK.
 * @param abis - ABIs included in the APK.
 * @param plugins - Page plugins sharing Android's native library namespace.
 * @returns Package inputs after rejecting ambiguous shared library filenames.
 */
export const resolveAndroidServices = async (
  definitions: readonly MuonResolvedService[],
  abis: readonly MuonAndroidAbi[],
  plugins: readonly MuonAndroidPackagedPlugin[],
): Promise<readonly MuonAndroidPackagedService[]> => {
  const services: MuonAndroidPackagedService[] = [];
  for (const definition of definitions) {
    if (definition.android === undefined)
      throw new Error(
        `Service ${definition.id} requires Android foreground settings.`,
      );
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
      id: definition.id,
      autostart: definition.autostart,
      config: definition.config,
      soname: [...names][0]!,
      libraries,
      dependencies,
      android: {
        ...definition.android,
        component: `dev.muon.generated.MuonService_${Buffer.from(definition.id, "utf8").toString("hex")}`,
      },
    });
  }
  for (const abi of abis) {
    const files = new Map<string, Buffer>();
    for (const entry of [...plugins, ...services]) {
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
 * Returns manifest permissions required by the declared foreground types.
 * @param services - Independently configured Android services.
 * @returns Unique permission names; runtime permissions remain user-controlled.
 */
export const getAndroidServicePermissions = (
  services: readonly MuonAndroidPackagedService[],
): readonly string[] => {
  if (!services.length) return [];
  const permissions = new Set([
    "android.permission.FOREGROUND_SERVICE",
    "android.permission.POST_NOTIFICATIONS",
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

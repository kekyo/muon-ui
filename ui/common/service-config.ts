// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { resolve } from "node:path";
import { buildMuonQuickJsService } from "./service-build.ts";
import {
  buildMuonNodeService,
  type MuonNodeServiceBuild,
} from "../cef/node-service.ts";
import {
  resolveMuonNodeProject,
  type ResolvedMuonNodeProject,
} from "../cef/node-project.ts";
import {
  resolveMuonServicePlugins,
  getMuonServicePluginModules,
  type MuonServiceBuildOptions,
  type MuonServicePlugin,
} from "./service-plugins.ts";
import type { MuonPluginTarget } from "muon-plugin-sdk";
import {
  readServiceArtifact,
  type ResolvedServiceArtifact,
} from "muon-plugin-sdk/artifact";

/** Android foreground execution and its user-visible notification. */
export interface MuonAndroidServiceConfig {
  /** Android foreground service types separated by |, without whitespace. */
  readonly foregroundServiceType: string;
  /** Purpose required when foregroundServiceType includes specialUse. */
  readonly specialUseSubtype?: string;
  /** Notification shown while this logical service runs in the foreground. */
  readonly notification: {
    /** Application notification channel identity. */
    readonly channelId: string;
    /** User-visible notification title. */
    readonly title: string;
    /** User-visible notification body. */
    readonly text: string;
  };
}

/** Verified build inputs for a logical service, independent of page plugins. */
export interface MuonServiceSettings {
  /** Unique application-local identity, also safe as a private directory name. */
  readonly id: string;
  /** Manifest default; a saved user preference takes precedence at runtime. */
  readonly autostart: boolean;
  /** Immutable string values passed to this instance's factory. */
  readonly config: Readonly<Record<string, string>>;
  /** Android execution purpose; required when building Android. */
  readonly android: MuonAndroidServiceConfig | undefined;
}

/** Verified native factory and its logical service settings. */
export interface MuonResolvedNativeService extends MuonServiceSettings {
  /** Executes the portable native service ABI. */ readonly runtime: "native";
  /** Factory manifest and verified binaries for the requested targets. */ readonly artifact: ResolvedServiceArtifact;
}

/** Bundled JavaScript and its logical service settings. */
export interface MuonResolvedQuickJsService extends MuonServiceSettings {
  /** Executes an independent QuickJS heap. */ readonly runtime: "quickjs";
  /** Self-contained JavaScript source, evaluated by the runtime host. */ readonly bundle: string;
  /** Explicit native capabilities; never inherited from a page. */ readonly plugins: readonly MuonServicePlugin[];
}

/** Node project compiled for an independent service child process. */
export interface MuonResolvedNodeService extends MuonServiceSettings {
  /** Executes the shared service SDK in Node.js. */ readonly runtime: "node";
  /** Service project and its runtime engine constraint. */ readonly project: ResolvedMuonNodeProject;
  /** Compiled entry, modules and external npm dependencies. */ readonly build: MuonNodeServiceBuild;
}

/** One independently configured native or JavaScript service. */
export type MuonResolvedService =
  | MuonResolvedNativeService
  | MuonResolvedQuickJsService
  | MuonResolvedNodeService;

const foregroundTypes = new Set([
  "camera",
  "connectedDevice",
  "dataSync",
  "health",
  "location",
  "mediaPlayback",
  "mediaProcessing",
  "mediaProjection",
  "microphone",
  "phoneCall",
  "remoteMessaging",
  "shortService",
  "specialUse",
  "systemExempted",
]);

const record = (input: unknown, label: string): Record<string, unknown> => {
  if (input === null || typeof input !== "object" || Array.isArray(input))
    throw new Error(`${label} must be an object.`);
  return input as Record<string, unknown>;
};

const text = (input: unknown, label: string): string => {
  if (typeof input !== "string" || input.length === 0 || input.includes("\0"))
    throw new Error(`${label} must be a nonempty string without NUL.`);
  return input;
};

const knownKeys = (
  input: Record<string, unknown>,
  allowed: readonly string[],
  label: string,
): void => {
  for (const key of Object.keys(input)) {
    if (!allowed.includes(key))
      throw new Error(`${label} contains unknown field ${key}.`);
  }
};

const androidConfig = (
  input: unknown,
  label: string,
): MuonAndroidServiceConfig => {
  const value = record(input, label);
  knownKeys(
    value,
    ["foregroundServiceType", "specialUseSubtype", "notification"],
    label,
  );
  const foregroundServiceType = text(
    value.foregroundServiceType,
    `${label}.foregroundServiceType`,
  );
  const types = foregroundServiceType.split("|");
  if (
    new Set(types).size !== types.length ||
    types.some((type) => !foregroundTypes.has(type))
  )
    throw new Error(
      `${label}.foregroundServiceType contains unknown or duplicate types.`,
    );
  const notification = record(value.notification, `${label}.notification`);
  knownKeys(
    notification,
    ["channelId", "title", "text"],
    `${label}.notification`,
  );
  const specialUseSubtype =
    value.specialUseSubtype === undefined
      ? undefined
      : text(value.specialUseSubtype, `${label}.specialUseSubtype`);
  if (types.includes("specialUse") && specialUseSubtype === undefined)
    throw new Error(`${label}.specialUseSubtype is required for specialUse.`);
  if (!types.includes("specialUse") && specialUseSubtype !== undefined)
    throw new Error(`${label}.specialUseSubtype requires the specialUse type.`);
  return {
    foregroundServiceType,
    ...(specialUseSubtype === undefined ? {} : { specialUseSubtype }),
    notification: {
      channelId: text(
        notification.channelId,
        `${label}.notification.channelId`,
      ),
      title: text(notification.title, `${label}.notification.title`),
      text: text(notification.text, `${label}.notification.text`),
    },
  };
};

/**
 * Validates configured services and resolves native factories or builds JavaScript.
 * @param input - Parsed muon.json services map, or undefined when absent.
 * @param directory - Directory of the configuration declaring artifact paths.
 * @param targets - Actual application targets whose binaries must be present.
 * @param options - Package resolution and source-build environment.
 * @returns Independent logical definitions with verified absolute artifact paths.
 * @remarks Native factories remain separate from the page plugin catalog. The
 * same factory may implement several IDs with different policy and configuration.
 */
export const resolveMuonServices = async (
  input: unknown,
  directory: string,
  targets: readonly MuonPluginTarget[],
  options?: MuonServiceBuildOptions,
): Promise<readonly MuonResolvedService[]> => {
  if (input === undefined) return [];
  const services = record(input, "services");
  for (const id of Object.keys(services)) {
    if (!/^[A-Za-z][A-Za-z0-9_.-]{0,63}$/u.test(id))
      throw new Error(`Invalid service identity: ${id}`);
  }
  const plugins = await resolveMuonServicePlugins(
    services,
    directory,
    targets,
    options ?? { root: directory, environment: process.env },
  );
  const result: MuonResolvedService[] = [];
  for (const [id, definition] of Object.entries(services)) {
    const label = `services.${id}`;
    const value = record(definition, label);
    knownKeys(
      value,
      [
        "runtime",
        "artifact",
        "entry",
        "project",
        "autostart",
        "config",
        "android",
        "plugins",
      ],
      label,
    );
    if (
      value.runtime !== "native" &&
      value.runtime !== "quickjs" &&
      value.runtime !== "node"
    )
      throw new Error(
        `${label}.runtime must be native, quickjs or node; this build does not support ${String(value.runtime)} services.`,
      );
    const androidTarget = targets.find((target) =>
      target.startsWith("android-"),
    );
    if (value.runtime === "node" && androidTarget !== undefined)
      throw new Error(
        `${label}: node services are not supported for ${androidTarget}; use native or quickjs.`,
      );
    if (value.runtime !== "node" && value.project !== undefined)
      throw new Error(`${label}.project requires the node runtime.`);
    if (value.autostart !== undefined && typeof value.autostart !== "boolean")
      throw new Error(`${label}.autostart must be boolean.`);
    const config = Object.fromEntries(
      Object.entries(
        record(
          value.config === undefined ? {} : value.config,
          `${label}.config`,
        ),
      ).map(([key, item]) => {
        if (
          key.length === 0 ||
          key.includes("\0") ||
          typeof item !== "string" ||
          item.includes("\0")
        )
          throw new Error(
            `${label}.config must contain string keys and values without NUL.`,
          );
        return [key, item];
      }),
    );
    const android =
      value.android === undefined
        ? undefined
        : androidConfig(value.android, `${label}.android`);
    if (
      android === undefined &&
      targets.some((target) => target.startsWith("android-"))
    )
      throw new Error(
        `${label}.android must declare the foreground execution purpose and notification.`,
      );
    const settings = {
      id,
      autostart: value.autostart ?? false,
      config,
      android,
    };
    if (value.runtime === "native") {
      if (value.entry !== undefined)
        throw new Error(`${label}.entry requires the quickjs or node runtime.`);
      const artifact = await readServiceArtifact(
        resolve(directory, text(value.artifact, `${label}.artifact`)),
        targets,
      );
      result.push({ ...settings, runtime: "native", artifact });
    } else if (value.runtime === "node") {
      if (value.artifact !== undefined)
        throw new Error(`${label}.artifact requires the native runtime.`);
      try {
        const project = (await resolveMuonNodeProject(
          { node: { project: text(value.project, `${label}.project`) } },
          directory,
        ))!;
        const build = await buildMuonNodeService(
          project,
          text(value.entry, `${label}.entry`),
        );
        result.push({ ...settings, runtime: "node", project, build });
      } catch (error) {
        throw new Error(
          `${label}: ${error instanceof Error ? error.message : String(error)}`,
        );
      }
    } else {
      if (value.artifact !== undefined)
        throw new Error(`${label}.artifact requires the native runtime.`);
      const entry = resolve(directory, text(value.entry, `${label}.entry`));
      const nativePlugins = plugins.get(id) ?? [];
      result.push({
        ...settings,
        runtime: "quickjs",
        bundle: await buildMuonQuickJsService(
          entry,
          getMuonServicePluginModules(nativePlugins),
        ),
        plugins: nativePlugins,
      });
    }
  }
  return result;
};

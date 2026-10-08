// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { readFile, readdir, realpath, stat } from "node:fs/promises";
import { isAbsolute, join, relative, resolve } from "node:path";
import {
  buildAndroidApplication,
  type MuonAndroidApplicationResult,
} from "../../core/android/src/build.js";
import { prepareAndroid } from "../../core/android/src/toolchain.js";
import { validateMuonAndroidConfig } from "../../core/android/src/renderer/android-config.js";
import type { MuonAndroidOptions } from "../android.js";
import type { MuonBuildOptions } from "./build.js";
import type { MuonProgressCallback } from "./progress.js";
import {
  createAppIconOptionsSource,
  resolveMuonAppIconPath,
} from "./app-icon.js";
import { resolveAndroidPlugins } from "../../core/android/src/plugins.js";
import { resolveMuonAndroidPluginAccess } from "./android-plugin-access.js";
import { readAndroidPluginMetadata } from "../../core/android/src/plugin-metadata.js";
import {
  resolveAndroidSigning,
  signAndroidApplication,
} from "../../core/android/src/signing.js";

/** Result of an Android build, discriminated by target without desktop fields. */
export interface MuonAndroidBuildTargetResult extends MuonAndroidApplicationResult {
  /** Public signer certificate digest, present for verified release APKs. */
  readonly certificateSha256?: string;
  /** Fixed Android output directory relative to the output root. */
  readonly distributionDirectoryName: "dist-muon/android";
  /** Absolute directory containing the APK and its metadata. */
  readonly outputPath: string;
}

type RecordValue = Record<string, unknown>;
const object = (value: unknown, label: string): RecordValue => {
  if (value === undefined) return {};
  if (value === null || typeof value !== "object" || Array.isArray(value))
    throw new Error(`${label} must be an object.`);
  return value as RecordValue;
};
const string = (value: unknown, label: string): string => {
  if (typeof value !== "string" || value.trim() === "" || value.includes("\0"))
    throw new Error(`${label} must be a nonempty string without NUL.`);
  return value;
};
const strings = (value: unknown, label: string): string[] => {
  if (!Array.isArray(value))
    throw new Error(`${label} must be an array of strings.`);
  return [...new Set(value.map((entry) => string(entry, label)))];
};

const assertKeystoreIsNotPackaged = async (
  assets: string,
  keystore: string,
): Promise<void> => {
  const key = await readFile(keystore);
  const pending = [assets];
  const visited = new Set<string>();
  while (pending.length > 0) {
    const path = await realpath(pending.pop()!);
    if (visited.has(path)) continue;
    visited.add(path);
    const info = await stat(path);
    if (info.isDirectory()) {
      for (const name of await readdir(path)) pending.push(join(path, name));
    } else if (
      info.isFile() &&
      info.size === key.length &&
      (await readFile(path)).equals(key)
    ) {
      throw new Error(
        "The Android keystore must not be included in web assets, including copied or renamed files.",
      );
    }
  }
};
const settings = (
  root: string,
  config: RecordValue,
  configDirectory: string,
  options: MuonAndroidOptions | undefined,
) => {
  const configured = object(config.android, "android");
  const merged = { ...configured, ...options };
  const path = (key: "sdkPath" | "icon"): string | undefined => {
    const value = merged[key];
    return value === undefined
      ? undefined
      : resolve(
          options?.[key] === undefined ? configDirectory : root,
          string(value, `android.${key}`),
        );
  };
  return {
    merged,
    sdkPath: path("sdkPath"),
    icon: path("icon"),
    pluginsDirectory: options?.plugins === undefined ? configDirectory : root,
  };
};

/**
 * Resolves the SDK override with the same precedence as a build.
 * @param root - Project root for explicit option paths.
 * @param packageDirectory - Installed Muon distribution.
 * @param config - Parsed project configuration.
 * @param configDirectory - Base directory for configuration paths.
 * @param options - Explicit Android options, or undefined.
 * @param environment - Child process environment.
 * @returns Validated tools after preparing the Gradle Wrapper.
 */
export const prepareMuonAndroidTarget = async (
  root: string,
  packageDirectory: string,
  config: RecordValue,
  configDirectory: string,
  options: MuonAndroidOptions | undefined,
  environment: NodeJS.ProcessEnv,
) => {
  const resolved = settings(root, config, configDirectory, options);
  return await prepareAndroid({
    componentsDirectory: join(packageDirectory, "android"),
    sdkPath: resolved.sdkPath,
    environment,
    prepareGradle: true,
  });
};

/**
 * Validates public app inputs before invoking the packaged Android builder.
 * @param input - Project, asset and configuration sources with their base paths.
 * @returns Android APK information without desktop runtime fields.
 */
export const buildMuonAndroidTarget = async (input: {
  root: string;
  packageDirectory: string;
  packageJson: RecordValue;
  config: RecordValue;
  configDirectory: string;
  options: MuonBuildOptions;
  assets: { sourcePath: string; prefix: string };
  environment: NodeJS.ProcessEnv;
  progress: MuonProgressCallback | undefined;
}): Promise<MuonAndroidBuildTargetResult> => {
  const { root, options, config, packageJson } = input;
  const resolved = settings(
    root,
    config,
    input.configDirectory,
    options.android,
  );
  const android = resolved.merged;
  const signing =
    options.androidRelease === true
      ? resolveAndroidSigning(
          android.signing,
          options.android?.signing === undefined ? input.configDirectory : root,
          input.environment,
        )
      : undefined;
  if (signing !== undefined) {
    const withinAssets = relative(input.assets.sourcePath, signing.keystore);
    if (
      withinAssets === "" ||
      (!withinAssets.startsWith("..") && !isAbsolute(withinAssets))
    ) {
      throw new Error(
        "The Android keystore must be outside the web assets directory.",
      );
    }
    await assertKeystoreIsNotPackaged(
      input.assets.sourcePath,
      signing.keystore,
    );
  }
  for (const [key, label] of [
    ["fcm", "FCM"],
    ["quickjs", "QuickJS"],
  ] as const) {
    if (android[key] !== undefined && android[key] !== false)
      throw new Error(
        `${label} is unavailable in Android application builds; android.${key} must be false or omitted.`,
      );
  }
  const { warnings } = validateMuonAndroidConfig(config);
  for (const warning of warnings) process.stderr.write(`Warning: ${warning}\n`);
  if (
    config.node !== undefined &&
    Object.keys(object(config.node, "node")).length > 0
  )
    throw new Error("node is unavailable in Android application builds.");
  const browser = object(config.browser, "browser");
  if (
    browser.initialWindowState !== undefined &&
    browser.initialWindowState !== "normal"
  )
    throw new Error(
      "Android browser.initialWindowState currently supports normal only; use the runtime fullscreen API.",
    );
  if (
    browser.contextMenu !== undefined &&
    object(browser.contextMenu, "browser.contextMenu").mode !== "standard"
  )
    throw new Error(
      "Android browser.contextMenu currently supports standard only.",
    );

  const defaultName =
    typeof packageJson.name === "string" ? packageJson.name : "application";
  const defaultId = `dev.muon.${defaultName
    .replace(/^@[^/]+\//u, "")
    .replace(/[^A-Za-z0-9_]/gu, "_")
    .replace(/^[^A-Za-z]/u, "app_")}`;
  const applicationId = string(
    options.android?.applicationId ??
      options.appId ??
      android.applicationId ??
      defaultId,
    "android.applicationId",
  );
  if (
    !/^[A-Za-z][A-Za-z0-9_]*(?:\.[A-Za-z][A-Za-z0-9_]*)+$/u.test(applicationId)
  )
    throw new Error(
      "android.applicationId must contain at least two Java-style identifier segments.",
    );
  const versionCode = android.versionCode ?? 1;
  if (
    !Number.isInteger(versionCode) ||
    typeof versionCode !== "number" ||
    versionCode < 1 ||
    versionCode > 2100000000
  )
    throw new Error(
      "android.versionCode must be an integer from 1 to 2100000000.",
    );
  const abis = strings(
    android.abis ?? ["arm64-v8a", "x86_64"],
    "android.abis",
  ).map((abi) => {
    if (abi !== "arm64-v8a" && abi !== "x86_64")
      throw new Error("android.abis supports arm64-v8a and x86_64 only.");
    return abi;
  });
  if (abis.length === 0) throw new Error("android.abis must not be empty.");
  const catalogs = await readAndroidPluginMetadata(
    android.plugins ?? [],
    abis,
    resolved.pluginsDirectory,
    options.runtimePluginConfig?.mode === "validate",
  );
  const pluginConfiguration = resolveMuonAndroidPluginAccess(
    config,
    options.runtimePluginConfig,
    android.plugins ?? [],
    catalogs,
  );
  const permissions = strings(android.permissions ?? [], "android.permissions");
  if (
    permissions.some(
      (permission) =>
        !/^android\.permission\.[A-Z][A-Z0-9_]*$/u.test(permission),
    )
  )
    throw new Error(
      "android.permissions must contain Android platform permission names.",
    );
  const values = Object.fromEntries(
    Object.entries(object(config.config, "config")).map(([key, value]) => {
      if (typeof value !== "string")
        throw new Error("muon.json config values must be strings.");
      return [key, value];
    }),
  );
  const prefix = input.assets.prefix;
  if (prefix !== "" && prefix !== "main" && !prefix.startsWith("main/"))
    throw new Error("Android assetPrefix must use the main asset host.");
  const assetPath = prefix.replace(/^main\/?/u, "").replace(/\/+$/u, "");
  const rawStartPage = string(
    browser.startPage ??
      options.browserStartPage ??
      `https://main.asset.muon.invalid/${assetPath === "" ? "" : `${assetPath}/`}index.html`,
    "browser.startPage",
  );
  const startPage = rawStartPage.replace(
    /^asset:\/\/main\//u,
    "https://main.asset.muon.invalid/",
  );
  const url = new URL(startPage);
  if (
    url.origin !== "https://main.asset.muon.invalid" ||
    url.username ||
    url.password
  )
    throw new Error(
      "Android browser.startPage must use asset://main/ or https://main.asset.muon.invalid/.",
    );
  if (!(await stat(input.assets.sourcePath)).isDirectory())
    throw new Error(
      "Android web assets must be a directory; ZIP assets are not supported.",
    );
  const iconValue =
    options.android?.icon === undefined
      ? (options.iconPath ?? resolved.icon ?? config.iconPath)
      : resolved.icon;
  const iconSource = createAppIconOptionsSource(
    iconValue,
    resolved.icon !== undefined || options.iconPath !== undefined
      ? root
      : input.configDirectory,
  );
  const icon = await resolveMuonAppIconPath(
    iconSource === undefined ? [] : [iconSource],
  );
  const plugins = await resolveAndroidPlugins(
    pluginConfiguration.plugins,
    abis,
    resolved.pluginsDirectory,
  );
  const outputPath = resolve(
    root,
    options.outputRoot ?? ".",
    "dist-muon/android",
  );
  input.progress?.({ phase: "build", status: "Building Android APK" });
  let result = await buildAndroidApplication({
    componentsDirectory: join(input.packageDirectory, "android"),
    assetsDirectory: input.assets.sourcePath,
    assetPath,
    startPage,
    applicationId,
    label: string(
      options.android?.label ?? options.appName ?? android.label ?? defaultName,
      "android.label",
    ),
    versionCode,
    versionName: string(
      android.versionName ?? packageJson.version ?? "0.0.0",
      "android.versionName",
    ),
    abis,
    permissions,
    values,
    pluginAccess: pluginConfiguration.pluginAccess,
    plugins,
    icon,
    projectDirectory: resolve(root, ".muon/android"),
    outputDirectory: outputPath,
    sdkPath: resolved.sdkPath,
    variant: signing === undefined ? "debug" : "release",
    environment: input.environment,
    output:
      input.progress === undefined
        ? undefined
        : (text) => input.progress?.({ phase: "build", status: text.trim() }),
  });
  if (signing !== undefined) {
    const tools = await prepareAndroid({
      componentsDirectory: join(input.packageDirectory, "android"),
      sdkPath: resolved.sdkPath,
      environment: input.environment,
      prepareGradle: false,
    });
    result = await signAndroidApplication(
      result,
      signing,
      { sdkPath: tools.sdkPath, buildTools: tools.toolchain.buildTools },
      input.environment,
    );
  }
  return {
    ...result,
    outputPath,
    distributionDirectoryName: "dist-muon/android",
  };
};

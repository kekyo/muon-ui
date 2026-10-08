// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { type MuonAndroidBuildTargetResult } from "../android/target.js";
import type { MuonAndroidOptions } from "../android.js";

import { type MuonTarget, type MuonDesktopTarget } from "./targets.js";
import { type MuonWindowsResourceOptions } from "../cef/windows-resource.js";
import {
  type MuonLinuxDesktopOptions,
  type ResolvedMuonLinuxDesktop,
} from "../cef/linux-desktop.js";
import { type MuonWindowsCodeSigningOptions } from "../cef/windows-code-signing.js";

import type { MuonRuntimePluginConfig } from "./capability.js";
import type { MuonProgressCallback } from "./progress.js";

import type { JsonObject } from "./build-config.js";

/** Internal build execution settings supplied by CLI and Vite orchestration. */
export interface InternalMuonBuildOptions extends MuonBuildOptions {
  /** Browser profile path relative to the generated application. */
  browserProfilePathOverride: string | undefined;
  /** Environment for build subprocesses. */
  environment?: NodeJS.ProcessEnv;
  /** Receives progress from backend build operations. */
  progress?: MuonProgressCallback;
}

/**
 * Public muon runtime target used by CLI, Vite options, and package layout.
 */
export type MuonBuildTarget = MuonTarget;

/**
 * Options for creating redistributable muon app directories.
 */
export interface MuonBuildOptions {
  /** Requests the signed release path used by Android packaging. @internal */
  androidRelease?: boolean;
  /** Android application metadata and prebuilt plugins. */
  android?: MuonAndroidOptions;
  /**
   * Project root containing package.json, muon.json, and app assets.
   */
  root?: string;
  /**
   * Directory containing package runtime/ and native/ folders.
   *
   * @remarks This defaults to the installed muon package dist directory.
   */
  packageDirectory?: string;
  /**
   * Public target identifiers to build.
   */
  targets?: readonly string[];
  /**
   * Build every supported target from the installed package.
   *
   * @remarks Defaults to true when targets is omitted. Set false to build only
   * the host target.
   */
  allTargets?: boolean;
  /**
   * File name used for the app launcher.
   *
   * @remarks The .exe suffix is added automatically for Windows targets.
   */
  appName?: string;
  /**
   * Stable base application identifier used for runtime app identity.
   *
   * @remarks Windows target distributions embed `<appId>.<arch>` as their
   * runtime app identifier. Linux targets embed this value unchanged.
   */
  appId?: string;
  /**
   * Parent directory that receives dist-muon/linux-amd64/ style outputs.
   */
  outputRoot?: string;
  /**
   * Directory or ZIP file used as app assets.
   */
  assetSourcePath?: string;
  /**
   * Optional ZIP entry prefix used for the asset source.
   */
  assetPrefix?: string;
  /**
   * Default browser start page embedded when muon config omits one.
   */
  browserStartPage?: string;
  /**
   * muon config path to embed.
   */
  configPath?: string;
  /**
   * Static application icon PNG file path.
   *
   * @remarks This icon is used as the shared source for Windows PE/NSIS,
   * Linux desktop entries, and the generated initial title bar icon asset.
   * Platform-specific icon paths override it for their target.
   */
  iconPath?: string;
  /**
   * Additional project files copied next to the generated app launcher.
   *
   * @remarks When omitted, `package.json` `files` is used as a candidate list.
   * Only regular files are copied. Asset input paths, `node_modules`, `.git`,
   * and generated target output directories are excluded.
   */
  distributionFiles?: readonly string[];
  /**
   * Windows PE and NSIS resource metadata.
   *
   * @defaultValue Uses `muon.json` `windows.resource`, `project.json`,
   * `package.json`, then muon defaults.
   */
  windowsResource?: MuonWindowsResourceOptions;
  /**
   * Windows code signing command for generated executable artifacts.
   *
   * @remarks Set false to disable `muon.json` `windows.codeSigning`.
   * The command is supplied by the application project or CI environment.
   */
  windowsCodeSigning?: false | MuonWindowsCodeSigningOptions;
  /**
   * Linux desktop entry metadata.
   *
   * @defaultValue Uses `muon.json` `linux.desktop`, package metadata, then
   * muon defaults.
   */
  linuxDesktop?: MuonLinuxDesktopOptions;
  /**
   * Asset salt override for deterministic tests.
   *
   * @remarks Production builds should omit this option.
   */
  assetSalt?: Uint8Array;
  /**
   * Runtime plugin configuration supplied by a bundler integration.
   *
   * @internal
   */
  runtimePluginConfig?: MuonRuntimePluginConfig;
  /**
   * Include a privileged Linux runtime helper in generated distributions.
   *
   * @internal
   */
  includeRuntimeHelper?: boolean;
}

/**
 * Asset metadata generated for a target distribution.
 */
export interface MuonBuildAssetResult {
  /**
   * Path of the generated assets.zip.
   */
  path: string;
  /**
   * 64-character lowercase hexadecimal SHA-256 digest of the generated asset
   * archive bytes followed by the decoded salt bytes.
   */
  signature: string;
  /**
   * Hex-encoded salt embedded into muon.json.
   */
  salt: string;
  /**
   * Number of files written to the ZIP archive.
   */
  entryCount: number;
}

/**
 * Result for one generated target distribution.
 */
export interface MuonDesktopBuildTargetResult {
  /**
   * Public target identifier used by the muon npm package.
   */
  target: MuonDesktopTarget;
  /**
   * Fixed output directory path for the target.
   */
  distributionDirectoryName: string;
  /**
   * Absolute path of the generated target directory.
   */
  outputPath: string;
  /**
   * Absolute path of the app launcher copied from muon-launcher.
   */
  launcherPath: string;
  /**
   * Absolute path of the privileged runtime helper when included.
   */
  runtimeHelperPath?: string;
  /**
   * Generated asset archive metadata.
   */
  asset: MuonBuildAssetResult;
  /**
   * Application identifier embedded into this target distribution.
   *
   * @remarks Windows targets append the target architecture to the base
   * `appId`; Linux targets use the base `appId` unchanged.
   */
  runtimeAppId: string;
  /**
   * Config object embedded into muon-core.
   *
   * @remarks Launcher-only generated settings are intentionally excluded.
   */
  embeddedConfig: JsonObject;
  /**
   * Linux desktop integration metadata when the target is Linux.
   */
  linuxDesktop?: ResolvedMuonLinuxDesktop;
}

/** A target result discriminated by its target field. */
export type MuonBuildTargetResult =
  MuonDesktopBuildTargetResult | MuonAndroidBuildTargetResult;

/**
 * Result of a muon app build.
 */
export interface MuonBuildResult {
  /**
   * Absolute project root used for the build.
   */
  root: string;
  /**
   * Sanitized app launcher base name.
   */
  appName: string;
  /**
   * Stable base application identifier used to derive target runtime identity.
   *
   * @remarks See each target's `runtimeAppId` for the identifier embedded into
   * that target distribution.
   */
  appId: string;
  /**
   * Generated target distributions.
   */
  targets: MuonBuildTargetResult[];
}

// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import type { MuonAndroidOptions } from "../android.js";

import { type MuonBuildResult } from "./build.js";

import { type MuonTarget } from "./targets.js";
import { type MuonWindowsResourceOptions } from "../cef/windows-resource.js";
import { type MuonWindowsCodeSigningOptions } from "../cef/windows-code-signing.js";

import { type MuonLinuxDesktopOptions } from "../cef/linux-desktop.js";

/**
 * muon package output type.
 */
export type MuonPackType = "zip" | "tar.gz" | "deb" | "nsis" | "apk";

/**
 * Linux CEF sandbox strategy used by deb packages.
 */
export type MuonLinuxSandboxMode = "disabled" | "setuid";

/**
 * Options for creating redistributable muon package artifacts.
 */
export interface MuonPackOptions {
  /** Android options overriding Vite and muon.json settings. */
  android?: MuonAndroidOptions;
  /**
   * Project root.
   */
  root?: string;
  /**
   * Package artifact types to generate.
   *
   * @remarks Defaults to zip, tar.gz, deb, and nsis when omitted.
   */
  types?: readonly string[];
  /**
   * Public target identifiers to build.
   */
  targets?: readonly string[];
  /**
   * Build every supported target.
   */
  allTargets?: boolean;
  /**
   * muon config path to embed.
   */
  configPath?: string;
  /**
   * Static application icon PNG file path.
   *
   * @remarks Used for Windows PE/NSIS resources, Linux desktop entries, and
   * the generated initial title bar icon asset unless a target-specific icon
   * override is supplied.
   */
  iconPath?: string;
  /**
   * File name used for the app launcher.
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
   * Windows PE and NSIS resource metadata.
   *
   * @defaultValue Uses Vite build options, `muon.json` `windows.resource`,
   * `project.json`, `package.json`, then muon defaults.
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
   * @defaultValue Uses Vite build options, `muon.json` `linux.desktop`,
   * package metadata, then muon defaults.
   */
  linuxDesktop?: MuonLinuxDesktopOptions;
  /**
   * Linux deb CEF sandbox mode.
   *
   * @remarks `setuid` is opt-in and valid only when packaging Linux deb
   * artifacts. The default `disabled` preserves the existing no-sandbox
   * runtime behavior.
   */
  linuxSandbox?: string;
  /**
   * Directory containing package runtime/ and native/ folders.
   */
  packageDirectory?: string;
  /**
   * Directory that receives generated package artifacts.
   */
  artifactsDir?: string;
  /**
   * Package name override.
   */
  packageName?: string;
  /**
   * Package version override.
   */
  packageVersion?: string;
  /**
   * Package description override.
   */
  description?: string;
  /**
   * Package author/maintainer override.
   */
  author?: string;
  /**
   * Environment used for child processes.
   */
  environment?: NodeJS.ProcessEnv;
}

/**
 * Generated package artifact metadata.
 */
export interface MuonPackArtifact {
  /**
   * Package artifact type.
   */
  type: MuonPackType;
  /**
   * muon target packaged in this artifact.
   */
  target: MuonTarget;
  /**
   * Generated artifact path.
   */
  path: string;
}

/**
 * Result of a muon package build.
 */
export interface MuonPackResult {
  /**
   * Absolute project root used for packaging.
   */
  root: string;
  /**
   * Debian/installer safe package name.
   */
  packageName: string;
  /**
   * Package version.
   */
  version: string;
  /**
   * Launcher file base name.
   */
  appName: string;
  /**
   * Stable base app identifier used to derive target runtime app identifiers.
   */
  appId: string;
  /**
   * muon dist build result.
   */
  build: MuonBuildResult;
  /**
   * Target dist directories used as package inputs.
   */
  targets: MuonBuildResult["targets"];
  /**
   * Generated package artifacts.
   */
  artifacts: MuonPackArtifact[];
}

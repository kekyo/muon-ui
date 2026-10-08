// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

/** ABI supported by the prebuilt Muon Android runtime. */
export type MuonAndroidAbi = "arm64-v8a" | "x86_64";

/** Prebuilt native plugin bundled in the application. */
export interface MuonAndroidPluginOptions {
  /** Unique plugin registration name. */
  name: string;
  /** ELF soname, including the lib prefix and .so suffix. */
  soname: string;
  /** Library path for each ABI selected by android.abis. */
  libraries: Partial<Record<MuonAndroidAbi, string>>;
  /** Producer catalog JSON path, required for validate mode; resolved like libraries. */
  metadata?: string;
}

/** Android application options, overriding the muon.json android section. */
export interface MuonAndroidOptions {
  /** External keystore and environment variable references used by muon pack. */
  signing?: MuonAndroidSigningOptions;
  /** Stable reverse-domain application identifier used for installation and updates. */
  applicationId?: string;
  /** Launcher label; defaults to the package name. */
  label?: string;
  /** Positive integer update version; defaults to 1. Increase for every release. */
  versionCode?: number;
  /** Display version; defaults to package.json version. */
  versionName?: string;
  /** Native ABIs to package; defaults to both supported ABIs. */
  abis?: readonly MuonAndroidAbi[];
  /** PNG launcher icon; relative to the configuration that supplies it. */
  icon?: string;
  /** Android manifest permissions. Runtime permission prompts remain app-specific. */
  permissions?: readonly string[];
  /** SDK directory, otherwise detected through the environment. */
  sdkPath?: string;
  /** Prebuilt native plugins; no NDK is required when building the app. */
  plugins?: readonly MuonAndroidPluginOptions[];
  /** FCM is unavailable in this release. Only false is accepted. */
  fcm?: false;
  /** QuickJS is unavailable in this release. Only false is accepted. */
  quickjs?: false;
}

/** External release credentials; passwords must not appear in project files. */
export interface MuonAndroidSigningOptions {
  /** Path to the application keystore; relative to the supplying configuration. */
  keystore: string;
  /** Key alias inside the keystore. */
  keyAlias: string;
  /** Environment variable containing the keystore password. */
  storePasswordEnv: string;
  /** Environment variable containing the key password; defaults to storePasswordEnv. */
  keyPasswordEnv?: string;
}

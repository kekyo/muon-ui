// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { existsSync } from "node:fs";
import { join } from "node:path";

/**
 * Selects modules compatible with the staged Linux runtime before starting it.
 * @param runtimeDirectory Absolute directory containing the staged muon-core.
 * @param platform Platform on which the child process will run.
 * @param environment Inherited environment, which is never modified.
 * @returns Child environment with bundled module paths, or the inherited
 * environment when the runtime does not bundle Linux modules.
 * @remarks Matches the distribution launcher in builder/src/muon_launcher.c.
 * GTK_IM_MODULE remains the user's input method choice. Library paths must be
 * set before spawn so dynamically loaded GTK/GIO modules use the same baseline.
 */
export const createMuonRuntimeEnvironment = (
  runtimeDirectory: string,
  platform: NodeJS.Platform,
  environment: NodeJS.ProcessEnv,
): NodeJS.ProcessEnv => {
  const moduleRoot = join(runtimeDirectory, "linux");
  if (platform !== "linux" || !existsSync(moduleRoot)) {
    return environment;
  }
  return {
    ...environment,
    GTK_EXE_PREFIX: moduleRoot,
    GTK_PATH: join(moduleRoot, "lib", "gtk-3.0"),
    GTK_IM_MODULE_FILE: join(moduleRoot, "gtk-immodules.cache"),
    GDK_PIXBUF_MODULE_FILE: join(moduleRoot, "gdk-pixbuf-loaders.cache"),
    MUON_LINUX_MODULE_ROOT: moduleRoot,
    XDG_DATA_DIRS:
      join(moduleRoot, "share") +
      ":" +
      (environment.XDG_DATA_DIRS || "/usr/local/share:/usr/share"),
    LD_LIBRARY_PATH:
      runtimeDirectory +
      (environment.LD_LIBRARY_PATH ? `:${environment.LD_LIBRARY_PATH}` : ""),
  };
};

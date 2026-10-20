// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFile } from "node:child_process";
import {
  chmod,
  cp,
  mkdir,
  mkdtemp,
  rm,
  symlink,
  writeFile,
} from "node:fs/promises";
import { tmpdir } from "node:os";
import { basename, dirname, join } from "node:path";
import { promisify } from "node:util";

import { runScriptOnceToText, type FunCityLogEntry } from "funcity";

import {
  createLinuxDesktopEntry,
  type ResolvedMuonLinuxDesktop,
} from "./linux-desktop.js";
import type { LinuxAppImageTools } from "./appimage-tools.js";

const execute = promisify(execFile);

/**
 * Creates a relocatable AppDir whose launcher prepares writable per-user state.
 * @param source - Built muon application distribution, without CEF.
 * @param launcher - Launcher file name inside that distribution.
 * @param desktop - Resolved application desktop metadata and icon.
 * @param directory - AppDir destination owned by the packaging operation.
 */
export const createLinuxAppDir = async (
  source: string,
  launcher: string,
  desktop: ResolvedMuonLinuxDesktop,
  directory: string,
): Promise<void> => {
  if (basename(launcher) !== launcher)
    throw new Error(`Invalid AppImage launcher file name: ${launcher}`);
  await rm(directory, { recursive: true, force: true });
  const payload = join(directory, "usr/lib/muon");
  await mkdir(dirname(payload), { recursive: true });
  await cp(source, payload, { recursive: true });
  await writeFile(
    join(payload, "muon-install.json"),
    `${JSON.stringify({ type: "appimage", runtimeMode: "user" }, null, 2)}\n`,
  );
  const logs: FunCityLogEntry[] = [];
  const appRun = await runScriptOnceToText(
    '#!/bin/sh\nset -eu\nappdir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)\nruntime="$appdir/usr/lib/muon"\nexport LD_LIBRARY_PATH="$runtime${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"\nexec "$runtime"/{{launcher}} "$@"\n',
    {
      sourceId: "muon-appimage-apprun",
      logs,
      variables: new Map([
        ["launcher", `'${launcher.replaceAll("'", "'\\''")}'`],
      ]),
    },
  );
  if (appRun === undefined || logs.some((entry) => entry.type === "error"))
    throw new Error(`Cannot render AppRun: ${JSON.stringify(logs)}`);
  await writeFile(join(directory, "AppRun"), appRun, { mode: 0o755 });
  const applications = join(directory, "usr/share/applications");
  const icons = join(directory, "usr/share/icons/hicolor/256x256/apps");
  await mkdir(applications, { recursive: true });
  await mkdir(icons, { recursive: true });
  const desktopName = `${desktop.desktopId}.desktop`;
  const iconName = `${desktop.desktopId}.png`;
  await writeFile(
    join(applications, desktopName),
    createLinuxDesktopEntry({
      desktop,
      exec: "AppRun",
      tryExec: "AppRun",
      icon: desktop.desktopId,
      appId: undefined,
      launcherPath: undefined,
    }),
  );
  await cp(join(source, desktop.iconFileName), join(icons, iconName));
  await symlink(
    `usr/share/applications/${desktopName}`,
    join(directory, desktopName),
  );
  await symlink(
    `usr/share/icons/hicolor/256x256/apps/${iconName}`,
    join(directory, iconName),
  );
  await symlink(iconName, join(directory, ".DirIcon"));
};

/**
 * Converts an AppDir to a type-2 AppImage with a pinned target runtime.
 * @param directory - Complete AppDir to package.
 * @param output - Resulting executable AppImage path.
 * @param tools - Verified host tool and target runtime.
 * @param version - Application version stored by appimagetool.
 * @param environment - Build environment; ARCH and VERSION are set explicitly.
 */
export const createLinuxAppImage = async (
  directory: string,
  output: string,
  tools: LinuxAppImageTools,
  version: string,
  environment: NodeJS.ProcessEnv,
): Promise<void> => {
  await mkdir(dirname(output), { recursive: true });
  // Extract the build tool itself so packaging does not require FUSE.
  // --runtime-file prevents appimagetool from fetching its moving default.
  // Each invocation needs its own extraction directory: the type-2 runtime
  // otherwise derives a shared directory from the tool's content hash.
  const temporary = await mkdtemp(join(tmpdir(), "muon-appimagetool-"));
  try {
    await execute(
      tools.executable,
      [
        "--appimage-extract-and-run",
        "--no-appstream",
        "--runtime-file",
        tools.runtime,
        "--mksquashfs-opt",
        "-processors",
        "--mksquashfs-opt",
        "2",
        directory,
        output,
      ],
      {
        env: {
          ...environment,
          ARCH: tools.architecture,
          VERSION: version,
          TMPDIR: temporary,
        },
        maxBuffer: 10 * 1024 * 1024,
      },
    );
  } finally {
    await rm(temporary, { recursive: true, force: true });
  }
  await chmod(output, 0o755);
};

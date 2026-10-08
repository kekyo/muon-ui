import { basename } from 'node:path';
import type { PluginBinaryInfo } from './binary.js';
import { getPluginTarget, type MuonPluginTarget } from './targets.js';

const androidSystem = [
  'libc.so',
  'libm.so',
  'libdl.so',
  'liblog.so',
  'libandroid.so',
  'libjnigraphics.so',
  'libz.so',
  'libEGL.so',
  'libGLESv2.so',
  'libGLESv3.so',
  'libOpenSLES.so',
  'libvulkan.so',
];
const linuxSystem = [
  'libc.so.6',
  'libm.so.6',
  'libdl.so.2',
  'libpthread.so.0',
  'librt.so.1',
  'libresolv.so.2',
  'libstdc++.so.6',
  'libgcc_s.so.1',
  'libglib-2.0.so.0',
  'libgobject-2.0.so.0',
  'libgio-2.0.so.0',
];
const windowsSystem = [
  'kernel32.dll',
  'user32.dll',
  'advapi32.dll',
  'msvcrt.dll',
  'ucrtbase.dll',
  'ntdll.dll',
  'gdi32.dll',
  'shell32.dll',
  'shlwapi.dll',
  'ole32.dll',
  'oleaut32.dll',
  'ws2_32.dll',
  'bcrypt.dll',
  'crypt32.dll',
  'secur32.dll',
  'version.dll',
  'winmm.dll',
  'comdlg32.dll',
  'comctl32.dll',
  'setupapi.dll',
  'iphlpapi.dll',
  'opengl32.dll',
  'imm32.dll',
  'dwmapi.dll',
  'dbghelp.dll',
];

/** A shared library whose loader metadata has already been validated. */
export interface PluginLibraryInspection {
  /** Library path; its basename is the packaged loader name. */ readonly path: string;
  /** Parsed export/import information. */ readonly info: PluginBinaryInfo;
}

/**
 * Checks packaged dependencies against the runtime and target's system libraries.
 * @param libraries - Plugin entries and ordinary shared dependencies.
 * @param target - Application output target.
 * @returns Resolves normally when the loader can find every declared dependency.
 * @remarks Files provided by muon or the OS must never be replaced by a plugin.
 */
export const validatePluginLibraryDependencies = (
  libraries: readonly PluginLibraryInspection[],
  target: MuonPluginTarget
): void => {
  const os = getPluginTarget(target).os;
  const normalize = (name: string): string =>
    os === 'windows' ? name.toLowerCase() : name;
  const runtime =
    os === 'android'
      ? ['libcardio.so', 'libc++_shared.so', 'libmuon_android_rpc.so']
      : os === 'windows'
        ? [
            'libcardio.dll',
            'libmuon-ui.dll',
            'libcef.dll',
            'libstdc++-6.dll',
            'libgcc_s_seh-1.dll',
            'libgcc_s_dw2-1.dll',
            'libgcc_s_sjlj-1.dll',
            'libwinpthread-1.dll',
          ]
        : ['libcardio.so', 'libmuon-ui.so', 'libcef.so'];
  const reserved = new Set(
    [
      ...runtime,
      ...(os === 'android'
        ? androidSystem
        : os === 'windows'
          ? windowsSystem
          : linuxSystem),
    ].map(normalize)
  );
  const system = (name: string): boolean =>
    reserved.has(normalize(name)) ||
    (os === 'windows' && /^(api|ext)-ms-win-[a-z0-9-]+\.dll$/i.test(name));
  const packaged = new Set(
    libraries.map((entry) => normalize(basename(entry.path)))
  );
  for (const entry of libraries) {
    const name = basename(entry.path);
    if (system(name))
      throw new Error(
        `Native library name is reserved by the runtime or OS: ${name}`
      );
    for (const dependency of entry.info.needed) {
      if (!system(dependency) && !packaged.has(normalize(dependency)))
        throw new Error(
          `${name} requires an unpackaged library: ${dependency}`
        );
      if (
        os === 'linux' &&
        packaged.has(dependency) &&
        !entry.info.runpath.includes('$ORIGIN')
      )
        throw new Error(
          `${name} needs a $ORIGIN runtime search path for packaged dependency ${dependency}.`
        );
    }
  }
};

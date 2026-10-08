/** Supported native plugin compilation targets. */
export type MuonPluginTarget =
  | 'linux-amd64'
  | 'linux-armhf'
  | 'linux-arm64'
  | 'windows-i686'
  | 'windows-amd64'
  | 'android-arm64-v8a'
  | 'android-x86_64';

/** Native compiler and runtime requirements for one target. */
export interface MuonPluginTargetDescriptor {
  /** Public target identifier. */ readonly target: MuonPluginTarget;
  /** Operating system ABI. */ readonly os: 'linux' | 'windows' | 'android';
  /** C compiler executable, relative to the toolchain bin directory. */ readonly cc: string;
  /** C++ compiler executable. */ readonly cxx: string;
  /** Shared library suffix. */ readonly extension: '.so' | '.dll';
  /** Cardio runtime file. */ readonly cardioLibrary: string;
  /** Cardio link input; Windows uses a MinGW import library. */ readonly cardioLinkLibrary: string;
  /** Public cardio compiler definitions shared with the host. */ readonly definitions: readonly string[];
  /** System development packages needed by cardio. */ readonly packages: readonly string[];
  /** Additional platform compiler flags. */ readonly cflags: readonly string[];
  /** Additional platform linker flags. */ readonly ldflags: readonly string[];
}

/** Plugin ABI supported by this SDK and matching muon runtimes. */
export const muonPluginAbiVersion = 1;
/** NDK revision used by the matching Android runtime. */
export const muonPluginNdkVersion = '29.0.14206865';
/** All targets supported by muon-plugin-sdk. */
export const muonPluginTargets: readonly MuonPluginTarget[] = [
  'linux-amd64',
  'linux-armhf',
  'linux-arm64',
  'windows-i686',
  'windows-amd64',
  'android-arm64-v8a',
  'android-x86_64',
];

/**
 * Gets native build requirements without probing the build machine.
 * @param target - Explicit output target, independent of the host architecture.
 * @returns The compiler, dependency and linker requirements.
 */
export const getPluginTarget = (
  target: MuonPluginTarget
): MuonPluginTargetDescriptor => {
  if (!muonPluginTargets.includes(target))
    throw new Error(`Unsupported muon plugin target: ${target}`);
  const os = target.startsWith('windows-')
    ? 'windows'
    : target.startsWith('android-')
      ? 'android'
      : 'linux';
  const prefix = {
    'linux-amd64': '',
    'linux-armhf': 'arm-linux-gnueabihf-',
    'linux-arm64': 'aarch64-linux-gnu-',
    'windows-i686': 'i686-w64-mingw32-',
    'windows-amd64': 'x86_64-w64-mingw32-',
    'android-arm64-v8a': 'aarch64-linux-android24-',
    'android-x86_64': 'x86_64-linux-android24-',
  }[target];
  return {
    target,
    os,
    cc: prefix + (os === 'android' ? 'clang' : 'gcc'),
    cxx: prefix + (os === 'android' ? 'clang++' : 'g++'),
    extension: os === 'windows' ? '.dll' : '.so',
    cardioLibrary: os === 'windows' ? 'libcardio.dll' : 'libcardio.so',
    cardioLinkLibrary: os === 'windows' ? 'libcardio.dll.a' : 'libcardio.so',
    definitions: [
      'CARDIO_SHARED_LIB=1',
      'CARDIO_WITH_LINUX_IO_URING=0',
      ...(os === 'android'
        ? ['CARDIO_HAS_POSIX_FD=1']
        : [
            'CARDIO_WITH_SUPPLEMENTAL=1',
            ...(os === 'linux'
              ? [
                  'CARDIO_HAS_POSIX_FD=1',
                  'CARDIO_WITH_GLIB=1',
                  'CARDIO_WITH_GIO=1',
                ]
              : []),
          ]),
    ],
    packages: os === 'linux' ? ['glib-2.0', 'gio-2.0 >= 2.74'] : [],
    cflags: os === 'windows' ? [] : ['-fPIC', '-pthread'],
    ldflags:
      os === 'android'
        ? ['-Wl,-z,max-page-size=16384', '-landroid']
        : os === 'windows'
          ? []
          : ['-pthread'],
  };
};

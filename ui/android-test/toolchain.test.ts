// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { chmod, mkdir, mkdtemp, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { describe, expect, it } from "vitest";
import { prepareAndroid } from "../android/toolchain.js";

describe("Android toolchain diagnostics", () => {
  it("diagnoses a mismatched installed platform and an unsupported JDK", async () => {
    const sdk = await mkdtemp(join(tmpdir(), "muon-sdk-version-"));
    try {
      const platform = join(sdk, "platforms/android-37.0");
      const buildTools = join(sdk, "build-tools/36.0.0");
      await mkdir(platform, { recursive: true });
      await mkdir(buildTools, { recursive: true });
      await writeFile(join(platform, "android.jar"), "test platform");
      await writeFile(
        join(platform, "source.properties"),
        "AndroidVersion.ApiLevel=36\n",
      );
      await writeFile(
        join(buildTools, "source.properties"),
        "Pkg.Revision=36.0.0\n",
      );
      for (const name of ["aapt2", "zipalign", "apksigner"])
        await writeFile(join(buildTools, name), "test tool");
      const options = {
        componentsDirectory: resolve("../core/android"),
        sdkPath: sdk,
        environment: process.env,
        prepareGradle: false,
      };
      await expect(prepareAndroid(options)).rejects.toThrow(
        /SDK platform version/,
      );
      await writeFile(
        join(platform, "source.properties"),
        "AndroidVersion.ApiLevel=37.0\n",
      );
      const jdk = join(sdk, "jdk");
      await mkdir(join(jdk, "bin"), { recursive: true });
      await writeFile(
        join(jdk, "bin/java"),
        "#!/bin/sh\necho 'openjdk version \"11.0.1\"' >&2\n",
      );
      await chmod(join(jdk, "bin/java"), 0o755);
      await expect(
        prepareAndroid({
          ...options,
          environment: { ...process.env, JAVA_HOME: jdk },
        }),
      ).rejects.toThrow(/JDK 17.*25 is required/);
    } finally {
      await rm(sdk, { recursive: true, force: true });
    }
  });
  it("reports the SDK path and required package when the SDK is empty", async () => {
    const sdk = await mkdtemp(join(tmpdir(), "muon-empty-sdk-"));
    try {
      await expect(
        prepareAndroid({
          componentsDirectory: resolve("../core/android"),
          sdkPath: sdk,
          environment: process.env,
          prepareGradle: false,
        }),
      ).rejects.toThrow(/platforms;android-37/);
    } finally {
      await rm(sdk, { recursive: true, force: true });
    }
  });
  it("explains how to configure a missing SDK without downloading it", async () => {
    await expect(
      prepareAndroid({
        componentsDirectory: resolve("../core/android"),
        sdkPath: undefined,
        environment: { PATH: "" },
        prepareGradle: false,
      }),
    ).rejects.toThrow(/ANDROID_HOME/);
  });
});

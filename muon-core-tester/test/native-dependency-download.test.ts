// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFile } from "node:child_process";
import {
  chmod,
  copyFile,
  mkdtemp,
  mkdir,
  readFile,
  rm,
  writeFile,
} from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { promisify } from "node:util";

import { describe, expect, it } from "vitest";

const execFileAsync = promisify(execFile);

const zlibSha256 =
  "bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16";

describe("native dependency downloads", () => {
  it("accepts only verified zlib downloads and cleans up after all sources fail", async () => {
    const temporaryDirectory = await mkdtemp(
      join(tmpdir(), "muon-zlib-download-"),
    );
    try {
      const fakeBinDirectory = join(temporaryDirectory, "bin");
      const scriptPath = join(temporaryDirectory, "build_zlib.sh");
      const requestLogPath = join(temporaryDirectory, "requests.log");
      const invalidArchivePath = join(temporaryDirectory, "invalid.tar.gz");
      const validArchivePath = join(temporaryDirectory, "valid.tar.gz");
      const fixtureDirectory = join(temporaryDirectory, "fixture");
      const sourceDirectory = join(fixtureDirectory, "zlib-1.3.2");
      const primaryUrl =
        "https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.gz";
      const fallbackUrl = "https://zlib.net/fossils/zlib-1.3.2.tar.gz";

      await mkdir(fakeBinDirectory);
      await mkdir(sourceDirectory, { recursive: true });
      await copyFile(
        new URL("../../muon-builder/build_zlib.sh", import.meta.url),
        scriptPath,
      );
      await writeFile(invalidArchivePath, "invalid response\n");
      await writeFile(
        join(sourceDirectory, "CMakeLists.txt"),
        "project(zlib)\n",
      );
      await writeFile(join(sourceDirectory, "LICENSE"), "zlib license\n");
      await writeFile(join(sourceDirectory, "zlib.h"), "#pragma once\n");
      await execFileAsync(
        "tar",
        ["-czf", validArchivePath, "-C", fixtureDirectory, "zlib-1.3.2"],
        { timeout: 10_000 },
      );

      const fakeCurlPath = join(fakeBinDirectory, "curl");
      await writeFile(
        fakeCurlPath,
        `#!/usr/bin/env bash
set -euo pipefail
output_path=""
url=""
while [[ "$#" -gt 0 ]]; do
  case "$1" in
    -o)
      output_path="$2"
      shift 2
      ;;
    *)
      url="$1"
      shift
      ;;
  esac
done
printf '%s\\n' "\${url}" >> "\${FAKE_CURL_REQUEST_LOG}"
if [[ "\${url}" == "\${FAKE_CURL_FALLBACK_URL}" ]]; then
  cp "\${FAKE_CURL_VALID_ARCHIVE}" "\${output_path}"
else
  cp "\${FAKE_CURL_INVALID_ARCHIVE}" "\${output_path}"
fi
`,
      );
      await chmod(fakeCurlPath, 0o755);

      const fakeSha256Path = join(fakeBinDirectory, "sha256sum");
      await writeFile(
        fakeSha256Path,
        `#!/usr/bin/env bash
set -euo pipefail
if cmp -s "$1" "\${FAKE_CURL_VALID_ARCHIVE}"; then
  printf '%s  %s\\n' "\${FAKE_ZLIB_SHA256}" "$1"
else
  printf '%064d  %s\\n' 0 "$1"
fi
`,
      );
      await chmod(fakeSha256Path, 0o755);

      const compilerPaths = ["gcc", "ar", "ranlib"].map((tool) =>
        join(fakeBinDirectory, tool),
      );
      for (const compilerPath of compilerPaths) {
        await writeFile(compilerPath, "#!/usr/bin/env bash\nexit 99\n");
        await chmod(compilerPath, 0o755);
      }

      const buildDirectory = join(
        temporaryDirectory,
        ".deps",
        "build",
        "zlib-linux64",
      );
      const installDirectory = join(buildDirectory, "install");
      await mkdir(join(installDirectory, "include"), { recursive: true });
      await mkdir(join(installDirectory, "lib"), { recursive: true });
      await writeFile(join(installDirectory, "include", "zlib.h"), "");
      await writeFile(join(installDirectory, "include", "zconf.h"), "");
      await writeFile(join(installDirectory, "lib", "libz.a"), "");
      await writeFile(
        join(buildDirectory, ".built-recipe"),
        `zlib-1.3.2|cmake-static-install-v1|${compilerPaths.join("|")}|\n`,
      );

      const runDownload = (acceptedUrl: string) =>
        execFileAsync("bash", [scriptPath, "linux64", "gcc", "ar", "ranlib"], {
          env: {
            ...process.env,
            CFLAGS: "",
            FAKE_CURL_FALLBACK_URL: acceptedUrl,
            FAKE_CURL_INVALID_ARCHIVE: invalidArchivePath,
            FAKE_CURL_REQUEST_LOG: requestLogPath,
            FAKE_CURL_VALID_ARCHIVE: validArchivePath,
            FAKE_ZLIB_SHA256: zlibSha256,
            PATH: `${fakeBinDirectory}:${process.env.PATH ?? ""}`,
          },
          timeout: 10_000,
        });

      await runDownload(fallbackUrl);

      expect(
        (await readFile(requestLogPath, "utf8")).trim().split("\n"),
      ).toEqual([primaryUrl, fallbackUrl]);
      expect(
        await readFile(join(temporaryDirectory, ".deps", "zlib-1.3.2.tar.gz")),
      ).toEqual(await readFile(validArchivePath));

      await rm(join(temporaryDirectory, ".deps"), {
        force: true,
        recursive: true,
      });
      await expect(runDownload("no-accepted-source")).rejects.toMatchObject({
        code: 1,
      });
      expect(
        (await readFile(requestLogPath, "utf8")).trim().split("\n"),
      ).toEqual([primaryUrl, fallbackUrl, primaryUrl, fallbackUrl]);
      await expect(
        readFile(join(temporaryDirectory, ".deps", "zlib-1.3.2.tar.gz")),
      ).rejects.toMatchObject({ code: "ENOENT" });
      await expect(
        readFile(
          join(temporaryDirectory, ".deps", ".locks", "zlib-1.3.2.lock"),
        ),
      ).rejects.toMatchObject({ code: "ENOENT" });
    } finally {
      await rm(temporaryDirectory, { force: true, recursive: true });
    }
  });
});

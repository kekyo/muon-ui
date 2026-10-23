// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

// Run after staging distribution targets and npm pack. The consumer has no
// repository symlinks and uses only the installed package's public CLI.
import assert from "node:assert/strict";
import { execFile } from "node:child_process";
import {
  access,
  mkdir,
  mkdtemp,
  readFile,
  rm,
  writeFile,
} from "node:fs/promises";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { promisify } from "node:util";

const execute = promisify(execFile);
const [archive, ...requestedTargets] = process.argv.slice(2);
assert(
  archive,
  "Usage: node test-packaged-appimage.mts <package.tgz> [linux targets...]",
);
const targets =
  requestedTargets.length > 0 ? requestedTargets : ["linux-amd64"];
const machines: Record<string, number> = {
  "linux-amd64": 62,
  "linux-armhf": 40,
  "linux-arm64": 183,
};
for (const target of targets)
  assert(target in machines, `Invalid target: ${target}`);
const consumer = await mkdtemp(join(tmpdir(), "muon-npm-appimage-"));
try {
  await writeFile(
    join(consumer, "package.json"),
    JSON.stringify({
      name: "appimage-consumer",
      version: "1.0.0",
      private: true,
    }),
  );
  await mkdir(join(consumer, "assets/main"), { recursive: true });
  await writeFile(
    join(consumer, "assets/main/index.html"),
    "<!doctype html><title>NPM consumer</title><h1>NPM consumer</h1>",
  );
  await writeFile(
    join(consumer, "muon.json"),
    JSON.stringify({ launcher: { defaultVersionPolicy: "compat-latest" } }),
  );
  await execute(
    "npm",
    ["install", "--no-audit", "--no-fund", resolve(archive)],
    { cwd: consumer, maxBuffer: 8 * 1024 * 1024 },
  );
  const packageDirectory = join(consumer, "node_modules/muon-ui");
  for (const target of targets) {
    const { stdout } = await execute(
      process.execPath,
      [
        join(packageDirectory, "dist/cli.cjs"),
        "pack",
        "--target",
        target,
        "--type",
        "appimage,deb,tar.gz",
        "--json",
      ],
      { cwd: consumer, maxBuffer: 8 * 1024 * 1024 },
    );
    const result = JSON.parse(stdout) as {
      artifacts: { type: string; target: string; path: string }[];
    };
    assert.deepEqual(result.artifacts.map((artifact) => artifact.type).sort(), [
      "appimage",
      "deb",
      "tar.gz",
    ]);
    const appimage = result.artifacts.find(
      (artifact) => artifact.type === "appimage",
    )!;
    const executable = await readFile(appimage.path);
    assert.equal(executable.subarray(0, 4).toString("hex"), "7f454c46");
    assert.equal(executable.subarray(8, 11).toString("hex"), "414902");
    assert.equal(executable.readUInt16LE(18), machines[target]);
    const payload = join(
      consumer,
      ".muon/pack/appimage",
      `appimage-consumer-${target}.AppDir/usr/lib/muon`,
    );
    assert.deepEqual(
      JSON.parse(await readFile(join(payload, "muon-install.json"), "utf8")),
      { type: "appimage", runtimeMode: "user" },
    );
    const reference = await readFile(
      join(packageDirectory, "dist/runtime", target, "linux-dependencies.json"),
    );
    assert.deepEqual(
      await readFile(join(payload, "linux-dependencies.json")),
      reference,
    );
    await assert.rejects(access(join(payload, "libcef.so")));
    const deb = result.artifacts.find((artifact) => artifact.type === "deb")!;
    const depends = await execute("dpkg-deb", ["--field", deb.path, "Depends"]);
    assert.match(depends.stdout, /libc6 \(>= /);
    assert.match(depends.stdout, /libgtk-3-0 \(>= /);
    assert.match(depends.stdout, /libglib2.0-0 \(>= 2\.7[34]/);
    const portable = result.artifacts.find(
      (artifact) => artifact.type === "tar.gz",
    )!;
    const listing = await execute("tar", ["-tzf", portable.path]);
    assert.match(listing.stdout, /muon-install\.json/);
    assert(!listing.stdout.includes("libcef.so"));
    console.log(
      JSON.stringify({
        target,
        formats: result.artifacts.map((artifact) => artifact.type),
        appImageBytes: executable.length,
        depends: depends.stdout.trim(),
      }),
    );
  }
} finally {
  await rm(consumer, { recursive: true, force: true });
}

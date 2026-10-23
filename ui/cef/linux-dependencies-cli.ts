// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { mkdir, readFile, writeFile } from "node:fs/promises";
import { dirname, join, resolve } from "node:path";

import { Command } from "commander";
import { runScriptOnceToText, type FunCityLogEntry } from "funcity";

import {
  createLinuxDependencyManifest,
  readLinuxReferenceCef,
} from "./linux-dependency-manifest.js";
import { type LinuxDependencyTarget } from "./linux-dependencies.js";

interface DependencyCommandOptions {
  target: string;
  cmakeCache: string;
  cef: string | undefined;
  runtime: string;
  native: string | undefined;
  libraryPath: string[];
  output: string;
}

const command = new Command()
  .name("muon-linux-dependencies")
  .description(
    "Audit the build reference CEF and muon against a Linux baseline",
  )
  .requiredOption(
    "--target <target>",
    "linux-amd64, linux-armhf or linux-arm64",
  )
  .requiredOption(
    "--cmake-cache <file>",
    "CMakeCache.txt of the native muon build",
  )
  .option(
    "--cef <directory>",
    "Release directory of the build reference CEF (default: native CMake cache)",
  )
  .requiredOption("--runtime <directory>", "muon runtime payload directory")
  .option(
    "--native <directory>",
    "additional launcher and builder payload directory",
  )
  .option(
    "--library-path <directories...>",
    "target system library directories",
    [],
  )
  .requiredOption(
    "--output <file>",
    "output JSON manifest; also writes a .txt report",
  );

try {
  await command.parseAsync(process.argv);
  const options = command.opts<DependencyCommandOptions>();
  if (!["linux-amd64", "linux-armhf", "linux-arm64"].includes(options.target)) {
    throw new Error(`Unsupported Linux dependency target: ${options.target}`);
  }
  const target = options.target as LinuxDependencyTarget;
  const cefRoot = (await readFile(options.cmakeCache, "utf8")).match(
    /^CEF_ROOT:[^=]+=(.+)$/m,
  )?.[1];
  const cefDirectory = options.cef ?? (cefRoot && join(cefRoot, "Release"));
  if (!cefDirectory) throw new Error("Missing CEF_ROOT in native build cache.");
  const triplet = {
    "linux-amd64": "x86_64-linux-gnu",
    "linux-armhf": "arm-linux-gnueabihf",
    "linux-arm64": "aarch64-linux-gnu",
  }[target];
  const libraryPaths = options.libraryPath.length
    ? options.libraryPath
    : [`/usr/lib/${triplet}`, `/lib/${triplet}`, "/usr/lib", "/lib"];
  const manifest = await createLinuxDependencyManifest({
    target,
    referenceCef: await readLinuxReferenceCef(options.cmakeCache, target),
    cefDirectory,
    runtimeDirectory: options.runtime,
    nativeDirectory: options.native,
    libraryPaths,
  });
  const output = resolve(options.output);
  await mkdir(dirname(output), { recursive: true });
  await writeFile(output, `${JSON.stringify(manifest, null, 2)}\n`);
  const roots = new Set(manifest.audit.roots);
  const requirements = manifest.audit.binaries
    .filter((binary) => roots.has(binary.path))
    .map((binary) => {
      const versions = new Map<string, string>();
      for (const requirement of binary.versionNeeds) {
        if (requirement.weak) continue;
        const version = requirement.version.match(
          /^([A-Z][A-Z_]*?)_([0-9.]+)$/,
        );
        if (!version) continue;
        const previous = versions.get(version[1]!);
        if (
          !previous ||
          previous.localeCompare(version[2]!, "en", { numeric: true }) < 0
        )
          versions.set(version[1]!, version[2]!);
      }
      return `${binary.path}: ${[...versions].map(([name, version]) => `${name}_${version}`).join(", ") || "no versioned requirements"}`;
    })
    .join("\n");
  const logs: FunCityLogEntry[] = [];
  const report = await runScriptOnceToText(
    "Target: {{target}}\nBuild reference CEF: {{cef}}\nELF files in closure: {{count}}\nUnresolved requirements: {{issues}}\n\n{{requirements}}\n\nDebian Depends:\n{{depends}}\n\nDiagnostics:\n{{diagnostics}}\n",
    {
      sourceId: "muon-linux-dependency-report",
      logs,
      variables: new Map<string, unknown>([
        ["target", target],
        ["cef", manifest.referenceCef.version],
        ["count", manifest.audit.binaries.length],
        ["issues", manifest.audit.issues.length],
        ["requirements", requirements],
        [
          "depends",
          manifest.debian
            ? [...manifest.debian.depends, ...manifest.runtimePackages]
                .sort()
                .join("\n")
            : "Unavailable: ABI audit failed",
        ],
        [
          "diagnostics",
          [
            ...manifest.audit.issues.map(
              (issue) =>
                `${issue.binary}: missing ${issue.kind} ${issue.requirement}`,
            ),
            ...(manifest.debian?.diagnostics ?? []),
          ].join("\n") || "none",
        ],
      ]),
    },
  );
  if (report === undefined || logs.some((entry) => entry.type === "error"))
    throw new Error(`Cannot render dependency report: ${JSON.stringify(logs)}`);
  await writeFile(`${output}.txt`, report);
  process.stdout.write(report);
  if (manifest.audit.issues.length) process.exitCode = 1;
} catch (error) {
  process.stderr.write(
    `${error instanceof Error ? error.message : String(error)}\n`,
  );
  process.exitCode = 1;
}

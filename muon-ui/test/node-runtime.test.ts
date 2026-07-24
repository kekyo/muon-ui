// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import {
  chmod,
  mkdir,
  mkdtemp,
  readFile,
  rm,
  writeFile,
} from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";

import { afterEach, describe, expect, it } from "vitest";

import { resolveMuonNodeProject } from "../src/node-project.js";
import { runMuonPrepare } from "../src/prepare.js";

const cleanupDirectories: string[] = [];
const bridgeEngineComparatorSets = [
  [">=20.19.0", "<21.0.0-0"],
  [">=22.12.0"],
] as const;

interface ResolvedNodeRuntimeProject {
  sourcePath: string;
  engineRange: string;
  comparatorSets: readonly (readonly string[])[];
}

interface NodeRuntimeRequirement {
  required: boolean;
  engineRange: string;
  comparatorSets: readonly (readonly string[])[];
}

const createTemporaryDirectory = async (prefix: string): Promise<string> => {
  const directory = await mkdtemp(join(tmpdir(), prefix));
  cleanupDirectories.push(directory);
  return directory;
};

const createNodeProject = async (
  packageJson: string,
): Promise<{
  root: string;
  projectPath: string;
  config: Readonly<Record<string, unknown>>;
}> => {
  const root = await createTemporaryDirectory("muon-node-runtime-");
  const projectPath = join(root, "backend");
  await mkdir(projectPath, { recursive: true });
  await writeFile(join(projectPath, "package.json"), packageJson);
  return {
    root,
    projectPath,
    config: {
      node: {
        project: "./backend",
      },
    },
  };
};

const resolveNodeRuntimeProject = async (
  packageJson: string,
): Promise<ResolvedNodeRuntimeProject> => {
  const fixture = await createNodeProject(packageJson);
  const project = await resolveMuonNodeProject(fixture.config, fixture.root);
  expect(project).toBeDefined();
  return project as ResolvedNodeRuntimeProject;
};

afterEach(async () => {
  for (const directory of cleanupDirectories.splice(0)) {
    await rm(directory, { recursive: true, force: true });
  }
});

describe("muon Node runtime requirement", () => {
  it("uses the bridge-supported range when package.json omits engines.node", async () => {
    const project = await resolveNodeRuntimeProject(
      `${JSON.stringify({ name: "no-engine-project" }, null, 2)}\n`,
    );

    expect(project.engineRange).toBe("*");
    expect(project.comparatorSets).toEqual(bridgeEngineComparatorSets);
  });

  it("normalizes engines.node and intersects it with the bridge-supported range", async () => {
    const project = await resolveNodeRuntimeProject(
      `${JSON.stringify(
        {
          name: "engine-project",
          engines: {
            node: ">=20 <23 || ^24.3.0",
          },
        },
        null,
        2,
      )}\n`,
    );

    expect(project.engineRange).toBe(">=20 <23 || ^24.3.0");
    expect(project.comparatorSets).toEqual([
      [">=20.19.0", "<21.0.0-0", ">=20.0.0", "<23.0.0-0"],
      [">=22.12.0", ">=20.0.0", "<23.0.0-0"],
      [">=22.12.0", ">=24.3.0", "<25.0.0-0"],
    ]);
  });

  it("rejects an invalid Node project package.json", async () => {
    await expect(resolveNodeRuntimeProject("{ invalid json\n")).rejects.toThrow(
      "muon Node project package.json could not be parsed",
    );
  });

  it.each([null, [], "node", 20])(
    "rejects a non-object package.json engines value: %j",
    async (engines) => {
      await expect(
        resolveNodeRuntimeProject(
          `${JSON.stringify({ name: "invalid-engines", engines })}\n`,
        ),
      ).rejects.toThrow("package.json engines must be an object");
    },
  );

  it.each([
    [null, "must be a non-empty string"],
    [20, "must be a non-empty string"],
    ["", "must be a non-empty string"],
    ["not a range", "is not a valid range"],
  ])(
    "rejects an invalid package.json engines.node value: %j",
    async (node, message) => {
      await expect(
        resolveNodeRuntimeProject(
          `${JSON.stringify({
            name: "invalid-node-engine",
            engines: { node },
          })}\n`,
        ),
      ).rejects.toThrow(message);
    },
  );

  it("rejects engines.node when it does not overlap the bridge-supported range", async () => {
    await expect(
      resolveNodeRuntimeProject(
        `${JSON.stringify({
          name: "unsupported-node-engine",
          engines: { node: "<20" },
        })}\n`,
      ),
    ).rejects.toThrow(
      "package.json engines.node does not overlap the muon Node bridge range",
    );
  });

  it("passes the normalized Node runtime requirement as one native prepare argument", async () => {
    const root = await createTemporaryDirectory("muon-node-prepare-");
    const executablePath = join(root, "fake-muon-builder.mjs");
    const argumentLogPath = join(root, "arguments.json");
    await writeFile(
      executablePath,
      [
        "#!/usr/bin/env node",
        'import { writeFileSync } from "node:fs";',
        "const logPath = process.env.MUON_TEST_ARGUMENT_LOG;",
        'if (logPath === undefined) throw new Error("missing argument log");',
        "writeFileSync(logPath, JSON.stringify(process.argv.slice(2)));",
        'process.stdout.write(JSON.stringify({ muonPath: "/runtime", cefPath: "/cache/cef.tar.bz2", cacheHit: false }));',
        "",
      ].join("\n"),
    );
    await chmod(executablePath, 0o755);
    const requirement: NodeRuntimeRequirement = {
      required: true,
      engineRange: ">=20 <23",
      comparatorSets: [
        [">=20.19.0", "<21.0.0-0", ">=20.0.0", "<23.0.0-0"],
        [">=22.12.0", ">=20.0.0", "<23.0.0-0"],
      ],
    };
    const options = {
      muonPath: "/source-runtime",
      cefPath: undefined,
      stageDir: "/staged-runtime",
      target: "linux-amd64",
      cacheDir: "/runtime-cache",
      force: false,
      quiet: true,
      prepareExecutablePath: executablePath,
      environment: {
        ...process.env,
        MUON_TEST_ARGUMENT_LOG: argumentLogPath,
      },
      cwd: root,
      nodeRuntimeRequirement: requirement,
    } as Parameters<typeof runMuonPrepare>[0] & {
      nodeRuntimeRequirement: NodeRuntimeRequirement;
    };

    await runMuonPrepare(options);

    const arguments_ = JSON.parse(
      await readFile(argumentLogPath, "utf8"),
    ) as string[];
    const requirementIndex = arguments_.indexOf("--node-runtime-requirement");
    expect(requirementIndex).toBeGreaterThanOrEqual(0);
    expect(JSON.parse(arguments_[requirementIndex + 1] ?? "")).toEqual(
      requirement,
    );
  });
});

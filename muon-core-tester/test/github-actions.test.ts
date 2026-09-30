// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { readFile } from "node:fs/promises";
import { fileURLToPath } from "node:url";

import { describe, expect, it } from "vitest";
import { parse } from "yaml";

type Mapping = Record<string, unknown>;

const workflowPath = fileURLToPath(
  new URL("../../.github/workflows/ci.yml", import.meta.url),
);

const requireMapping = (value: unknown, description: string): Mapping => {
  if (typeof value !== "object" || value === null || Array.isArray(value)) {
    throw new Error(`${description} must be a mapping.`);
  }
  return value as Mapping;
};

const requireSequence = (value: unknown, description: string): unknown[] => {
  if (!Array.isArray(value)) {
    throw new Error(`${description} must be a sequence.`);
  }
  return value;
};

const requireString = (value: unknown, description: string): string => {
  if (typeof value !== "string") {
    throw new Error(`${description} must be a string.`);
  }
  return value;
};

const findStep = (steps: unknown[], name: string): Mapping => {
  const step = steps
    .map((value, index) => requireMapping(value, `Step ${index + 1}`))
    .find((value) => value.name === name);
  if (step === undefined) {
    throw new Error(`Workflow step was not found: ${name}`);
  }
  return step;
};

describe("GitHub Actions CI", () => {
  it("builds and tests on Ubuntu, analyzes all project languages, and does not publish", async () => {
    const workflow = requireMapping(
      parse(await readFile(workflowPath, "utf8")),
      "Workflow",
    );
    const triggers = requireMapping(workflow.on, "Workflow triggers");
    expect(Object.keys(triggers).sort()).toEqual([
      "pull_request",
      "push",
      "workflow_dispatch",
    ]);
    expect(
      requireSequence(
        requireMapping(triggers.push, "Push trigger").branches,
        "Push branches",
      ).sort(),
    ).toEqual(["develop", "main"]);
    expect(workflow.permissions).toEqual({ contents: "read" });

    const jobs = requireMapping(workflow.jobs, "Workflow jobs");
    expect(Object.keys(jobs).sort()).toEqual(["build-test", "codeql"]);
    for (const [jobName, value] of Object.entries(jobs)) {
      const job = requireMapping(value, `Job ${jobName}`);
      expect(job["runs-on"]).toBe("ubuntu-latest");
    }

    const buildTestJob = requireMapping(jobs["build-test"], "Build/test job");
    expect(buildTestJob["timeout-minutes"]).toBe(45);
    expect(buildTestJob.permissions).toEqual({ contents: "read" });
    const buildTestSteps = requireSequence(
      buildTestJob.steps,
      "Build/test steps",
    );
    const checkout = findStep(buildTestSteps, "Checkout repository");
    expect(checkout.with).toEqual({
      "persist-credentials": false,
      submodules: "recursive",
    });

    const setupNode = findStep(buildTestSteps, "Set up Node.js");
    expect(setupNode.with).toEqual({ cache: "npm", "node-version": 24 });

    const dependencies = requireString(
      findStep(buildTestSteps, "Install build dependencies").run,
      "Dependency installation command",
    );
    for (const packageName of [
      "g++-mingw-w64",
      "libgtk-3-dev",
      "ninja-build",
      "wine32:i386",
      "wine64",
      "xvfb",
    ]) {
      expect(dependencies).toContain(packageName);
    }

    expect(findStep(buildTestSteps, "Install npm dependencies").run).toBe(
      "npm ci",
    );
    expect(findStep(buildTestSteps, "Build and test").run).toBe("npm run test");

    const cefCache = findStep(buildTestSteps, "Cache CEF");
    expect(cefCache.uses).toBe(
      "actions/cache@55cc8345863c7cc4c66a329aec7e433d2d1c52a9",
    );
    const cefCacheConfiguration = requireMapping(
      cefCache.with,
      "CEF cache configuration",
    );
    expect(requireString(cefCacheConfiguration.path, "CEF cache paths")).toBe(
      [
        "~/.cache/muon/cef-catalog.json",
        "~/.cache/muon/artifacts/cef",
        "muon-core/.cef",
      ].join("\n"),
    );
    const cefCacheKey = requireString(
      cefCacheConfiguration.key,
      "CEF cache key",
    );
    expect(cefCacheKey).toContain("hashFiles(");
    expect(cefCacheKey).not.toContain("github.sha");

    const nativeCache = findStep(buildTestSteps, "Cache native dependencies");
    expect(nativeCache.uses).toBe(
      "actions/cache@55cc8345863c7cc4c66a329aec7e433d2d1c52a9",
    );
    const nativeCacheConfiguration = requireMapping(
      nativeCache.with,
      "Native cache configuration",
    );
    expect(
      requireString(nativeCacheConfiguration.path, "Native cache paths"),
    ).toBe(
      [
        "muon-builder/.deps",
        "muon-core/.deps/*.tar.gz",
        "muon-core/.deps/src",
      ].join("\n"),
    );
    const nativeCacheKey = requireString(
      nativeCacheConfiguration.key,
      "Native cache key",
    );
    expect(nativeCacheKey).toContain("steps.native-toolchain.outputs");
    expect(nativeCacheKey).toContain("hashFiles(");
    expect(nativeCacheKey).toContain("muon-builder/build.sh");
    expect(nativeCacheKey).not.toContain("github.sha");

    const nativeToolchainCommand = requireString(
      findStep(buildTestSteps, "Identify native toolchain").run,
      "Native toolchain identification command",
    );
    for (const tool of [
      "gcc",
      "ar",
      "ranlib",
      "i686-w64-mingw32-gcc",
      "i686-w64-mingw32-ar",
      "i686-w64-mingw32-ranlib",
      "x86_64-w64-mingw32-gcc",
      "x86_64-w64-mingw32-ar",
      "x86_64-w64-mingw32-ranlib",
    ]) {
      expect(nativeToolchainCommand).toContain(tool);
    }
    expect(nativeToolchainCommand).toContain("command -v");
    expect(nativeToolchainCommand).toContain("--version");

    expect(
      buildTestSteps.some(
        (value, index) =>
          requireMapping(value, `Build/test step ${index + 1}`).uses !==
            undefined &&
          requireString(
            requireMapping(value, `Build/test step ${index + 1}`).uses,
            `Build/test action ${index + 1}`,
          ).startsWith("github/codeql-action/"),
      ),
    ).toBe(false);

    const codeqlJob = requireMapping(jobs.codeql, "CodeQL job");
    expect(codeqlJob["timeout-minutes"]).toBe(30);
    expect(codeqlJob.permissions).toEqual({
      contents: "read",
      "security-events": "write",
    });
    const strategy = requireMapping(codeqlJob.strategy, "CodeQL strategy");
    const matrix = requireMapping(strategy.matrix, "CodeQL matrix");
    expect(requireSequence(matrix.language, "CodeQL languages").sort()).toEqual(
      ["actions", "c-cpp", "javascript-typescript"],
    );
    const codeqlSteps = requireSequence(codeqlJob.steps, "CodeQL steps");
    expect(findStep(codeqlSteps, "Checkout repository").with).toEqual({
      "persist-credentials": false,
      submodules: "recursive",
    });
    const initializeCodeql = findStep(codeqlSteps, "Initialize CodeQL");
    expect(initializeCodeql.with).toEqual({
      "build-mode": "none",
      languages: "${{ matrix.language }}",
    });
    const analyzeCodeql = findStep(codeqlSteps, "Analyze");
    expect(analyzeCodeql.uses).toBe(
      "github/codeql-action/analyze@5595ccaf912efad79be6eef63a5619ff05969be3",
    );
    expect(analyzeCodeql.with).toEqual({
      category: "/language:${{ matrix.language }}",
    });
    expect(codeqlSteps.indexOf(initializeCodeql)).toBeLessThan(
      codeqlSteps.indexOf(analyzeCodeql),
    );

    const allSteps = [...buildTestSteps, ...codeqlSteps].map((value, index) =>
      requireMapping(value, `Combined step ${index + 1}`),
    );
    const externalActions = allSteps
      .map((step) => step.uses)
      .filter((value): value is string => typeof value === "string")
      .filter((value) => !value.startsWith("./"));
    expect(externalActions.length).toBeGreaterThan(0);
    const allowedActions = [
      "actions/cache",
      "actions/checkout",
      "actions/setup-node",
      "github/codeql-action/analyze",
      "github/codeql-action/init",
    ];
    for (const action of externalActions) {
      expect(action).toMatch(/^[^@\s]+@[0-9a-f]{40}$/);
      expect(allowedActions).toContain(action.split("@", 1)[0]);
    }

    for (const [index, value] of codeqlSteps.entries()) {
      expect(
        requireMapping(value, `CodeQL step ${index + 1}`).run,
      ).toBeUndefined();
    }

    const runCommands = allSteps
      .map((step) => step.run)
      .filter((value): value is string => typeof value === "string")
      .join("\n");
    expect(runCommands).not.toMatch(
      /(?:build_package|build:dist|npm\s+run\s+pack|npm\s+publish)/,
    );
  });
});

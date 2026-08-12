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
    expect(workflow.permissions).toEqual({ contents: "read" });

    const jobs = requireMapping(workflow.jobs, "Workflow jobs");
    expect(Object.keys(jobs).sort()).toEqual(["build-test", "codeql"]);
    for (const [jobName, value] of Object.entries(jobs)) {
      const job = requireMapping(value, `Job ${jobName}`);
      expect(job["runs-on"]).toBe("ubuntu-latest");
      expect(job.permissions).toEqual({
        contents: "read",
        "security-events": "write",
      });
    }

    const buildTestJob = requireMapping(jobs["build-test"], "Build/test job");
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
    expect(findStep(buildTestSteps, "Build").run).toBe("npm run build");
    expect(findStep(buildTestSteps, "Test").run).toBe("npm run test");

    const cppInitialization = findStep(
      buildTestSteps,
      "Initialize CodeQL for C and C++",
    );
    expect(cppInitialization.with).toEqual({
      "build-mode": "manual",
      languages: "c-cpp",
    });
    const buildStepIndex = buildTestSteps.indexOf(
      findStep(buildTestSteps, "Build"),
    );
    expect(buildTestSteps.indexOf(cppInitialization)).toBeLessThan(
      buildStepIndex,
    );
    expect(buildStepIndex).toBeLessThan(
      buildTestSteps.indexOf(findStep(buildTestSteps, "Analyze C and C++")),
    );

    const codeqlJob = requireMapping(jobs.codeql, "CodeQL job");
    const strategy = requireMapping(codeqlJob.strategy, "CodeQL strategy");
    const matrix = requireMapping(strategy.matrix, "CodeQL matrix");
    expect(requireSequence(matrix.language, "CodeQL languages").sort()).toEqual(
      ["actions", "javascript-typescript"],
    );
    const codeqlSteps = requireSequence(codeqlJob.steps, "CodeQL steps");
    expect(findStep(codeqlSteps, "Initialize CodeQL").with).toEqual({
      "build-mode": "none",
      languages: "${{ matrix.language }}",
    });

    const allSteps = [...buildTestSteps, ...codeqlSteps].map((value, index) =>
      requireMapping(value, `Combined step ${index + 1}`),
    );
    const externalActions = allSteps
      .map((step) => step.uses)
      .filter((value): value is string => typeof value === "string")
      .filter((value) => !value.startsWith("./"));
    expect(externalActions.length).toBeGreaterThan(0);
    for (const action of externalActions) {
      expect(action).toMatch(/^[^@\s]+@[0-9a-f]{40}$/);
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

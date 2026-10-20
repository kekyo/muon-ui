// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFile } from "node:child_process";
import { randomUUID } from "node:crypto";
import { mkdtemp, readFile, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { fileURLToPath } from "node:url";
import { promisify } from "node:util";
import { expect, it } from "vitest";

const execFileAsync = promisify(execFile);

it.each([
  "windows-path.test.ts",
  "windows-builder-path.test.ts",
  "notifications-windows.test.ts",
])("skips %s before preparation without a configured machine", async (file) => {
  const directory = await mkdtemp(join(tmpdir(), "muon-unconfigured-e2e-"));
  const reportPath = join(directory, "report.json");
  try {
    let stderr = "";
    try {
      const result = await execFileAsync(
        process.execPath,
        [
          fileURLToPath(
            new URL("../../node_modules/vitest/vitest.mjs", import.meta.url),
          ),
          "run",
          file,
          "--config",
          "fixtures/windows-e2e-unconfigured.config.ts",
          "--reporter=json",
          `--outputFile=${reportPath}`,
        ],
        {
          cwd: fileURLToPath(new URL(".", import.meta.url)),
          env: {
            ...process.env,
            MUON_E2E_AGENT_SECTION: `unconfigured-${randomUUID()}`,
          },
          maxBuffer: 4 * 1024 * 1024,
        },
      );
      stderr = result.stderr;
    } catch (error) {
      let report = "";
      try {
        report = await readFile(reportPath, "utf8");
      } catch {
        // Runner startup failures may occur before a report is created.
      }
      throw new Error(`${String(error)}\n${report}`, { cause: error });
    }
    const report = JSON.parse(await readFile(reportPath, "utf8"));
    expect(report.success).toBe(true);
    expect(report.numTotalTests).toBeGreaterThan(0);
    expect(report.numPendingTests).toBe(report.numTotalTests);
    expect(report.numFailedTests).toBe(0);
    expect(stderr).toContain("Skipping");
    expect(stderr).toContain("was not found");
  } finally {
    await rm(directory, { recursive: true, force: true });
  }
});

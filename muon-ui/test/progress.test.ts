// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

import { afterEach, describe, expect, it, vi } from "vitest";

import { createMuonProgressRenderer } from "../src/progress.js";

const wait = async (milliseconds: number): Promise<void> => {
  await new Promise<void>((resolve) => {
    setTimeout(resolve, milliseconds);
  });
};

const captureStderr = (): {
  chunks: string[];
  restore(): void;
} => {
  const chunks: string[] = [];
  const write = vi.spyOn(process.stderr, "write").mockImplementation(((
    chunk: string | Uint8Array,
  ) => {
    chunks.push(
      typeof chunk === "string" ? chunk : Buffer.from(chunk).toString("utf8"),
    );
    return true;
  }) as typeof process.stderr.write);
  return {
    chunks,
    restore: (): void => {
      write.mockRestore();
    },
  };
};

const getCompletedProgressLines = (output: string): string[] =>
  output
    .replace(/\x1b\[K/gu, "")
    .split("\n")
    .filter((line) => line.length > 0)
    .map((line) => {
      const segments = line.split("\r").filter((segment) => segment.length > 0);
      return segments.at(-1) ?? "";
    });

describe("createMuonProgressRenderer", () => {
  afterEach(() => {
    vi.restoreAllMocks();
  });

  it("throttles installing file updates on one spinner line", async () => {
    const stderr = captureStderr();
    try {
      const renderer = createMuonProgressRenderer();
      for (let count = 1; count <= 228; count += 1) {
        renderer.report({
          phase: "installing",
          status: "Installing CEF runtime...",
          current: count,
          total: 0,
          determinate: false,
        });
      }

      expect(stderr.chunks.join("")).not.toContain("\n");

      await wait(450);
      expect(stderr.chunks.join("")).not.toContain("228 files");

      await wait(100);
      renderer.flush();
    } finally {
      stderr.restore();
    }

    const output = stderr.chunks.join("");
    expect(output).toMatch(/\r[-\\|/] Installing CEF runtime\.\.\. 228 files/u);
    expect(output.match(/\n/gu)?.length ?? 0).toBe(1);
  });

  it("renders the final installing file count before finalizing", () => {
    const stderr = captureStderr();
    try {
      const renderer = createMuonProgressRenderer();
      renderer.report({
        phase: "installing",
        status: "Installing CEF runtime...",
        current: 1,
        total: 0,
        determinate: false,
      });
      renderer.report({
        phase: "installing",
        status: "Installing CEF runtime...",
        current: 3,
        total: 0,
        determinate: false,
      });
      renderer.report({
        phase: "finalizing",
        status: "Starting muon...",
        current: 0,
        total: 0,
        determinate: false,
      });
      renderer.flush();
    } finally {
      stderr.restore();
    }

    const output = stderr.chunks.join("");
    expect(output).toMatch(/\r[-\\|/] Starting muon\.\.\./u);
    expect(getCompletedProgressLines(output)).toEqual([
      "Installing CEF runtime... 3 files",
      "Starting muon...",
    ]);
  });

  it("keeps different status messages as completed lines", () => {
    const stderr = captureStderr();
    try {
      const renderer = createMuonProgressRenderer();
      renderer.report({
        phase: "build",
        status: "Creating assets.zip",
        current: 0,
        total: 0,
        determinate: false,
      });
      renderer.report({
        phase: "build",
        status: "Updating Windows resources",
        current: 0,
        total: 0,
        determinate: false,
      });
      renderer.report({
        phase: "build",
        status: "Writing Linux desktop files",
        current: 0,
        total: 0,
        determinate: false,
      });
      renderer.flush();
    } finally {
      stderr.restore();
    }

    expect(getCompletedProgressLines(stderr.chunks.join(""))).toEqual([
      "Creating assets.zip",
      "Updating Windows resources",
      "Writing Linux desktop files",
    ]);
  });
});

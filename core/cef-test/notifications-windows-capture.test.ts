// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { expect, it } from "vitest";
import {
  captureWindowsNotification,
  captureWindowsNotificationOpening,
  createWindowsNotificationArtifacts,
} from "./notifications-windows-accessibility.ts";

it("captures the complete notification after its slide and content animations settle", async () => {
  const rectangles = [
    { x: 1900, y: 40, width: 334, height: 140 },
    { x: 1500, y: 40, width: 334, height: 140 },
  ];
  const frames = ["clipped", "moving content", "complete", "complete"];
  let reads = 0;
  const result = await captureWindowsNotification(
    async () => rectangles[reads++ === 0 ? 0 : 1]!,
    async () => {
      const frame = frames.shift();
      if (frame === undefined)
        throw new Error("A stable image should finish capture");
      return Buffer.from(frame);
    },
  );
  expect(result.toString()).toBe("complete");
});

it("preserves earlier videos while allocating a fresh destination for the next run", async () => {
  const parent = await mkdtemp(join(tmpdir(), "muon-notification-capture-"));
  try {
    const first = await createWindowsNotificationArtifacts(parent);
    await writeFile(join(first, "opening.mp4"), "previous recording", {
      flag: "wx",
    });
    const second = await createWindowsNotificationArtifacts(parent);
    await writeFile(join(second, "opening.mp4"), "next recording", {
      flag: "wx",
    });
    expect(await readFile(join(first, "opening.mp4"), "utf8")).toBe(
      "previous recording",
    );
    expect(await readFile(join(second, "opening.mp4"), "utf8")).toBe(
      "next recording",
    );
  } finally {
    await rm(parent, { recursive: true, force: true });
  }
});

it.each(["video", "notification"] as const)(
  "drains the other capture before reporting a %s failure to cleanup",
  async (failed) => {
    const failure = new Error(`${failed} failed`);
    let complete = (): void => {};
    const pending = new Promise<void>((resolve) => {
      complete = resolve;
    });
    const video = failed === "video" ? Promise.reject(failure) : pending;
    const notification =
      failed === "notification" ? Promise.reject(failure) : pending;
    let cleanup = false;
    const execution = (async () => {
      try {
        await captureWindowsNotificationOpening(video, notification);
      } catch (error) {
        return error;
      } finally {
        cleanup = true;
      }
    })();
    try {
      // Advance one event-loop turn, without a time delay, while the other
      // operation remains behind an explicit completion barrier.
      await new Promise<void>((resolve) => setImmediate(resolve));
      expect(cleanup).toBe(false);
    } finally {
      complete();
      expect(await execution).toBe(failure);
    }
    expect(cleanup).toBe(true);
  },
);

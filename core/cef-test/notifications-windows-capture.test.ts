// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { expect, it } from "vitest";
import { captureWindowsNotification } from "./notifications-windows-accessibility.ts";

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

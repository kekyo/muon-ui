// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { describe, expect, it } from "vitest";
import sharp from "sharp";
import { resolveMuonNotificationConfig } from "../common/notification-config.js";
import {
  resolveAndroidNotificationConfig,
  createAndroidNotificationIcon,
} from "../android/notifications.js";

describe("notification package settings", () => {
  it("keeps enablement and default category independent of Android channels", () => {
    expect(
      resolveMuonNotificationConfig({
        enabled: true,
        defaultCategory: "orders",
      }),
    ).toEqual({ enabled: true, defaultCategory: "orders" });
    expect(resolveMuonNotificationConfig(undefined)).toEqual({
      enabled: false,
    });
    expect(() =>
      resolveMuonNotificationConfig({ defaultCategory: "" }),
    ).toThrow(/category/i);
  });
  it("normalizes declared channels and validates the default reference", () => {
    const config = resolveAndroidNotificationConfig(
      {
        channels: [{ id: "orders", name: "Orders", importance: "default" }],
        smallIcon: "notification.png",
      },
      { enabled: true, defaultCategory: "orders" },
      "/app",
    );
    expect(config).toEqual({
      channels: [
        {
          id: "orders",
          name: "Orders",
          description: "",
          importance: "default",
        },
      ],
      smallIcon: "/app/notification.png",
    });
    expect(() =>
      resolveAndroidNotificationConfig(
        { channels: [], smallIcon: "notification.png" },
        { enabled: true, defaultCategory: "missing" },
        "/app",
      ),
    ).toThrow(/defaultCategory/);
  });
  it.each([
    {},
    {
      smallIcon: "notification.png",
      channels: [{ id: "x", name: "X", importance: "urgent" }],
    },
    {
      smallIcon: "notification.png",
      channels: [
        { id: "x", name: "X", importance: "default" },
        { id: "x", name: "Other", importance: "low" },
      ],
    },
    {
      smallIcon: "notification.png",
      channels: [{ id: "x", name: "X", importance: "default", sound: "url" }],
    },
    { smallIcon: "notification.png", enabled: true },
  ])(
    "rejects incomplete, duplicated and unsupported Android settings: %j",
    (value) => {
      expect(() =>
        resolveAndroidNotificationConfig(value, { enabled: true }, "/app"),
      ).toThrow();
    },
  );
  it("does not require an icon when notification operations are disabled", () => {
    expect(
      resolveAndroidNotificationConfig(undefined, { enabled: false }, "/app"),
    ).toEqual({ channels: [], smallIcon: undefined });
  });
  it("generates density-specific white silhouettes and preserves transparent pixels", async () => {
    const bytes = Buffer.alloc(24 * 24 * 4, 255);
    for (let y = 0; y < 24; y++)
      for (let x = 0; x < 24; x++)
        bytes[(y * 24 + x) * 4 + 3] =
          x >= 6 && x < 18 && y >= 6 && y < 18 ? 255 : 0;
    const source = await sharp(bytes, {
      raw: { width: 24, height: 24, channels: 4 },
    })
      .png()
      .toBuffer();
    const icons = await createAndroidNotificationIcon(source);
    expect(icons.map((icon) => icon.density)).toEqual([
      "mdpi",
      "hdpi",
      "xhdpi",
      "xxhdpi",
      "xxxhdpi",
    ]);
    for (const icon of icons) {
      const { data, info } = await sharp(icon.png)
        .raw()
        .toBuffer({ resolveWithObject: true });
      expect(info.width).toBe(icon.size);
      expect(info.height).toBe(icon.size);
      expect(data[3]).toBe(0);
      expect(
        data[
          (Math.floor(icon.size / 2) * icon.size + Math.floor(icon.size / 2)) *
            4 +
            3
        ],
      ).toBe(255);
    }
  });
  it.each([
    { r: 255, g: 255, b: 255, alpha: 1 },
    { r: 255, g: 255, b: 255, alpha: 0 },
    { r: 255, g: 0, b: 0, alpha: 0.5 },
  ])(
    "rejects icons without a usable monochrome alpha silhouette: %j",
    async (background) => {
      const png = await sharp({
        create: { width: 24, height: 24, channels: 4, background },
      })
        .png()
        .toBuffer();
      await expect(createAndroidNotificationIcon(png)).rejects.toThrow(/icon/i);
    },
  );
});

// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { expect, it, vi } from "vitest";
import * as cdp from "./helper.ts";
import { connectToMainPage, MUON_APP_URL } from "./plugin-e2e/shared.ts";

it("returns the rendered main document as the popup opener when CDP advertises it early", async () => {
  const other = {
    id: "other",
    type: "page",
    title: "",
    url: "data:text/html,other",
    webSocketDebuggerUrl: "ws://localhost/other",
  };
  const main = {
    ...other,
    id: "main",
    url: MUON_APP_URL,
    webSocketDebuggerUrl: "ws://localhost/main",
  };
  vi.spyOn(cdp, "listCdpTargets").mockResolvedValue([other, main]);
  const mainDriver: cdp.CdpDriver = {
    send: vi.fn(),
    on: vi.fn(),
    waitForEvent: vi.fn(),
    navigate: vi.fn(),
    screenshot: vi.fn(),
    close: vi.fn(),
    evaluate: vi
      .fn()
      .mockResolvedValueOnce("about:blank")
      .mockResolvedValue(MUON_APP_URL),
  };
  const otherDriver = {
    ...mainDriver,
    evaluate: vi.fn().mockResolvedValue(other.url),
  };
  vi.spyOn(cdp, "connectToMuonCdp").mockImplementation(async (options) =>
    options?.targetId === main.id ? mainDriver : otherDriver,
  );

  const opener = await connectToMainPage();
  expect(opener.targetId).toBe(main.id);
  await expect(opener.driver.evaluate("document.location.href")).resolves.toBe(
    MUON_APP_URL,
  );
  opener.driver.close();
});

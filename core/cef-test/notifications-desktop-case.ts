// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { join } from "node:path";
import { writeFile } from "node:fs/promises";
import { expect } from "vitest";
import type { MuonNotificationEvent } from "../common/renderer/notifications.js";

interface NotificationDesktopDriver {
  waitForNotification: (title: string, screenshotPath: string) => Promise<void>;
  activateNotification: (
    title: string,
    actionTitle: string | null,
    screenshotPath: string,
  ) => Promise<void>;
  waitForRemoval: (title: string) => Promise<void>;
}

/**
 * Exercises the public notification API against the real GNOME notification UI.
 * @param evaluate - Playwright MCP evaluator for the selected CEF page.
 * @param driver - Native accessibility actions, restricted to this test's titles.
 * @param directory - Directory for screenshots and observed events.
 * @param mode - Public API exposure mode used by the driver page.
 * @returns Completion after display, replacement, activation and cancellation.
 */
export const runNotificationDesktopCase = async (
  evaluate: <T>(source: string) => Promise<T>,
  driver: NotificationDesktopDriver,
  directory: string,
  mode: "simple" | "validate",
): Promise<void> => {
  const title = `Muon notification ${mode} ${process.pid}`;
  const capabilities = await evaluate(
    "return await notificationApi.getCapabilities();",
  );
  expect(capabilities).toMatchObject({
    interaction: { state: "supported" },
    actions: { state: "supported" },
    launch: { state: "unsupported" },
    persistence: { state: "unknown" },
  });
  const status = await evaluate(
    "return await notificationApi.getStatus('desktop-purpose');",
  );
  expect(status).toMatchObject({
    app: { state: "unknown" },
    category: { id: "desktop-purpose", state: "unknown" },
  });
  await evaluate("await beginWatch(); return true;");
  const first = {
    id: "desktop-order",
    title: `${title} first`,
    body: "Ready <plain text>",
    data: { revision: 1 },
  };
  await evaluate(
    `await notificationApi.post(${JSON.stringify(first)}); return true;`,
  );
  await driver.waitForNotification(first.title, join(directory, "first.png"));
  const second = {
    ...first,
    title: `${title} second`,
    category: "desktop-purpose",
    data: { revision: 2 },
    actions: [{ id: "default", title: "View order" }],
  };
  await evaluate(
    `await notificationApi.post(${JSON.stringify(second)}); return true;`,
  );
  await driver.waitForNotification(
    second.title,
    join(directory, "updated.png"),
  );
  await driver.waitForRemoval(first.title);
  await driver.activateNotification(
    second.title,
    "View order",
    join(directory, "named-action.png"),
  );
  const named = await evaluate<MuonNotificationEvent>(
    "return await nextEvent;",
  );
  expect(named).toMatchObject({
    id: "desktop-order",
    type: "action",
    actionId: "default",
    data: { revision: 2 },
  });
  expect(
    await evaluate("return await notificationApi.getPendingEvents();"),
  ).toEqual([named]);
  await evaluate(
    `await notificationApi.acknowledgeEvent(${JSON.stringify(named.eventId)}); await watcher.close(); await beginWatch(); return true;`,
  );
  const activation = {
    id: "desktop-open",
    title: `${title} open`,
    body: "Open the application",
    data: { screen: "inbox" },
  };
  await evaluate(
    `await notificationApi.post(${JSON.stringify(activation)}); return true;`,
  );
  await driver.activateNotification(
    activation.title,
    null,
    join(directory, "default-action.png"),
  );
  const opened = await evaluate<MuonNotificationEvent>(
    "return await nextEvent;",
  );
  expect(opened).toMatchObject({
    id: "desktop-open",
    type: "activate",
    actionId: null,
    data: { screen: "inbox" },
  });
  expect(opened.eventId).not.toBe(named.eventId);
  await evaluate(
    `await notificationApi.acknowledgeEvent(${JSON.stringify(opened.eventId)}); await watcher.close(); return true;`,
  );
  const canceled = {
    id: "desktop-cancel",
    title: `${title} cancel`,
    body: "This notification will be withdrawn",
  };
  await evaluate(
    `await notificationApi.post(${JSON.stringify(canceled)}); return true;`,
  );
  await driver.waitForNotification(
    canceled.title,
    join(directory, "cancel-before.png"),
  );
  await evaluate(
    "await notificationApi.cancel('desktop-cancel'); return true;",
  );
  await driver.waitForRemoval(canceled.title);
  expect(
    await evaluate("return await notificationApi.getPendingEvents();"),
  ).toEqual([]);
  await writeFile(
    join(directory, "result.json"),
    JSON.stringify(
      { mode, capabilities, status, named, opened, cancellation: true },
      null,
      2,
    ),
  );
};

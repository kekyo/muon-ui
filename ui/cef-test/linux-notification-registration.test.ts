// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { mkdtemp, readFile, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { afterEach, expect, it } from "vitest";
import {
  createLinuxDesktopEntry,
  resolveMuonLinuxDesktop,
  writeLinuxDesktopDistributionFiles,
} from "../cef/linux-desktop.js";

const directories: string[] = [];
afterEach(async () => {
  for (const directory of directories.splice(0))
    await rm(directory, { recursive: true, force: true });
});

const resolveDesktop = async (
  enabled: boolean,
  appId: string,
  desktopId: string | undefined,
) => {
  const root = await mkdtemp(join(tmpdir(), "muon-notification-desktop-"));
  directories.push(root);
  return await resolveMuonLinuxDesktop({
    root,
    packageDirectory: resolve("dist"),
    muonConfig: { notifications: { enabled } },
    muonConfigDirectory: root,
    options: desktopId === undefined ? undefined : { desktopId },
    appIconPath: resolve("../images/muon-256.png"),
    defaults: {
      desktopId: appId,
      name: "Notification sample",
      comment: "",
      categories: ["Utility"],
      startupNotify: true,
    },
  });
};

it("uses a stable D-Bus identity for a single-component default app ID when notifications are enabled or disabled", async () => {
  const enabled = await resolveDesktop(
    true,
    "3-notification-sample",
    undefined,
  );
  const disabled = await resolveDesktop(
    false,
    "3-notification-sample",
    undefined,
  );
  expect(enabled.desktopId).toMatch(
    /^[A-Za-z_-][A-Za-z0-9_-]*(?:\.[A-Za-z_-][A-Za-z0-9_-]*)+$/u,
  );
  expect(disabled.desktopId).toBe(enabled.desktopId);
  expect(enabled).toMatchObject({ notificationsEnabled: true });
  expect(disabled).toMatchObject({ notificationsEnabled: false });
});

it("keeps an explicit valid identity and rejects a notification identity that cannot be activated by D-Bus", async () => {
  const desktop = await resolveDesktop(true, "sample", "dev.muon.Sample");
  expect(desktop.desktopId).toBe("dev.muon.sample");
  await expect(resolveDesktop(true, "sample", "single-name")).rejects.toThrow(
    /D-Bus/u,
  );
});

it("exports notification launch ownership to the launcher sidecar and desktop entry", async () => {
  const desktop = await resolveDesktop(true, "dev.muon.sample", undefined);
  const output = await mkdtemp(join(tmpdir(), "muon-notification-sidecar-"));
  directories.push(output);
  await writeLinuxDesktopDistributionFiles(output, desktop);
  expect(
    JSON.parse(await readFile(join(output, desktop.configFileName), "utf8")),
  ).toMatchObject({
    desktopId: "dev.muon.sample",
    notificationsEnabled: true,
  });
  const input = {
    desktop,
    exec: '"/opt/sample/launcher" --muon-launch-from=normal',
    tryExec: "/opt/sample/launcher",
    icon: desktop.desktopId,
    appId: "sample-runtime",
    launcherPath: "/opt/sample/launcher",
  };
  expect(createLinuxDesktopEntry(input)).toContain("DBusActivatable=true\n");
  expect(createLinuxDesktopEntry(input)).toContain(
    "X-muon-Launcher=/opt/sample/launcher\n",
  );
  expect(createLinuxDesktopEntry(input)).toContain(
    "X-muon-AppId=sample-runtime\n",
  );
});

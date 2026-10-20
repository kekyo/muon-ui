// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFile } from "node:child_process";
import {
  chmod,
  copyFile,
  mkdir,
  mkdtemp,
  readFile,
  rm,
  watch,
  writeFile,
} from "node:fs/promises";
import { homedir, tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { promisify } from "node:util";
import { describe, expect, it } from "vitest";
import { createGnomeNotificationDriver } from "./notifications-accessibility.js";
import { createKdeNotificationDriver } from "./notifications-kde-accessibility.js";

const execute = promisify(execFile);
const enabled =
  process.platform === "linux" &&
  process.env.MUON_TEST_NOTIFICATION_DESKTOP === "1";
const shellQuote = (value: string): string =>
  `'${value.replaceAll("'", "'\\''")}'`;

describe.skipIf(!enabled)("Linux Portal activation on the desktop", () => {
  it("retains OS notifications, launches a new process, and acknowledges durable events", async () => {
    const directory = await mkdtemp(join(tmpdir(), "muon-portal-desktop-"));
    const desktopId = `dev.muon.PortalBackendTest${process.pid}`;
    const applicationName = `Muon Portal Backend ${process.pid}`;
    const data = process.env.XDG_DATA_HOME ?? join(homedir(), ".local/share");
    const applications = join(data, "applications");
    const services = join(data, "dbus-1/services");
    const theme = join(data, "icons/hicolor");
    const icons = join(theme, "256x256/apps");
    const desktopPath = join(applications, `${desktopId}.desktop`);
    const servicePath = join(services, `${desktopId}.service`);
    const iconPath = join(icons, `${desktopId}.png`);
    const launcher = join(directory, "launcher");
    const executable = resolve(
      "../cef/.build/test/linux-amd64/release/muon_notification_portal_driver",
    );
    const artifacts = resolve(
      process.env.MUON_TEST_NOTIFICATION_SHELL === "kde"
        ? ".run/notifications-portal-kde"
        : ".run/notifications-portal",
    );
    const arguments_ = [directory, desktopId, launcher];
    const driver = await (process.env.MUON_TEST_NOTIFICATION_SHELL === "kde"
      ? createKdeNotificationDriver()
      : createGnomeNotificationDriver());
    const installed: string[] = [];
    try {
      for (const path of [applications, services, icons, artifacts])
        await mkdir(path, { recursive: true });
      await writeFile(
        launcher,
        `#!/bin/sh\nexec ${[executable, ...arguments_, "receive"].map(shellQuote).join(" ")} >> ${shellQuote(join(directory, "receiver.log"))} 2>&1\n`,
      );
      await chmod(launcher, 0o755);
      // These files exercise the real desktop registration. The public pack
      // and launcher tests separately verify how distributions create them.
      await writeFile(
        desktopPath,
        `[Desktop Entry]\nType=Application\nName=${applicationName}\nExec=${launcher}\nIcon=${desktopId}\nDBusActivatable=true\nX-muon-Launcher=${launcher}\nX-GNOME-UsesNotifications=true\n`,
        { flag: "wx" },
      );
      installed.push(desktopPath);
      await writeFile(
        servicePath,
        `[D-BUS Service]\nName=${desktopId}\nExec="${launcher}" --muon-notification-activation\nX-muon-AppId=portal-driver\n`,
        { flag: "wx" },
      );
      installed.push(servicePath);
      await copyFile(resolve("../../images/muon-256.png"), iconPath);
      installed.push(iconPath);
      await execute("gtk-update-icon-cache", [
        "--force",
        "--ignore-theme-index",
        theme,
      ]);
      // GNOME refreshes its application cache asynchronously. Observe that
      // registration in the app grid before sending the first notification.
      await driver.waitForApplication(applicationName);
      const receivedPath = join(directory, "received.json");
      let previous: {
        eventId: string;
        data: unknown;
        type: string;
        actionId: string | null;
      }[] = [];
      for (const action of [null, "View original order"]) {
        await rm(receivedPath, { force: true });
        // This process exits normally before the actual shell UI is clicked.
        const posted = await execute(executable, [...arguments_, "post"], {
          timeout: 45000,
        });
        expect(JSON.parse(posted.stdout.trim()).launch.state).toBe("supported");
        const controller = new AbortController();
        const changes = watch(directory, { signal: controller.signal });
        const received = (async () => {
          for await (const change of changes)
            if (change.filename === "received.json")
              return JSON.parse(
                await readFile(receivedPath, "utf8"),
              ) as typeof previous;
          throw new Error(
            "Notification receiver stopped before publishing its event",
          );
        })();
        const deadline = setTimeout(() => controller.abort(), 60000);
        try {
          const [, events] = await Promise.all([
            driver.activateNotification(
              "Muon portal backend cold launch",
              action,
              join(artifacts, action === null ? "default.png" : "action.png"),
            ),
            received,
          ]);
          expect(events.slice(0, -1)).toEqual(previous);
          expect(events.at(-1)).toMatchObject({
            id: "cold",
            data: { order: 42 },
            type: action === null ? "activate" : "action",
            actionId: action === null ? null : "default",
          });
          expect(
            await readFile(join(directory, "startup.txt"), "utf8"),
          ).toMatch(/_TIME\d+/);
          previous = events;
          await driver.waitForRemoval("Muon portal backend cold launch");
        } finally {
          clearTimeout(deadline);
          controller.abort();
        }
        const reopened = await execute(executable, [...arguments_, "pending"], {
          timeout: 45000,
        });
        expect(JSON.parse(reopened.stdout.trim().split("\n").at(-1)!)).toEqual(
          previous,
        );
      }
      await execute(executable, [...arguments_, "post"], { timeout: 45000 });
      await driver.waitForNotification(
        "Muon portal backend cold launch",
        join(artifacts, "maintenance.png"),
      );
      const snapshot = JSON.parse(
        await readFile(join(directory, "snapshot.json"), "utf8"),
      );
      await copyFile(
        join(directory, "snapshot.json"),
        join(directory, "notifications.json"),
      );
      const configPath = join(directory, "maintenance.json");
      await writeFile(
        configPath,
        JSON.stringify({
          launcher: { appId: "portal-driver", desktopId },
          notifications: { enabled: false },
          browser: { profilePath: directory },
        }),
      );
      // A disabled update must withdraw history without starting CEF or
      // opening a display, while retaining events already delivered to disk.
      await execute(
        resolve("../cef/.run/test-linux-amd64-release/muon-core"),
        ["-c", configPath, "--muon-notification-clear-history"],
        {
          timeout: 45000,
          env: {
            ...process.env,
            MUON_LAUNCHER_PATH: launcher,
            DISPLAY: "",
            WAYLAND_DISPLAY: "",
          },
        },
      );
      const cleared = JSON.parse(
        await readFile(join(directory, "notifications.json"), "utf8"),
      );
      expect(cleared).toEqual({ ...snapshot, revisions: [] });
      await driver.waitForRemoval("Muon portal backend cold launch");
      await execute(executable, [...arguments_, "ack"], { timeout: 45000 });
      const acknowledged = await execute(
        executable,
        [...arguments_, "pending"],
        { timeout: 45000 },
      );
      expect(
        JSON.parse(acknowledged.stdout.trim().split("\n").at(-1)!),
      ).toEqual([]);
      await writeFile(
        join(artifacts, "events.json"),
        `${JSON.stringify(previous, null, 2)}\n`,
      );
    } finally {
      try {
        await execute(executable, [...arguments_, "cancel"], {
          timeout: 45000,
        });
      } catch {
        /* Registration may not have completed when setup failed. */
      }
      try {
        const peer = await execute("gdbus", [
          "call",
          "--session",
          "--dest",
          "org.freedesktop.DBus",
          "--object-path",
          "/org/freedesktop/DBus",
          "--method",
          "org.freedesktop.DBus.GetConnectionUnixProcessID",
          desktopId,
        ]);
        const pid = Number(peer.stdout.match(/uint32 (\d+)/)?.[1]);
        if (Number.isInteger(pid) && pid > 1) process.kill(pid, "SIGTERM");
      } catch {
        /* Normally the one-shot receiver has already exited. */
      }
      for (const path of installed) await rm(path, { force: true });
      if (installed.includes(iconPath))
        await execute("gtk-update-icon-cache", [
          "--force",
          "--ignore-theme-index",
          theme,
        ]);
      let receiverLog = "";
      try {
        receiverLog = await readFile(join(directory, "receiver.log"), "utf8");
      } catch {
        /* Startup may fail before the receiver writes diagnostics. */
      }
      await writeFile(join(artifacts, "receiver.log"), receiverLog);
      await driver.close();
      await rm(directory, { recursive: true, force: true });
    }
  });
});

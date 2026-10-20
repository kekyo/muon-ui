// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFile, spawn } from "node:child_process";
import { EventEmitter, once } from "node:events";
import { mkdir } from "node:fs/promises";
import { dirname, resolve } from "node:path";
import { createInterface } from "node:readline";
import { promisify } from "node:util";

const execute = promisify(execFile);
const accessible = "org.a11y.atspi.Accessible";
const properties = "org.freedesktop.DBus.Properties.Get";

interface Reference {
  bus: string;
  path: string;
}
interface AccessibleNode extends Reference {
  name: string;
  parent: AccessibleNode | undefined;
  children: AccessibleNode[];
}

const references = (value: string): Reference[] =>
  [...value.matchAll(/\('([^']*)', (?:objectpath )?'([^']*)'\)/g)].map(
    (match) => ({
      bus: match[1]!,
      path: match[2]!,
    }),
  );

// gdbus prints GVariant strings using either quote style. Decode only a string
// literal, never evaluate the CLI's output as JavaScript.
const stringValue = (value: string): string => {
  const literal = value.match(/'(?:\\.|[^'\\])*'|"(?:\\.|[^"\\])*"/)?.[0];
  if (literal === undefined)
    throw new Error(`No string in D-Bus result: ${value}`);
  return literal
    .slice(1, -1)
    .replace(
      /\\(u[\da-fA-F]{4}|x[\da-fA-F]{2}|.)/g,
      (_match, escaped: string) => {
        if (escaped[0] === "u" || escaped[0] === "x")
          return String.fromCodePoint(Number.parseInt(escaped.slice(1), 16));
        return (
          (
            { n: "\n", r: "\r", t: "\t", b: "\b", f: "\f" } as Record<
              string,
              string
            >
          )[escaped] ?? escaped
        );
      },
    );
};

/** Native GNOME notification observation through AT-SPI and pointer activation. */
export interface GnomeNotificationDriver {
  /** Waits for the exact test notification title to become visible, then captures its container.
   * @param title Exact title belonging to the test application.
   * @param screenshotPath Destination PNG path for the visible notification.
   * @returns When the notification is visible and the screenshot is saved.
   */
  waitForNotification(title: string, screenshotPath: string): Promise<void>;
  /** Activates only the matched test notification, or one named action in its container.
   * @param title Exact title belonging to the test application.
   * @param actionTitle Exact action label, or null for ordinary notification activation.
   * @param screenshotPath Destination PNG path captured before activation.
   * @returns When the pointer click has been sent to the matched title or button.
   */
  activateNotification(
    title: string,
    actionTitle: string | null,
    screenshotPath: string,
  ): Promise<void>;
  /** Waits until the exact test title is absent from the shell's notification tree.
   * @param title Exact title belonging to the test application.
   * @returns When no notification container exposes that title.
   */
  waitForRemoval(title: string): Promise<void>;
  /** Releases its event registration and restores the original pointer position.
   * @returns When the observer process has finished and the pointer has been restored.
   */
  close(): Promise<void>;
}

/**
 * Connects to GNOME's public accessibility bus before test notifications are posted.
 * @returns A driver whose waits are triggered by accessibility events, without polling.
 */
export const createGnomeNotificationDriver =
  async (): Promise<GnomeNotificationDriver> => {
    const address = stringValue(
      (
        await execute("gdbus", [
          "call",
          "--session",
          "--dest",
          "org.a11y.Bus",
          "--object-path",
          "/org/a11y/bus",
          "--method",
          "org.a11y.Bus.GetAddress",
        ])
      ).stdout,
    );
    const call = async (
      reference: Reference,
      method: string,
      args: string[],
    ): Promise<string> =>
      (
        await execute(
          "gdbus",
          [
            "call",
            "--address",
            address,
            "--dest",
            reference.bus,
            "--object-path",
            reference.path,
            "--method",
            method,
            ...args,
          ],
          { timeout: 15000, maxBuffer: 2 * 1024 * 1024 },
        )
      ).stdout;
    const root = {
      bus: "org.a11y.atspi.Registry",
      path: "/org/a11y/atspi/accessible/root",
    };
    let shell: Reference | undefined;
    for (const child of references(
      await call(root, `${accessible}.GetChildren`, []),
    )) {
      try {
        if (
          stringValue(await call(child, properties, [accessible, "Name"])) ===
          "gnome-shell"
        ) {
          shell = child;
          break;
        }
      } catch {
        /* A different application's root may disappear during enumeration. */
      }
    }
    if (shell === undefined)
      throw new Error("The GNOME shell accessibility root is unavailable");
    const shellRoot = shell;
    const originalPointer = (
      await execute("xdotool", ["getmouselocation", "--shell"])
    ).stdout;
    const originalX = originalPointer.match(/^X=(-?\d+)$/m)?.[1];
    const originalY = originalPointer.match(/^Y=(-?\d+)$/m)?.[1];
    if (originalX === undefined || originalY === undefined)
      throw new Error("The original pointer position is unavailable");
    let pointerMoved = false;
    let openedNotificationList = false;
    const observer = spawn(
      resolve(
        "../cef/.build/test/linux-amd64/release/muon_atspi_event_observer",
      ),
      [address, shell.bus],
      { stdio: ["pipe", "pipe", "pipe"] },
    );
    const events = new EventEmitter();
    let revision = 0;
    let ready = false;
    let closed = false;
    let failure: Error | undefined;
    let diagnostics = "";
    const lines = createInterface({ input: observer.stdout });
    lines.on("line", (line) => {
      if (line === "ready") ready = true;
      revision += 1;
      events.emit("changed");
    });
    observer.stderr.on("data", (value: Buffer) => {
      diagnostics += value.toString();
    });
    observer.on("error", (error) => {
      failure = error;
      events.emit("changed");
    });
    observer.on("close", (code) => {
      if (!closed)
        failure = new Error(
          `AT-SPI observer stopped (${code}): ${diagnostics}`,
        );
      events.emit("changed");
    });
    const close = async (): Promise<void> => {
      if (closed) return;
      closed = true;
      try {
        if (observer.exitCode === null && observer.signalCode === null) {
          const finished = once(observer, "close", {
            signal: AbortSignal.timeout(15000),
          });
          observer.stdin.end();
          await finished;
        }
      } finally {
        lines.close();
        try {
          if (openedNotificationList) {
            await execute("xdotool", ["key", "Escape"]);
            openedNotificationList = false;
          }
        } finally {
          if (pointerMoved)
            await execute("xdotool", ["mousemove", originalX, originalY]);
        }
      }
    };
    const waitForChange = async (
      previous: number,
      signal: AbortSignal,
    ): Promise<void> => {
      if (failure) throw failure;
      if (closed) throw new Error("AT-SPI notification driver is closed");
      if (revision === previous) await once(events, "changed", { signal });
      if (failure) throw failure;
    };
    try {
      const startup = AbortSignal.timeout(15000);
      while (!ready) await waitForChange(revision, startup);
    } catch (error) {
      await close();
      throw error;
    }

    const readTree = async (): Promise<AccessibleNode> => {
      const seen = new Set<string>();
      const read = async (
        reference: Reference,
        parent: AccessibleNode | undefined,
      ): Promise<AccessibleNode> => {
        const node: AccessibleNode = {
          ...reference,
          parent,
          name: "",
          children: [],
        };
        const key = `${reference.bus}${reference.path}`;
        if (seen.has(key)) return node;
        if (seen.size > 2048)
          throw new Error("Notification accessibility tree is too large");
        seen.add(key);
        try {
          const [name, children] = await Promise.all([
            call(reference, properties, [accessible, "Name"]),
            call(reference, `${accessible}.GetChildren`, []),
          ]);
          node.name = stringValue(name);
          // Only inspect shell objects. Never recurse into an unrelated app's UI.
          node.children = await Promise.all(
            references(children)
              .filter((child) => child.bus === shellRoot.bus)
              .map(async (child) => await read(child, node)),
          );
        } catch {
          /* A notification may disappear while its subtree is queried. */
        }
        return node;
      };
      // GNOME's Message is an AT-SPI notification (role 101). Query the public
      // Collection interface first, avoiding unrelated menus and the app grid.
      const matches = await call(
        shellRoot,
        "org.a11y.atspi.Collection.GetMatches",
        ["([], 1, {}, 1, [0,0,0,32], 1, [], 1, false)", "1", "0", "true"],
      );
      const root: AccessibleNode = {
        ...shellRoot,
        name: "",
        parent: undefined,
        children: [],
      };
      root.children = await Promise.all(
        references(matches)
          .filter((notification) => notification.bus === shellRoot.bus)
          .map(async (notification) => await read(notification, undefined)),
      );
      return root;
    };
    const find = (
      node: AccessibleNode,
      name: string,
    ): AccessibleNode | undefined => {
      if (node.name === name) return node;
      for (const child of node.children) {
        const result = find(child, name);
        if (result) return result;
      }
      return undefined;
    };
    const isShowing = async (node: AccessibleNode): Promise<boolean> => {
      const states = await call(node, `${accessible}.GetState`, []);
      const firstWord = Number(
        states.match(/\[\s*(?:uint32\s+)?(\d+)/)?.[1] ?? "0",
      );
      return (firstWord & (1 << 25)) !== 0 && (firstWord & (1 << 30)) !== 0;
    };
    const findShowing = async (
      node: AccessibleNode,
      name: string,
    ): Promise<AccessibleNode | undefined> => {
      if (node.name === name) {
        try {
          if (await isShowing(node)) return node;
        } catch {
          /* A banner may be replaced while its state is queried. */
        }
      }
      // The notification list and banner can expose separate objects with the
      // same title. A hidden list match must not hide a later visible banner.
      for (const child of node.children) {
        const match = await findShowing(child, name);
        if (match) return match;
      }
      return undefined;
    };
    const wait = async (
      title: string,
      present: boolean,
    ): Promise<AccessibleNode | undefined> => {
      const signal = AbortSignal.timeout(60000);
      let requestedList = false;
      try {
        while (true) {
          signal.throwIfAborted();
          const previous = revision;
          const tree = await readTree();
          if (!present && !find(tree, title)) return undefined;
          if (present) {
            const match = await findShowing(tree, title);
            if (match) return match;
            if (
              !requestedList &&
              !openedNotificationList &&
              find(tree, title)
            ) {
              // GNOME keeps expired banners in its notification list. Open that
              // public UI only after finding the exact test title, never by timer.
              requestedList = true;
              await execute("xdotool", ["key", "Super_L+v"]);
              openedNotificationList = true;
              continue;
            }
          }
          await waitForChange(previous, signal);
        }
      } catch (error) {
        throw new Error(
          `Notification ${present ? "showing" : "removal"} wait failed: ${title}`,
          { cause: error },
        );
      }
    };
    const notificationContainer = (node: AccessibleNode): AccessibleNode => {
      let container = node;
      // Collection matches are read with no parent, so this stops at role 101.
      while (container.parent) container = container.parent;
      return container;
    };
    const extents = async (node: AccessibleNode): Promise<number[]> => {
      if (!(await isShowing(node)))
        throw new Error(`Notification is no longer showing: ${node.name}`);
      const value = await call(node, "org.a11y.atspi.Component.GetExtents", [
        "0",
      ]);
      const match = value.match(/\(\((-?\d+), (-?\d+), (\d+), (\d+)\),\)/);
      if (!match)
        throw new Error(`Invalid notification screen extents: ${value}`);
      const bounds = match.slice(1).map(Number);
      if (
        bounds[0]! < 0 ||
        bounds[1]! < 0 ||
        bounds[2]! <= 0 ||
        bounds[3]! <= 0
      )
        throw new Error(
          `Notification has no visible screen rectangle: ${value}`,
        );
      return bounds;
    };
    const hover = async (node: AccessibleNode): Promise<void> => {
      const bounds = await extents(notificationContainer(node));
      // GNOME retains and expands a hovered banner. Moving only after SHOWING
      // prevents interacting with hidden notification-center objects.
      pointerMoved = true;
      await execute("xdotool", [
        "mousemove",
        String(Math.floor(bounds[0]! + bounds[2]! / 2)),
        String(Math.floor(bounds[1]! + bounds[3]! / 2)),
      ]);
    };
    const screenshot = async (
      node: AccessibleNode,
      path: string,
    ): Promise<void> => {
      const bounds = await extents(notificationContainer(node));
      await mkdir(dirname(path), { recursive: true });
      await execute(
        "import",
        [
          "-window",
          "root",
          "-crop",
          `${bounds[2]}x${bounds[3]}+${bounds[0]}+${bounds[1]}`,
          path,
        ],
        { timeout: 15000 },
      );
    };
    const click = async (node: AccessibleNode): Promise<void> => {
      const bounds = await extents(node);
      // GNOME St.Button exposes role and geometry but no AT-SPI actions here.
      // Use the actual visible control; never click a guessed screen position.
      pointerMoved = true;
      await execute("xdotool", [
        "mousemove",
        String(Math.floor(bounds[0]! + bounds[2]! / 2)),
        String(Math.floor(bounds[1]! + bounds[3]! / 2)),
        "click",
        "1",
      ]);
    };
    const expandNotification = async (node: AccessibleNode): Promise<void> => {
      const header = notificationContainer(node).children[0];
      if (!header) throw new Error("Notification header is unavailable");
      const buttons: AccessibleNode[] = [];
      const collect = async (current: AccessibleNode): Promise<void> => {
        if (current.name === "") {
          const role = await call(current, `${accessible}.GetRole`, []);
          if (Number(role.match(/(?:uint32\s+)?(\d+)/)?.[1]) === 43)
            buttons.push(current);
        }
        for (const child of current.children) await collect(child);
      };
      await collect(header);
      // GNOME 46 MessageHeader exposes its expand and close buttons in this
      // order. Require both; never guess a different or unrelated UI control.
      if (buttons.length !== 2 || !(await isShowing(buttons[0]!)))
        throw new Error("Notification expand/close controls are unavailable");
      await click(buttons[0]!);
    };
    return {
      waitForNotification: async (title, path) => {
        const matched = await wait(title, true);
        if (!matched) throw new Error(`Notification disappeared: ${title}`);
        await hover(matched);
        await screenshot(matched, path);
      },
      waitForRemoval: async (title) => {
        await wait(title, false);
      },
      activateNotification: async (title, actionTitle, path) => {
        try {
          // Actions use the stable notification list. Banner hover can itself
          // expand content, racing a manual expand toggle during its animation.
          if (!openedNotificationList) {
            await execute("xdotool", ["key", "Super_L+v"]);
            openedNotificationList = true;
          }
          const matched = await wait(title, true);
          if (!matched) throw new Error(`Notification disappeared: ${title}`);
          let chosen: AccessibleNode | undefined = matched;
          if (actionTitle !== null) {
            chosen = undefined;
            const signal = AbortSignal.timeout(60000);
            const expanded = new Set<string>();
            while (!chosen) {
              signal.throwIfAborted();
              const previous = revision;
              const current = await findShowing(await readTree(), title);
              const action = current
                ? find(notificationContainer(current), actionTitle)
                : undefined;
              try {
                if (action && (await isShowing(action))) chosen = action;
              } catch {
                /* A banner may replace a child between enumeration and state lookup. */
              }
              if (
                !chosen &&
                current &&
                action &&
                !expanded.has(notificationContainer(current).path)
              ) {
                expanded.add(notificationContainer(current).path);
                await expandNotification(current);
                continue;
              }
              if (!chosen) await waitForChange(previous, signal);
            }
          }
          await screenshot(chosen, path);
          // The default target remains the exact title, avoiding body links.
          await click(chosen);
          if (openedNotificationList) {
            await execute("xdotool", ["key", "Escape"]);
            openedNotificationList = false;
          }
        } catch (error) {
          throw new Error(
            `Notification activation failed: ${title}, action=${actionTitle ?? "<default>"}`,
            { cause: error },
          );
        }
      },
      close,
    };
  };

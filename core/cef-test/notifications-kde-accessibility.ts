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
import type { GnomeNotificationDriver } from "./notifications-accessibility.js";

const execute = promisify(execFile);
const accessible = "org.a11y.atspi.Accessible";
interface Reference {
  bus: string;
  path: string;
}
interface Node extends Reference {
  name: string;
  role: number;
  parent: Node | undefined;
  children: Node[];
}
const references = (value: string): Reference[] =>
  [...value.matchAll(/\('([^']*)', (?:objectpath )?'([^']*)'\)/g)].map(
    (match) => ({ bus: match[1]!, path: match[2]! }),
  );
const stringValue = (value: string): string => {
  const literal = value.match(/'(?:\\.|[^'\\])*'|"(?:\\.|[^"\\])*"/)?.[0];
  if (!literal) throw new Error(`Missing D-Bus string: ${value}`);
  return literal
    .slice(1, -1)
    .replace(
      /\\(u[\da-fA-F]{4}|x[\da-fA-F]{2}|.)/g,
      (_match, escaped: string) =>
        escaped[0] === "u" || escaped[0] === "x"
          ? String.fromCodePoint(Number.parseInt(escaped.slice(1), 16))
          : ((
              { n: "\n", r: "\r", t: "\t", b: "\b", f: "\f" } as Record<
                string,
                string
              >
            )[escaped] ?? escaped),
    );
};

/**
 * Observes the real Plasma shell through its public accessibility interface.
 * @returns A driver restricted to matching notification titles and their controls.
 * @remarks Qt 5 does not implement AT-SPI Collection or populate Cache.GetItems.
 * Only visible subtrees are visited; hidden applets can expose stale Qt objects.
 */
export const createKdeNotificationDriver =
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
    const registry = {
      bus: "org.a11y.atspi.Registry",
      path: "/org/a11y/atspi/accessible/root",
    };
    let shell: Reference | undefined;
    for (const child of references(
      await call(registry, `${accessible}.GetChildren`, []),
    )) {
      const name = await call(child, "org.freedesktop.DBus.Properties.Get", [
        accessible,
        "Name",
      ]);
      if (stringValue(name) === "plasmashell") shell = child;
    }
    if (!shell) throw new Error("The Plasma accessibility root is unavailable");
    const shellRoot = shell;
    const pointer = (await execute("xdotool", ["getmouselocation", "--shell"]))
      .stdout;
    const originalX = pointer.match(/^X=(-?\d+)$/m)?.[1];
    const originalY = pointer.match(/^Y=(-?\d+)$/m)?.[1];
    if (!originalX || !originalY)
      throw new Error("Pointer location is unavailable");
    const observer = spawn(
      resolve(
        "../cef/.build/test/linux-amd64/release/muon_atspi_event_observer",
      ),
      [address, shellRoot.bus],
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
        failure = new Error(`Plasma observer exited (${code}): ${diagnostics}`);
      events.emit("changed");
    });
    const waitForChange = async (
      previous: number,
      signal: AbortSignal,
    ): Promise<void> => {
      if (failure) throw failure;
      if (closed) throw new Error("Plasma notification driver is closed");
      if (previous === revision) await once(events, "changed", { signal });
      if (failure) throw failure;
    };
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
        await execute("xdotool", ["mousemove", originalX, originalY]);
      }
    };
    try {
      const signal = AbortSignal.timeout(15000);
      while (!ready) await waitForChange(revision, signal);
    } catch (error) {
      await close();
      throw error;
    }
    const showing = async (node: Reference): Promise<boolean> => {
      const state = await call(node, `${accessible}.GetState`, []);
      const word = Number(state.match(/\[\s*(?:uint32 )?(\d+)/)?.[1] ?? "0");
      return (word & (1 << 25)) !== 0 && (word & (1 << 30)) !== 0;
    };
    const readTree = async (): Promise<Node> => {
      const seen = new Set<string>();
      const read = async (
        reference: Reference,
        parent: Node | undefined,
      ): Promise<Node | undefined> => {
        if (seen.has(reference.path)) return undefined;
        seen.add(reference.path);
        if (seen.size > 1024)
          throw new Error("Plasma visible tree is too large");
        try {
          if (parent && !(await showing(reference))) return undefined;
          const [name, role, children] = await Promise.all([
            call(reference, "org.freedesktop.DBus.Properties.Get", [
              accessible,
              "Name",
            ]),
            call(reference, `${accessible}.GetRole`, []),
            call(reference, `${accessible}.GetChildren`, []),
          ]);
          const node: Node = {
            ...reference,
            name: stringValue(name),
            role: Number(role.match(/(?:uint32 )?(\d+)/)?.[1] ?? "0"),
            parent,
            children: [],
          };
          node.children = (
            await Promise.all(
              references(children)
                .filter((child) => child.bus === shellRoot.bus)
                .map(async (child) => await read(child, node)),
            )
          ).filter((child): child is Node => child !== undefined);
          return node;
        } catch (error) {
          if (!parent) throw error;
          return undefined;
        }
      };
      const root = await read(shellRoot, undefined);
      if (!root) throw new Error("Plasma disappeared");
      return root;
    };
    const find = (node: Node, name: string): Node | undefined => {
      if (node.name === name) return node;
      for (const child of node.children) {
        const result = find(child, name);
        if (result) return result;
      }
      return undefined;
    };
    const bounds = async (node: Node): Promise<number[]> => {
      if (!(await showing(node)))
        throw new Error(`Plasma control is hidden: ${node.name}`);
      const value = await call(node, "org.a11y.atspi.Component.GetExtents", [
        "0",
      ]);
      const match = value.match(/\(\((-?\d+), (-?\d+), (\d+), (\d+)\),\)/);
      const rectangle = match?.slice(1).map(Number);
      if (
        !rectangle ||
        rectangle.some((part, index) => (index < 2 ? part < 0 : part <= 0))
      )
        throw new Error(`Invalid Plasma control bounds: ${value}`);
      return rectangle;
    };
    const point = async (node: Node, click: boolean): Promise<void> => {
      const rectangle = await bounds(node);
      await execute("xdotool", [
        "mousemove",
        String(Math.floor(rectangle[0]! + rectangle[2]! / 2)),
        String(Math.floor(rectangle[1]! + rectangle[3]! / 2)),
        ...(click ? ["click", "1"] : []),
      ]);
    };
    const container = (node: Node): Node => {
      let result = node;
      // Qt exposes a history item as a form (20), and a popup as a frame (23).
      while (result.parent && result.role !== 20 && result.role !== 23)
        result = result.parent;
      return result;
    };
    const wait = async (
      title: string,
      present: boolean,
    ): Promise<Node | undefined> => {
      const signal = AbortSignal.timeout(60000);
      while (true) {
        signal.throwIfAborted();
        const previous = revision;
        const match = find(await readTree(), title);
        if (present === !!match) return match;
        await waitForChange(previous, signal);
      }
    };
    const screenshot = async (node: Node, path: string): Promise<void> => {
      const rectangle = await bounds(container(node));
      await mkdir(dirname(path), { recursive: true });
      await execute(
        "import",
        [
          "-window",
          "root",
          "-crop",
          `${rectangle[2]}x${rectangle[3]}+${rectangle[0]}+${rectangle[1]}`,
          path,
        ],
        { timeout: 15000 },
      );
    };
    return {
      waitForApplication: async () => {
        // KService's public cache builder consumes new desktop files before
        // the notification backend asks for their application name and icon.
        await execute(
          process.env.KDE_SESSION_VERSION === "6"
            ? "kbuildsycoca6"
            : "kbuildsycoca5",
          ["--noincremental"],
          { timeout: 60000 },
        );
      },
      waitForNotification: async (title, path) => {
        const node = await wait(title, true);
        if (!node) throw new Error(`Plasma notification disappeared: ${title}`);
        await point(node, false);
        await screenshot(node, path);
      },
      activateNotification: async (title, actionTitle, path) => {
        const node = await wait(title, true);
        if (!node) throw new Error(`Plasma notification disappeared: ${title}`);
        await point(node, false);
        let target =
          node.children.find(
            (child) => child.name === title && child.role === 29,
          ) ?? node;
        if (actionTitle !== null) {
          const action = find(container(node), actionTitle);
          if (!action)
            throw new Error(
              `Plasma notification action is unavailable: ${actionTitle}`,
            );
          target = action;
        }
        await screenshot(target, path);
        await point(target, true);
      },
      waitForRemoval: async (title) => {
        await wait(title, false);
      },
      close,
    };
  };

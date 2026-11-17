// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { spawn } from "node:child_process";
import { randomUUID } from "node:crypto";
import { mkdir, mkdtemp, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { createInterface } from "node:readline";
import type { MuonResolvedService } from "../common/service-config.ts";
import type { MuonDesktopTarget } from "./targets.ts";
import { stageMuonDesktopServices } from "./services.ts";

/** Generated private configuration shared by a development UI and its host. */
export interface MuonDevelopmentServiceConfig {
  /** A profile owned by this development session, preserved across UI recycle. */
  readonly browser: { readonly profilePath: string };
  /** Development disables OS login registration, including page UI preferences. */
  readonly serviceHost: {
    /** This host belongs to a development command. */
    readonly development: true;
    /** Absolute generated registry, when services are declared. */
    readonly registry?: string;
    /** Private endpoint of the supervisor-owned host. */
    readonly endpoint?: string;
  };
}

/** Lifetime of prepared services, independent of an individual UI process. */
export interface MuonDevelopmentServices {
  /** Configuration to merge after the project configuration. */
  readonly config: MuonDevelopmentServiceConfig;
  /**
   * Releases the UI startup guard and waits for the last service to stop.
   * @returns Host exit after all normal service references are gone.
   * @remarks Resident services keep the development command alive. This does
   * not change any saved autostart setting or create service references.
   */
  readonly waitForExit: () => Promise<void>;
  /**
   * Stops every service, waits for host exit, and removes session resources.
   * @returns Shared completion of idempotent cleanup.
   * @throws Error if the host failed or required forced termination.
   */
  readonly stop: () => Promise<void>;
}

/**
 * Stages services into a fresh snapshot and starts their independent host.
 * @param root - Application root owning the ignored .muon directory.
 * @param target - Selected desktop ABI.
 * @param runtimePath - Prepared runtime containing the service host executable.
 * @param definitions - Services validated by the shared distribution resolver.
 * @param environment - Environment inherited by the host.
 * @param signal - Development-command cancellation, including startup.
 * @returns Private configuration and development-session lifetime operations.
 * @remarks The bootstrap pipe owns the host even after the UI closes. Its EOF
 * stops resident services on supervisor death without an OS service manager.
 */
export const startMuonDevelopmentServices = async (
  root: string,
  target: MuonDesktopTarget,
  runtimePath: string,
  definitions: readonly MuonResolvedService[],
  environment: NodeJS.ProcessEnv,
  signal: AbortSignal,
): Promise<MuonDevelopmentServices> => {
  const base = join(root, ".muon", "development", target);
  await mkdir(base, { recursive: true });
  const directory = await mkdtemp(join(base, "session-"));
  const profilePath = join(directory, "profile");
  let socketDirectory: string | undefined;
  let stopHost: (() => Promise<void>) | undefined;
  let waitForExit = async (): Promise<void> => {};
  let cleanup: Promise<void> | undefined;
  const stop = async (): Promise<void> => {
    cleanup ??= (async () => {
      try {
        await stopHost?.();
      } finally {
        if (socketDirectory !== undefined)
          await rm(socketDirectory, { recursive: true, force: true });
        await rm(directory, { recursive: true, force: true });
      }
    })();
    await cleanup;
  };
  try {
    if (signal.aborted)
      throw new Error("Development session ended during startup");
    const snapshot = join(directory, "initial");
    const registry = await stageMuonDesktopServices(
      definitions,
      target,
      snapshot,
    );
    if (registry === undefined)
      return {
        config: {
          browser: { profilePath },
          serviceHost: { development: true },
        },
        waitForExit,
        stop,
      };

    const windows = target.startsWith("windows-");
    socketDirectory = windows
      ? undefined
      : await mkdtemp(join(tmpdir(), "muon-dev-"));
    const endpoint = windows
      ? "\\\\.\\pipe\\muon-service-dev-" + randomUUID()
      : join(socketDirectory!, "control");
    const registryPath = join(snapshot, registry);
    if (signal.aborted)
      throw new Error("Development session ended during startup");
    const host = spawn(
      join(
        runtimePath,
        windows ? "muon-service-host.exe" : "muon-service-host",
      ),
      [
        "--config",
        registryPath,
        "--endpoint",
        endpoint,
        "--data-root",
        join(profilePath, "services"),
        "--bootstrap",
        "development",
      ],
      {
        cwd: runtimePath,
        env: environment,
        windowsHide: true,
        stdio: windows ? "overlapped" : "pipe",
      },
    );
    let diagnostic = "";
    let processError: Error | undefined;
    let completed = false;
    let forced = false;
    host.stderr!.on("data", (bytes: Buffer) => {
      diagnostic = (diagnostic + bytes.toString()).slice(-65536);
    });
    host.once("error", (error) => {
      processError = error;
    });
    // Host exit can close the pipe immediately before the supervisor detaches.
    // The close result remains authoritative; an EPIPE must not crash Node.
    host.stdin!.on("error", () => {});
    let deadline: ReturnType<typeof setTimeout> | undefined;
    const beginStop = (): void => {
      if (completed) return;
      if (!host.stdin!.writableEnded) host.stdin!.end();
      deadline ??= setTimeout(() => {
        forced = true;
        host.kill("SIGKILL");
      }, 15_000);
    };
    const exited = new Promise<void>((resolve) => {
      host.once("close", () => {
        completed = true;
        clearTimeout(deadline);
        signal.removeEventListener("abort", beginStop);
        resolve();
      });
    });
    const checkExit = async (): Promise<void> => {
      await exited;
      if (forced || processError || host.exitCode !== 0)
        throw new Error(
          `muon development service host ${forced ? "exceeded its 15 second stop deadline" : "failed"}: ${processError?.message ?? host.signalCode ?? host.exitCode}${diagnostic ? "\n" + diagnostic.trim() : ""}`,
        );
    };
    stopHost = async () => {
      beginStop();
      await checkExit();
    };
    signal.addEventListener("abort", beginStop, { once: true });
    if (signal.aborted) beginStop();
    waitForExit = async () => {
      if (!completed && !host.stdin!.writableEnded) host.stdin!.write("d");
      await checkExit();
    };
    const lines = createInterface({ input: host.stdout! });
    let markReady!: () => void;
    const ready = new Promise<void>((resolve) => {
      markReady = resolve;
    });
    lines.on("line", (line) => {
      if (line === '{"ready":true}') markReady();
    });
    try {
      await Promise.race([
        ready,
        (async () => {
          await checkExit();
          throw new Error(
            "muon development service host exited before readiness",
          );
        })(),
      ]);
    } finally {
      // Keep draining stdout after readiness; state notifications may continue.
      lines.close();
      host.stdout!.resume();
    }
    return {
      config: {
        browser: { profilePath },
        serviceHost: { development: true, registry: registryPath, endpoint },
      },
      waitForExit,
      stop,
    };
  } catch (error) {
    try {
      await stop();
    } catch {
      /* Preserve the original startup diagnostic. */
    }
    throw error;
  }
};

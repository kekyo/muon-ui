// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { spawn } from "node:child_process";
import { randomUUID } from "node:crypto";
import { mkdir, mkdtemp, rm, writeFile } from "node:fs/promises";
import { createConnection } from "node:net";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { createInterface } from "node:readline";
import type { MuonServiceState } from "../../core/common/renderer/services.ts";
import type { MuonResolvedService } from "../common/service-config.ts";
import type { MuonServiceInputObserver } from "../common/service-inputs.ts";
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
   * Prepares a replacement before stopping the current host, then switches it.
   * @param runtimePath - Prepared runtime retained until this generation exits.
   * @param definitions - Newly validated services to stage into a fresh snapshot.
   * @param onInput - Optional observer for installed runtime dependencies.
   * @returns Completion after the replacement host is ready.
   * @remarks The endpoint and saved user preferences remain session-owned.
   */
  readonly update: (
    runtimePath: string,
    definitions: readonly MuonResolvedService[],
    onInput?: MuonServiceInputObserver,
  ) => Promise<void>;
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
 * @param onInput - Optional observer for installed runtime dependencies.
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
  onInput?: MuonServiceInputObserver,
): Promise<MuonDevelopmentServices> => {
  const base = join(root, ".muon", "development", target);
  await mkdir(base, { recursive: true });
  const directory = await mkdtemp(join(base, "session-"));
  const profilePath = join(directory, "profile");
  const windows = target.startsWith("windows-");
  const socketDirectory = windows
    ? undefined
    : await mkdtemp(join(tmpdir(), "muon-dev-"));
  const endpoint = windows
    ? "\\\\.\\pipe\\muon-service-dev-" + randomUUID()
    : join(socketDirectory!, "control");
  let config: MuonDevelopmentServiceConfig = {
    browser: { profilePath },
    serviceHost: { development: true },
  };
  let current: ReturnType<typeof launchHost> | undefined;
  let currentSnapshot: string | undefined;
  let updating: Promise<void> | undefined;
  let closing = false;
  let uiClosed = false;
  let cleanup: Promise<void> | undefined;
  const update = async (
    nextRuntime: string,
    nextDefinitions: readonly MuonResolvedService[],
    observe: MuonServiceInputObserver | undefined = undefined,
  ): Promise<void> => {
    const previous = updating;
    const operation = (async () => {
      if (previous) {
        try {
          await previous;
        } catch {
          /* A failed update does not prevent retry. */
        }
      }
      if (closing || signal.aborted)
        throw new Error("Development session ended during startup");
      if (!nextDefinitions.length && !current && !currentSnapshot) return;
      const snapshot = await mkdtemp(join(directory, "snapshot-"));
      let candidate: ReturnType<typeof launchHost> | undefined;
      try {
        let registry = await stageMuonDesktopServices(
          nextDefinitions,
          target,
          snapshot,
          nextRuntime,
          observe,
        );
        // Keep the supervised endpoint available when the last declaration is
        // removed. Existing pages can reconnect and observe the empty catalog.
        if (registry === undefined) {
          registry = "muon-services.json";
          await writeFile(join(snapshot, registry), '{"services":{}}\n');
        }
        if (closing || signal.aborted)
          throw new Error("Development session ended during startup");
        const oldSnapshot = currentSnapshot;
        try {
          await current?.stop();
        } finally {
          current = undefined;
        }
        if (closing || signal.aborted)
          throw new Error("Development session ended during startup");
        candidate = launchHost(
          nextRuntime,
          windows,
          join(snapshot, registry),
          endpoint,
          join(profilePath, "services"),
          environment,
          signal,
        );
        current = candidate;
        await candidate.ready;
        await waitForServices(
          endpoint,
          nextDefinitions.map(({ id }) => id),
          signal,
        );
        if (closing || signal.aborted)
          throw new Error("Development session ended during startup");
        config = {
          browser: { profilePath },
          serviceHost: {
            development: true,
            registry: join(snapshot, registry),
            endpoint,
          },
        };
        currentSnapshot = snapshot;
        if (uiClosed) candidate.releaseStartup();
        if (oldSnapshot)
          await rm(oldSnapshot, { recursive: true, force: true });
      } catch (error) {
        if (candidate) {
          try {
            await candidate.stop();
          } catch {
            /* Preserve the preparation/startup diagnostic. */
          }
          if (current === candidate) current = undefined;
        }
        await rm(snapshot, { recursive: true, force: true });
        throw error;
      }
    })();
    updating = operation;
    try {
      await operation;
    } finally {
      if (updating === operation) updating = undefined;
    }
  };
  const waitForExit = async (): Promise<void> => {
    uiClosed = true;
    for (;;) {
      const host = current;
      host?.releaseStartup();
      await host?.exited;
      if (updating) {
        try {
          await updating;
        } catch {
          /* The update caller reports its failure. */
        }
      }
      if (current !== host) continue;
      await host?.checkExit();
      return;
    }
  };
  const stop = async (): Promise<void> => {
    cleanup ??= (async () => {
      closing = true;
      current?.beginStop();
      try {
        if (updating) {
          try {
            await updating;
          } catch {
            /* The update caller reports its failure. */
          }
        }
        await current?.stop();
      } finally {
        if (socketDirectory !== undefined)
          await rm(socketDirectory, { recursive: true, force: true });
        await rm(directory, { recursive: true, force: true });
      }
    })();
    await cleanup;
  };
  try {
    await update(runtimePath, definitions, onInput);
    return {
      get config() {
        return config;
      },
      waitForExit,
      update,
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

const launchHost = (
  runtimePath: string,
  windows: boolean,
  registryPath: string,
  endpoint: string,
  dataRoot: string,
  environment: NodeJS.ProcessEnv,
  signal: AbortSignal,
) => {
  const host = spawn(
    join(runtimePath, windows ? "muon-service-host.exe" : "muon-service-host"),
    [
      "--config",
      registryPath,
      "--endpoint",
      endpoint,
      "--data-root",
      dataRoot,
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
  let stopping = false;
  let stopFailure: string | undefined;
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
    stopping = true;
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
    if (forced || processError || stopFailure || host.exitCode !== 0)
      throw new Error(
        `muon development service host ${forced ? "exceeded its 15 second stop deadline" : "failed"}: ${stopFailure ?? processError?.message ?? host.signalCode ?? host.exitCode}${diagnostic ? "\n" + diagnostic.trim() : ""}`,
      );
  };
  const stop = async () => {
    beginStop();
    await checkExit();
  };
  signal.addEventListener("abort", beginStop, { once: true });
  if (signal.aborted) beginStop();
  const releaseStartup = (): void => {
    if (!completed && !host.stdin!.writableEnded) host.stdin!.write("d");
  };
  const lines = createInterface({ input: host.stdout! });
  let markReady!: () => void;
  const notification = new Promise<void>((resolve) => {
    markReady = resolve;
  });
  // Keep draining state notifications after readiness, including failures
  // during asynchronous stop. Readline closes when the host closes stdout.
  lines.on("line", (line) => {
    if (line === '{"ready":true}') markReady();
    else if (stopping) {
      try {
        const state = JSON.parse(line);
        if (state.state === "failed" && typeof state.error === "string")
          stopFailure = `services.${state.id}: ${state.error}`;
      } catch {
        /* Diagnostic text is retained separately on stderr. */
      }
    }
  });
  const ready = Promise.race([
    notification,
    (async () => {
      await checkExit();
      throw new Error("muon development service host exited before readiness");
    })(),
  ]);
  return { ready, exited, checkExit, stop, beginStop, releaseStartup };
};

// Observe autostart completion without adding an application reference. Each
// pending state request waits at the host; startup does not poll or toggle policy.
const waitForServices = async (
  endpoint: string,
  ids: readonly string[],
  signal: AbortSignal,
): Promise<void> => {
  const socket = createConnection({ path: endpoint, signal });
  let sequence = 0;
  let failure: Error | undefined;
  const pending = new Map<
    string,
    { resolve: (value: unknown) => void; reject: (error: Error) => void }
  >();
  socket.on("error", (error) => {
    failure = error;
  });
  socket.on("close", () => {
    failure ??= new Error(
      "Development service host disconnected during startup",
    );
    for (const operation of pending.values()) operation.reject(failure);
    pending.clear();
  });
  const lines = createInterface({ input: socket });
  lines.on("line", (line) => {
    const message = JSON.parse(line);
    const operation = pending.get(message.requestId);
    if (!operation) return;
    pending.delete(message.requestId);
    if (message.error) operation.reject(new Error(message.error.message));
    else operation.resolve(message.result);
  });
  const request = async <T>(
    serviceId: string,
    operation: string,
    token: string | undefined,
  ): Promise<T> => {
    if (failure || socket.destroyed)
      throw failure ?? new Error("Development service host disconnected");
    const requestId = String(++sequence);
    const result = new Promise<unknown>((resolve, reject) =>
      pending.set(requestId, { resolve, reject }),
    );
    socket.write(
      JSON.stringify({
        requestId,
        op: "watchState",
        serviceId,
        operation,
        token,
      }) + "\n",
    );
    return (await result) as T;
  };
  try {
    for (const id of ids) {
      const observer = await request<{
        token: string;
        state: MuonServiceState;
      }>(id, "acquire", undefined);
      let state = observer.state;
      while (state.state === "starting" || state.state === "stopping")
        state = (
          await request<{ state: MuonServiceState }>(id, "next", observer.token)
        ).state;
      if (state.state === "failed")
        throw new Error(`services.${id} failed to start: ${state.error}`);
      await request(id, "release", observer.token);
    }
  } finally {
    lines.close();
    socket.destroy();
  }
};

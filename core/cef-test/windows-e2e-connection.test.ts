// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { connectRemoteAgent, type RemoteAgent } from "agent-rover";
import { beforeEach, describe, expect, it, vi } from "vitest";

import {
  connectWindowsE2eAgent,
  resolveWindowsE2eEnvironment,
} from "../../scripts/windows-e2e-environment.js";
import { connectWindowsAgent } from "./plugin-e2e/windows-agent.js";
import { buildWindowsRuntime } from "./plugin-e2e/windows-build.js";
import { resolveWindowsRuntimeTarget } from "./plugin-e2e/windows-matrix.js";

vi.mock("agent-rover", () => ({ connectRemoteAgent: vi.fn() }));
vi.mock("./plugin-e2e/windows-build.js", () => ({
  buildWindowsRuntime: vi.fn(),
}));

beforeEach(() => {
  vi.mocked(connectRemoteAgent).mockReset();
  vi.mocked(buildWindowsRuntime).mockReset();
});

describe("Windows runtime preparation", () => {
  const environment = resolveWindowsE2eEnvironment({});
  const target = resolveWindowsRuntimeTarget("windows-i686");

  it("skips without building when a machine cannot be reached", async () => {
    vi.mocked(connectRemoteAgent).mockRejectedValue(new Error("No machine"));

    await expect(connectWindowsAgent(environment, target)).resolves.toEqual({
      reason: "No machine",
      status: "skip",
    });
    expect(buildWindowsRuntime).not.toHaveBeenCalled();
  });

  it("releases the machine during local builds and reconnects for testing", async () => {
    const probe = { release: vi.fn() } as unknown as RemoteAgent;
    const agent = { release: vi.fn() } as unknown as RemoteAgent;
    vi.mocked(connectRemoteAgent)
      .mockResolvedValueOnce(probe)
      .mockResolvedValueOnce(agent);
    vi.mocked(buildWindowsRuntime).mockImplementation(async (runtimeTarget) => {
      expect(runtimeTarget).toEqual(target);
      expect(probe.release).toHaveBeenCalledOnce();
    });

    await expect(connectWindowsAgent(environment, target)).resolves.toEqual({
      agent,
      status: "connected",
    });
    expect(buildWindowsRuntime).toHaveBeenCalledOnce();
    expect(agent.release).not.toHaveBeenCalled();
  });

  it("fails a build error without holding a machine connection", async () => {
    const probe = { release: vi.fn() } as unknown as RemoteAgent;
    const failure = new Error("Windows compilation failed");
    vi.mocked(connectRemoteAgent).mockResolvedValue(probe);
    vi.mocked(buildWindowsRuntime).mockRejectedValue(failure);

    await expect(connectWindowsAgent(environment, target)).rejects.toBe(
      failure,
    );
    expect(probe.release).toHaveBeenCalledOnce();
    expect(connectRemoteAgent).toHaveBeenCalledOnce();
  });

  it("skips if the machine cannot be reached after local preparation", async () => {
    const probe = { release: vi.fn() } as unknown as RemoteAgent;
    vi.mocked(connectRemoteAgent)
      .mockResolvedValueOnce(probe)
      .mockRejectedValueOnce(new Error("Disconnected before testing"));

    await expect(connectWindowsAgent(environment, target)).resolves.toEqual({
      reason: "Disconnected before testing",
      status: "skip",
    });
    expect(probe.release).toHaveBeenCalledOnce();
  });
});

describe("Windows e2e connection", () => {
  it("connects through the standard win11 section during ordinary tests", async () => {
    const agent = { release: vi.fn() } as unknown as RemoteAgent;
    vi.mocked(connectRemoteAgent).mockResolvedValue(agent);

    const connection = await connectWindowsE2eAgent(
      resolveWindowsE2eEnvironment({}),
    );

    expect(connection).toEqual({ agent, status: "connected" });
    expect(connectRemoteAgent).toHaveBeenCalledWith({
      section: "win11",
      timeoutMs: 30000,
    });
    expect(agent.release).not.toHaveBeenCalled();
  });

  it.each([
    "No .agent-rover configuration found",
    "The configured section is empty",
    "Connection refused",
    "Authentication failed",
  ])("skips when the SDK cannot connect: %s", async (message) => {
    vi.mocked(connectRemoteAgent).mockRejectedValue(new Error(message));

    await expect(
      connectWindowsE2eAgent(resolveWindowsE2eEnvironment({})),
    ).resolves.toEqual({ reason: message, status: "skip" });
  });

  it("propagates an operation failure after a successful connection", async () => {
    const failure = new Error("Remote operation failed");
    const agent = {
      capabilities: vi.fn().mockRejectedValue(failure),
    } as unknown as RemoteAgent;
    vi.mocked(connectRemoteAgent).mockResolvedValue(agent);

    const connection = await connectWindowsE2eAgent(
      resolveWindowsE2eEnvironment({}),
    );

    expect(connection.status).toBe("connected");
    if (connection.status !== "connected") {
      throw new Error(connection.reason);
    }
    await expect(connection.agent.capabilities()).rejects.toBe(failure);
  });
});

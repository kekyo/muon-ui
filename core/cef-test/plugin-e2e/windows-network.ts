// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { randomUUID } from "node:crypto";
import { createConnection, isIPv4 } from "node:net";

import type { RemoteAgent } from "agent-rover";
import { vi } from "vitest";

/** Addresses for Muon CDP and test HTTP traffic on the selected machine. */
export interface WindowsTestNetwork {
  /** Reachable IPv4 address of the selected Windows machine. */
  cdpHost: string;
  /** Local address used to reach that machine. */
  httpHost: string;
}

// Run only on the machine selected by the SDK. The nonce distinguishes this
// probe from unrelated listeners, including a loopback listener on the driver.
const networkProbeScript = `
const { createServer } = require('node:net');
const { networkInterfaces } = require('node:os');
const server = createServer((socket) => {
  socket.on('error', () => {});
  socket.end(process.argv[1]);
});
server.listen(0, '0.0.0.0', () => {
  const interfaces = Object.values(networkInterfaces()).flat();
  const addresses = interfaces.filter((entry) => entry.family === 'IPv4')
    .sort((left, right) => Number(left.internal) - Number(right.internal))
    .map((entry) => entry.address);
  console.log(JSON.stringify({ addresses, port: server.address().port }));
});
`;

const connectNetworkProbe = async (
  host: string,
  port: number,
  nonce: string,
): Promise<WindowsTestNetwork> => {
  const socket = createConnection({ host, port });
  try {
    return await new Promise<WindowsTestNetwork>((resolve, reject) => {
      let response = "";
      let network: WindowsTestNetwork | undefined;
      socket.setEncoding("utf8");
      socket.setTimeout(3000, () =>
        socket.destroy(new Error("Network probe timed out")),
      );
      socket.once("error", reject);
      socket.once("connect", () => {
        if (
          socket.remoteAddress !== undefined &&
          socket.localAddress !== undefined
        ) {
          network = {
            cdpHost: socket.remoteAddress,
            httpHost: socket.localAddress,
          };
        }
      });
      socket.on("data", (chunk: string) => {
        response += chunk;
      });
      socket.once("end", () => {
        if (response === nonce && network !== undefined) {
          resolve(network);
        } else {
          reject(
            new Error(
              "Network probe response did not match the selected machine",
            ),
          );
        }
      });
    });
  } finally {
    socket.destroy();
  }
};

/**
 * Discovers the route to the machine already selected by agent-rover.
 * @param agent Connected Windows agent.
 * @param nodePath Node.js executable for the temporary probe.
 * @returns Addresses for CDP and HTTP test fixtures.
 * @remarks Uses a temporary listener and always releases its managed process.
 * No agent connection settings or credentials are read by this helper.
 */
export const discoverWindowsTestNetwork = async (
  agent: RemoteAgent,
  nodePath: string,
): Promise<WindowsTestNetwork> => {
  const nonce = randomUUID();
  const probe = await agent.processes.launchManaged({
    path: nodePath,
    arguments: ["-e", networkProbeScript, nonce],
    captureStdout: true,
    captureStderr: true,
    createNoWindow: true,
  });
  try {
    const output = await vi.waitUntil(
      async () => {
        const stdout = await probe.stdoutText();
        if (stdout.endsWith("\n")) return stdout;
        if (!(await probe.rootSnapshot()).running) {
          throw new Error(
            `Windows network probe exited: ${await probe.stderrText()}`,
          );
        }
        return false;
      },
      { timeout: 30000, interval: 100 },
    );
    const { addresses, port } = JSON.parse(output) as {
      addresses: string[];
      port: number;
    };
    const errors: unknown[] = [];
    for (const address of addresses.filter(isIPv4)) {
      try {
        return await connectNetworkProbe(address, port, nonce);
      } catch (error) {
        errors.push(error);
      }
    }
    throw new AggregateError(
      errors,
      "No reachable IPv4 route to the selected Windows machine",
    );
  } finally {
    await probe.releaseAsync();
  }
};

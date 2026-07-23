// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { mkdtemp, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";

import { expect, it, vi } from "vitest";

import {
  cdpCommandTimeoutMs,
  connectToMuonCdp,
  delay,
  MUON_PORT,
  startDebugMuon,
  stopMuon,
  TEST_NETWORK_ALLOW_PATTERNS,
  type CdpDriver,
  type RunningMuon,
} from "./plugin-e2e/shared.js";
import { isWindowsRemoteE2e } from "./plugin-e2e/windows-context.js";

interface ExecutorSpawnOptions {
  args: string[];
  command: string;
}

interface StressAttemptOptions {
  attempt: number;
  attempts: number;
  churnPath: string;
  churnWorkers: number;
  iterations: number;
}

const stressEnabled = process.env.MUON_EXECUTOR_FD_STRESS === "1";
const isLocalLinuxE2e = process.platform === "linux" && !isWindowsRemoteE2e();
const stressIt = stressEnabled && isLocalLinuxE2e ? it : it.skip;

const readPositiveIntegerEnv = (name: string, defaultValue: number): number => {
  if (!stressEnabled) {
    return defaultValue;
  }
  const raw = process.env[name];
  if (raw === undefined || raw === "") {
    return defaultValue;
  }
  const value = Number(raw);
  if (!Number.isInteger(value) || value <= 0) {
    throw new Error(`${name} must be a positive integer.`);
  }
  return value;
};

const stressAttempts = readPositiveIntegerEnv(
  "MUON_EXECUTOR_FD_STRESS_ATTEMPTS",
  20,
);
const stressIterations = readPositiveIntegerEnv(
  "MUON_EXECUTOR_FD_STRESS_ITERATIONS",
  50,
);
const stressChurnWorkers = readPositiveIntegerEnv(
  "MUON_EXECUTOR_FD_STRESS_CHURN",
  8,
);
const stressTimeoutMs = readPositiveIntegerEnv(
  "MUON_EXECUTOR_FD_STRESS_TIMEOUT_MS",
  600000,
);
const stressCdpTimeoutMs = Math.max(cdpCommandTimeoutMs, 30000);

if (stressEnabled) {
  vi.setConfig({
    hookTimeout: stressTimeoutMs,
    testTimeout: stressTimeoutMs,
  });
}

const createExecutorLongRunningSpawnOptions = (
  marker: string,
): ExecutorSpawnOptions => ({
  args: [
    "-e",
    `const marker = ${JSON.stringify(marker)}; setInterval(() => {}, 1000);`,
  ],
  command: process.execPath,
});

const createExecutorStdinSpawnOptions = (): ExecutorSpawnOptions => ({
  args: [
    "-e",
    [
      'let input = "";',
      'process.stdin.setEncoding("utf8");',
      'process.stdin.on("data", (chunk) => { input += chunk; });',
      'process.stdin.on("end", () => {',
      '  process.stdout.write("stdout:" + input);',
      '  process.stderr.write("stderr:ok");',
      "  process.exitCode = 7;",
      "});",
    ].join("\n"),
  ],
  command: process.execPath,
});

const startFdChurnExpression = (
  churnPath: string,
  churnWorkers: number,
): string => `(async () => {
  const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
  const state = {
    errors: [],
    readCount: 0,
    stop: false,
  };
  globalThis.__muonExecutorFdStress = state;
  for (let worker = 0; worker < ${JSON.stringify(churnWorkers)}; worker += 1) {
    void (async () => {
      while (!state.stop) {
        try {
          await window.muon.fs.readFile(${JSON.stringify(churnPath)});
          state.readCount += 1;
        } catch (error) {
          state.errors.push(String(error));
        }
        await sleep((worker % 3) + 1);
      }
    })();
  }
  await sleep(20);
  return { errorCount: state.errors.length, readCount: state.readCount };
})()`;

const stopFdChurnExpression = (): string => `(async () => {
  const state = globalThis.__muonExecutorFdStress;
  if (state === undefined) {
    return { errorCount: 0, readCount: 0 };
  }
  state.stop = true;
  await new Promise((resolve) => setTimeout(resolve, 20));
  return { errorCount: state.errors.length, readCount: state.readCount };
})()`;

const verifyFsReadExpression = (churnPath: string): string => `(async () => {
  const data = await window.muon.fs.readFile(${JSON.stringify(churnPath)});
  return data.byteLength ?? data.length ?? 0;
})()`;

const runExecutorIterationExpression = (
  iteration: number,
  longRunningOptions: ExecutorSpawnOptions,
  stdinOptions: ExecutorSpawnOptions,
): string => `(async () => {
  const fail = (message) => {
    throw new Error(\`iteration ${iteration}: \${message}\`);
  };
  const child = await window.muon.executor.spawn(${JSON.stringify(
    longRunningOptions,
  )});
  await child.kill();
  const waitResult = await child.wait();
  if (waitResult.processId <= 0) {
    fail("killed child did not report a process id");
  }
  if (waitResult.exitCode === 0) {
    fail("killed child exited cleanly");
  }

  const closedChild = await window.muon.executor.spawn(${JSON.stringify(
    stdinOptions,
  )});
  await closedChild.closeStdin();
  let writeAfterCloseRejected = false;
  try {
    await closedChild.writeStdin("x");
  } catch {
    writeAfterCloseRejected = true;
  }
  if (!writeAfterCloseRejected) {
    fail("write after closeStdin was accepted");
  }
  await closedChild.release();
  let writeAfterReleaseRejected = false;
  try {
    await closedChild.writeStdin("x");
  } catch {
    writeAfterReleaseRejected = true;
  }
  if (!writeAfterReleaseRejected) {
    fail("write after release was accepted");
  }

  return {
    exitCode: waitResult.exitCode,
    iteration: ${JSON.stringify(iteration)},
    processId: waitResult.processId,
  };
})()`;

const runStressAttempt = async ({
  attempt,
  attempts,
  churnPath,
  churnWorkers,
  iterations,
}: StressAttemptOptions): Promise<void> => {
  let running: RunningMuon | undefined = undefined;
  let driver: CdpDriver | undefined = undefined;
  let currentIteration = 0;
  try {
    running = await startDebugMuon(
      [],
      TEST_NETWORK_ALLOW_PATTERNS,
      {},
      undefined,
      ["muon.executor.spawn", "muon.fs.readFile"],
    );
    driver = await connectToMuonCdp({
      port: MUON_PORT,
      timeoutMs: stressCdpTimeoutMs,
    });
    await driver.navigate(
      `data:text/html,<title>muon executor fd stress ${attempt}</title>`,
      stressCdpTimeoutMs,
    );
    await expect(
      driver.evaluate<number>(verifyFsReadExpression(churnPath)),
    ).resolves.toBeGreaterThan(0);
    await driver.evaluate(startFdChurnExpression(churnPath, churnWorkers));

    for (let iteration = 1; iteration <= iterations; iteration += 1) {
      currentIteration = iteration;
      const marker = [
        "muon-executor-fd-stress",
        attempt,
        iteration,
        Date.now().toString(36),
      ].join("-");
      await driver.evaluate(
        runExecutorIterationExpression(
          iteration,
          createExecutorLongRunningSpawnOptions(marker),
          createExecutorStdinSpawnOptions(),
        ),
      );
      if (iteration % 10 === 0) {
        await delay(1);
      }
    }

    const churnResult = await driver.evaluate<{
      errorCount: number;
      readCount: number;
    }>(stopFdChurnExpression());
    expect(churnResult.readCount).toBeGreaterThan(0);
  } catch (error) {
    throw new Error(
      [
        `executor FD stress failed at attempt ${attempt}/${attempts}, ` +
          `iteration ${currentIteration}/${iterations}`,
        String(error),
        "Muon stderr:",
        running?.stderr ?? "",
      ].join("\n"),
    );
  } finally {
    if (running !== undefined) {
      await stopMuon(running, driver);
    }
  }
};

stressIt(
  "executor fd reuse stress does not crash muon",
  async () => {
    const directory = await mkdtemp(join(tmpdir(), "muon-executor-fd-stress-"));
    const churnPath = join(directory, "churn.bin");
    try {
      await writeFile(churnPath, Buffer.alloc(4096, 0x5a));
      for (let attempt = 1; attempt <= stressAttempts; attempt += 1) {
        await runStressAttempt({
          attempt,
          attempts: stressAttempts,
          churnPath,
          churnWorkers: stressChurnWorkers,
          iterations: stressIterations,
        });
      }
    } finally {
      await rm(directory, { recursive: true, force: true });
    }
  },
  stressTimeoutMs,
);

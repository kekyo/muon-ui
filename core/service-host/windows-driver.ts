// Remote-only observation driver. The Vitest suite uploads this TypeScript file
// and runs it with Node's built-in type stripping; no test package is installed.
import assert from "node:assert/strict";
import { execFile, spawn, type ChildProcess } from "node:child_process";
import { randomUUID } from "node:crypto";
import { once } from "node:events";
import { mkdir, rename, rm, writeFile } from "node:fs/promises";
import { createConnection, createServer, type Socket } from "node:net";
import { join, resolve } from "node:path";
import { createInterface } from "node:readline";
import { promisify } from "node:util";

const execute = promisify(execFile);
const binaryDirectory = resolve(process.argv[2]!);
const children: ChildProcess[] = [];
const sockets: Socket[] = [];
const directory = join(binaryDirectory, "日本語-service-" + randomUUID());
await mkdir(directory);

const availablePort = async (): Promise<number> => {
  const server = createServer();
  server.listen(0, "127.0.0.1");
  await once(server, "listening");
  const address = server.address();
  assert(address && typeof address !== "string");
  const closed = once(server, "close");
  server.close();
  await closed;
  return address.port;
};

try {
  for (const mode of ["simple", "probe"]) {
    for (const shutdown of ["command", "session"]) {
      const endpoint = String.raw`\\.\pipe\muon-service-${randomUUID()}`;
      const ports = [await availablePort(), await availablePort()];
      const config = join(directory, "services.json");
      const library = "../" + mode + "-service.dll";
      await writeFile(
        config,
        JSON.stringify({
          services: {
            "resident.v1-test": {
              library,
              autostart: true,
              config: { port: String(ports[0]), label: "日本語😀" },
            },
            borrowed: {
              library,
              autostart: false,
              config: { port: String(ports[1]) },
            },
            broken: { library: "./absent.dll", autostart: false },
          },
        }),
      );
      const args = [
        "--config",
        config,
        "--endpoint",
        endpoint,
        "--data-root",
        join(directory, "data"),
      ];
      const host = spawn(join(binaryDirectory, "muon-service-host.exe"), args, {
        stdio: ["ignore", "pipe", "pipe"],
        windowsHide: true,
      });
      children.push(host);
      const hostExit = once(host, "exit");
      let diagnostic = "";
      host.stderr!.on("data", (value: Buffer) => {
        diagnostic += value.toString();
      });
      const observations: Record<string, unknown>[] = [];
      const waiters: {
        match: (value: Record<string, unknown>) => boolean;
        resolve: (value: Record<string, unknown>) => void;
      }[] = [];
      createInterface({ input: host.stdout! }).on("line", (line) => {
        const value = JSON.parse(line) as Record<string, unknown>;
        observations.push(value);
        for (const waiter of [...waiters]) {
          if (!waiter.match(value)) continue;
          waiters.splice(waiters.indexOf(waiter), 1);
          waiter.resolve(value);
        }
      });
      const observed = async (
        match: (value: Record<string, unknown>) => boolean,
      ): Promise<Record<string, unknown>> => {
        const existing = observations.find(match);
        if (existing) return existing;
        return await Promise.race([
          new Promise<Record<string, unknown>>((resolve) => {
            waiters.push({ match, resolve });
          }),
          (async () => {
            await hostExit;
            throw new Error("Host exited before observation: " + diagnostic);
          })(),
        ]);
      };
      await observed((value) => value.ready === true);
      const initial = await observed(
        (value) => value.id === "resident.v1-test" && value.state === "running",
      );
      await assert.rejects(
        execute(join(binaryDirectory, "muon-service-host.exe"), args),
      );
      const security = await execute(join(binaryDirectory, "session-end.exe"), [
        "security",
        endpoint,
      ]);
      assert.equal(security.stdout.trim(), "logon-only");
      const nativeClients = await execute(
        join(binaryDirectory, "transport-test.exe"),
        [endpoint],
      );
      assert(nativeClients.stdout.includes("transport barriers: passed"));
      assert(nativeClients.stdout.includes("native clients: passed"));

      // Hard process death, without a release call, is the owner-loss barrier.
      const page = spawn(
        process.execPath,
        [
          "--input-type=module",
          "-e",
          `
        import {createConnection} from 'node:net';
        import {createInterface} from 'node:readline';
        const socket=createConnection(process.argv[1]);
        socket.on('connect',()=>socket.write(JSON.stringify({requestId:'page',op:'acquire',serviceId:'borrowed'})+'\\n'));
        createInterface({input:socket}).on('line',line=>{if(JSON.parse(line).event!=='ready')process.stdout.write(line+'\\n')});
      `,
          endpoint,
        ],
        { stdio: ["ignore", "pipe", "pipe"], windowsHide: true },
      );
      children.push(page);
      const pageExit = once(page, "exit");
      const [line] = await once(
        createInterface({ input: page.stdout! }),
        "line",
      );
      const pageReference = JSON.parse(line).result;
      assert.equal(pageReference.serviceId, "borrowed");
      page.kill("SIGKILL");
      await pageExit;
      await observed(
        (value) => value.id === "borrowed" && value.state === "stopped",
      );

      const probe = async (): Promise<Record<string, unknown>> => {
        const business = createConnection({
          host: "127.0.0.1",
          port: ports[0]!,
        });
        sockets.push(business);
        await once(business, "connect");
        const response = once(createInterface({ input: business }), "line");
        const nonce = randomUUID();
        business.write(nonce + "\n");
        const [text] = await response;
        business.destroy();
        const value = JSON.parse(text);
        assert.equal(value.nonce, nonce);
        assert.equal(value.pid, host.pid);
        assert.equal(value.generation, initial.generation);
        assert.equal(value.labelHex, Buffer.from("日本語😀").toString("hex"));
        return value;
      };
      const before = mode === "probe" ? await probe() : undefined;

      const socket = createConnection(endpoint);
      sockets.push(socket);
      await once(socket, "connect");
      const replies = createInterface({ input: socket })[
        Symbol.asyncIterator
      ]();
      assert.equal(JSON.parse((await replies.next()).value!).event, "ready");
      let next = 0;
      const request = async (
        value: Record<string, unknown>,
      ): Promise<Record<string, any>> => {
        const requestId = String(++next);
        socket.write(JSON.stringify({ requestId, ...value }) + "\n");
        for (;;) {
          const reply = await replies.next();
          assert(!reply.done, "Connection closed: " + diagnostic);
          const response = JSON.parse(reply.value);
          if (response.requestId === requestId) return response;
        }
      };
      const acquired = (
        await request({ op: "acquire", serviceId: "resident.v1-test" })
      ).result;
      assert.equal(acquired.generation, initial.generation);
      if (mode === "probe")
        assert(Number((await probe()).count) > Number(before!.count));
      await request({ op: "release", ...acquired });
      const state = (
        await request({ op: "getState", serviceId: "resident.v1-test" })
      ).result;
      assert.equal(state.state, "running");
      assert.equal(state.clientReferences, 0);
      assert.equal(state.resident, true);
      assert.equal(
        (await request({ op: "acquire", serviceId: "broken" })).error.code,
        "unavailable",
      );
      assert.equal(
        (await request({ op: "getState", serviceId: "broken" })).result
          .clientReferences,
        0,
      );

      const second = (await request({ op: "acquire", serviceId: "borrowed" }))
        .result;
      assert(BigInt(second.generation) > BigInt(pageReference.generation));
      // A new pipe owner cannot release another owner's reference.
      const stranger = createConnection(endpoint);
      sockets.push(stranger);
      await once(stranger, "connect");
      const strangerReplies = createInterface({ input: stranger })[
        Symbol.asyncIterator
      ]();
      assert.equal(
        JSON.parse((await strangerReplies.next()).value!).event,
        "ready",
      );
      stranger.write(
        JSON.stringify({ requestId: "stolen", op: "release", ...second }) +
          "\n",
      );
      assert.equal(
        JSON.parse((await strangerReplies.next()).value!).requestId,
        "stolen",
      );
      stranger.destroy();
      assert.equal(
        (await request({ op: "getState", serviceId: "borrowed" })).result
          .clientReferences,
        1,
      );
      await request({ op: "release", ...second });
      await observed(
        (value) =>
          value.id === "borrowed" &&
          value.state === "stopped" &&
          value.generation === second.generation,
      );

      // An idle pending read and the pending accept both have to cancel before
      // host exit. The oversized owner is independently disconnected.
      const idle = createConnection(endpoint);
      sockets.push(idle);
      await once(idle, "connect");
      assert.equal(
        JSON.parse((await once(createInterface({ input: idle }), "line"))[0])
          .event,
        "ready",
      );
      const malformed = createConnection(endpoint);
      sockets.push(malformed);
      await once(malformed, "connect");
      assert.equal(
        JSON.parse(
          (await once(createInterface({ input: malformed }), "line"))[0],
        ).event,
        "ready",
      );
      const malformedClose = new Promise<void>((resolve) => {
        malformed.once("close", resolve);
      });
      malformed.on("error", () => {});
      malformed.resume();
      malformed.write("x".repeat(1024 * 1024 + 1));
      await malformedClose;

      const stalled = createConnection(endpoint);
      sockets.push(stalled);
      await once(stalled, "connect");
      const stalledLines = createInterface({ input: stalled });
      assert.equal(
        JSON.parse((await once(stalledLines, "line"))[0]).event,
        "ready",
      );
      stalledLines.pause();
      stalled.pause();
      // Responses exceed the pipe buffer while staying below the host's output
      // queue limit. The subsequent acquire snapshot proves the whole batch
      // was processed, without a sleep to guess whether a write is blocked.
      let batch = "";
      for (let index = 0; index < 4096; ++index)
        batch +=
          JSON.stringify({
            requestId: String(index),
            op: "getState",
            serviceId: "resident.v1-test",
          }) + "\n";
      batch +=
        JSON.stringify({
          requestId: "barrier",
          op: "acquire",
          serviceId: "borrowed",
        }) + "\n";
      stalled.write(batch);
      await observed(
        (value) =>
          value.id === "borrowed" &&
          value.state === "running" &&
          value.generation === String(BigInt(second.generation) + 1n),
      );
      let deadline: ReturnType<typeof setTimeout>;
      const timedOut = new Promise<never>((_, reject) => {
        deadline = setTimeout(
          () =>
            reject(
              new Error(
                "Host did not exit after native stop with blocked output: " +
                  diagnostic,
              ),
            ),
          30000,
        );
      });
      let code: number | null;
      try {
        [code] = await Promise.race([
          (async () => {
            if (shutdown === "command")
              socket.write(
                JSON.stringify({ requestId: "shutdown", op: "shutdown" }) +
                  "\n",
              );
            else
              await execute(join(binaryDirectory, "session-end.exe"), [
                "end",
                endpoint,
              ]);
            return await hostExit;
          })(),
          timedOut,
        ]);
      } finally {
        clearTimeout(deadline!);
      }
      assert.equal(code, 0, diagnostic);
      assert(
        observations.some(
          (value) =>
            value.id === "resident.v1-test" && value.state === "stopped",
        ),
      );
      if (mode === "simple")
        assert(diagnostic.includes("resident.v1-test: stopped"));
      socket.destroy();
      idle.destroy();
      console.log(JSON.stringify({ mode, shutdown, passed: true }));
    }
  }
  for (const resident of [false, true]) {
    for (const action of ["detach", "hold"]) {
      const port = await availablePort();
      const endpoint = String.raw`\\.\pipe\muon-service-${randomUUID()}`;
      const config = join(directory, "launch registry.json");
      await writeFile(
        config,
        JSON.stringify({
          services: {
            worker: {
              library: "../probe-service.dll",
              autostart: resident,
              config: { port: String(port) },
            },
          },
        }),
      );
      const driver = join(binaryDirectory, "process-driver.exe");
      const ui = spawn(
        driver,
        [
          action,
          join(binaryDirectory, "muon-service-host.exe"),
          config,
          endpoint,
          join(directory, "launch data"),
        ],
        { stdio: ["ignore", "pipe", "pipe"], windowsHide: true },
      );
      children.push(ui);
      const uiExit = once(ui, "exit");
      const uiClosed = once(ui, "close");
      let diagnostic = "";
      ui.stderr!.on("data", (data: Buffer) => {
        diagnostic += data.toString();
      });
      const firstLine = once(createInterface({ input: ui.stdout! }), "line");
      const [line] = await Promise.race([
        firstLine,
        (async () => {
          await uiClosed;
          throw new Error("UI launcher exited before readiness: " + diagnostic);
        })(),
      ]);
      const launch = JSON.parse(line);
      assert.equal(launch.launched, true);
      const observer = spawn(driver, ["wait", String(launch.pid)], {
        stdio: ["ignore", "pipe", "pipe"],
        windowsHide: true,
      });
      children.push(observer);
      const hostExit = once(observer, "exit");
      if (action === "hold") {
        const [waiting] = await once(
          createInterface({ input: observer.stdout! }),
          "line",
        );
        assert.equal(JSON.parse(waiting).waiting, true);
        ui.kill("SIGKILL");
      }
      const [uiCode] = await uiExit;
      if (action === "detach") assert.equal(uiCode, 0, diagnostic);
      if (resident) {
        const socket = createConnection(endpoint);
        sockets.push(socket);
        await once(socket, "connect");
        const replies = createInterface({ input: socket })[
          Symbol.asyncIterator
        ]();
        assert.equal(JSON.parse((await replies.next()).value!).event, "ready");
        socket.write(
          '{"requestId":"acquire","op":"acquire","serviceId":"worker"}\n',
        );
        const reference = JSON.parse((await replies.next()).value!).result;
        assert.equal(reference.serviceId, "worker");
        const business = createConnection({ host: "127.0.0.1", port });
        sockets.push(business);
        await once(business, "connect");
        const nonce = randomUUID();
        const response = once(createInterface({ input: business }), "line");
        business.write(nonce + "\n");
        const value = JSON.parse((await response)[0]);
        assert.equal(value.nonce, nonce);
        assert.equal(value.pid, launch.pid);
        assert.equal(value.generation, reference.generation);
        business.destroy();
        socket.write('{"requestId":"exit","op":"shutdown"}\n');
      }
      const [hostCode] = await hostExit;
      assert.equal(hostCode, 0);
      console.log(
        JSON.stringify({ bootstrap: action, resident, passed: true }),
      );
    }
  }
  const sessionPort = await availablePort();
  const sessionConfig = join(directory, "session registry.json");
  await writeFile(
    sessionConfig,
    JSON.stringify({
      services: {
        worker: {
          library: "../probe-service.dll",
          autostart: true,
          config: { port: String(sessionPort) },
        },
      },
    }),
  );
  const session = await execute(join(binaryDirectory, "process-driver.exe"), [
    "session",
    join(binaryDirectory, "muon-service-host.exe"),
    sessionConfig,
    String.raw`\\.\pipe\muon-service-${randomUUID()}`,
    join(directory, "session data"),
  ]);
  assert(session.stdout.includes('"session":true'));
  console.log(JSON.stringify({ session: true, passed: true }));
  const extendedArguments = await execute(
    join(binaryDirectory, "process-driver.exe"),
    [
      "session",
      "\\\\?\\" + join(binaryDirectory, "muon-service-host.exe"),
      "\\\\?\\" + sessionConfig,
      String.raw`\\.\pipe\muon-service-${randomUUID()}`,
      "\\\\?\\" + join(directory, "extended data"),
    ],
  );
  assert(extendedArguments.stdout.includes('"session":true'));
  await writeFile(
    sessionConfig,
    JSON.stringify({
      services: {
        worker: {
          library: "\\\\?\\" + join(binaryDirectory, "probe-service.dll"),
          autostart: true,
          config: { port: String(sessionPort) },
        },
      },
    }),
  );
  const extendedLibrary = await execute(
    join(binaryDirectory, "process-driver.exe"),
    [
      "session",
      join(binaryDirectory, "muon-service-host.exe"),
      sessionConfig,
      String.raw`\\.\pipe\muon-service-${randomUUID()}`,
      join(directory, "extended library data"),
    ],
  );
  assert(extendedLibrary.stdout.includes('"session":true'));

  const policyConfig = join(directory, "policy-services.json");
  const policyRoot = join(directory, "policy data");
  const policyEndpoint = String.raw`\\.\pipe\muon-service-${randomUUID()}`;
  await writeFile(
    policyConfig,
    JSON.stringify({
      services: {
        worker: { library: "../simple-service.dll", autostart: true },
        second: { library: "../simple-service.dll", autostart: false },
      },
    }),
  );
  const policyArguments = [
    "--config",
    "\\\\?\\" + policyConfig,
    "--endpoint",
    policyEndpoint,
    "--data-root",
    "\\\\?\\" + policyRoot,
  ];
  const startPolicyHost = async () => {
    const host = spawn(
      join(binaryDirectory, "muon-service-host.exe"),
      policyArguments,
      { stdio: ["ignore", "pipe", "pipe"], windowsHide: true },
    );
    children.push(host);
    const exited = once(host, "exit");
    let diagnostic = "";
    host.stderr.on("data", (bytes: Buffer) => {
      diagnostic += bytes.toString();
    });
    for await (const line of createInterface({ input: host.stdout })) {
      if (JSON.parse(line).ready) break;
    }
    assert.equal(host.exitCode, null, diagnostic);
    const socket = createConnection(policyEndpoint);
    sockets.push(socket);
    const callbacks = new Map<
      string,
      (response: Record<string, any>) => void
    >();
    const ready = new Promise<void>((resolve) => {
      createInterface({ input: socket }).on("line", (line) => {
        const response = JSON.parse(line);
        if (response.event === "ready") {
          resolve();
          return;
        }
        const callback = callbacks.get(response.requestId);
        callbacks.delete(response.requestId);
        callback?.(response);
      });
    });
    await ready;
    let sequence = 0;
    const request = async (
      value: Record<string, unknown>,
    ): Promise<Record<string, any>> => {
      const requestId = String(++sequence);
      const pending = new Promise<Record<string, any>>((resolve) => {
        callbacks.set(requestId, resolve);
      });
      socket.write(JSON.stringify({ ...value, requestId }) + "\n");
      return await pending;
    };
    const shutdown = () => {
      socket.write(
        JSON.stringify({ op: "shutdown", requestId: "exit" }) + "\n",
      );
    };
    return { host, exited, request, shutdown };
  };
  const firstPolicy = await startPolicyHost();
  const acquired = await firstPolicy.request({
    op: "acquire",
    serviceId: "worker",
  });
  assert(acquired.result, JSON.stringify(acquired));
  const watching = await firstPolicy.request({
    op: "watchState",
    serviceId: "worker",
    operation: "acquire",
  });
  assert(watching.result, JSON.stringify(watching));
  const token = watching.result.token;
  const changed = firstPolicy.request({
    op: "watchState",
    serviceId: "worker",
    operation: "next",
    token,
  });
  const updates = await Promise.all([
    firstPolicy.request({
      op: "setAutostart",
      serviceId: "worker",
      enabled: false,
    }),
    firstPolicy.request({
      op: "setAutostart",
      serviceId: "second",
      enabled: true,
    }),
  ]);
  for (const response of updates)
    assert.equal(response.result, null, JSON.stringify(response));
  const observation = (await changed).result.state;
  assert.equal(observation.autostart, false);
  assert.equal(observation.clientReferences, 1);
  assert.equal(observation.state, "running");
  await firstPolicy.request({ op: "release", ...acquired.result });
  for (;;) {
    const response = await firstPolicy.request({
      op: "watchState",
      serviceId: "worker",
      operation: "next",
      token,
    });
    assert(response.result, JSON.stringify(response));
    if (response.result.state.state === "stopped") {
      assert.equal(response.result.state.clientReferences, 0);
      break;
    }
  }
  const pendingWatch = firstPolicy.request({
    op: "watchState",
    serviceId: "worker",
    operation: "next",
    token,
  });
  await firstPolicy.request({
    op: "watchState",
    serviceId: "worker",
    operation: "release",
    token,
  });
  assert.equal((await pendingWatch).result.state, null);
  await rename(join(policyRoot, ".autostart"), join(policyRoot, ".backup"));
  await writeFile(join(policyRoot, ".autostart"), "blocked");
  const deniedPolicy = await firstPolicy.request({
    op: "setAutostart",
    serviceId: "worker",
    enabled: true,
  });
  assert.equal(deniedPolicy.error.code, "persistence-failed");
  assert.equal(
    (await firstPolicy.request({ op: "getState", serviceId: "worker" })).result
      .autostart,
    false,
  );
  await rm(join(policyRoot, ".autostart"));
  await rename(join(policyRoot, ".backup"), join(policyRoot, ".autostart"));
  firstPolicy.host.kill("SIGKILL");
  await firstPolicy.exited;
  const restoredPolicy = await startPolicyHost();
  const off = (
    await restoredPolicy.request({ op: "getState", serviceId: "worker" })
  ).result;
  assert.equal(off.state, "stopped");
  assert.equal(off.generation, "0");
  assert.equal(off.autostart, false);
  const secondPolicy = await restoredPolicy.request({
    op: "acquire",
    serviceId: "second",
  });
  assert(secondPolicy.result, JSON.stringify(secondPolicy));
  await restoredPolicy.request({ op: "release", ...secondPolicy.result });
  await restoredPolicy.request({
    op: "setAutostart",
    serviceId: "second",
    enabled: true,
  });
  const on = (
    await restoredPolicy.request({ op: "getState", serviceId: "second" })
  ).result;
  assert.equal(on.generation, secondPolicy.result.generation);
  assert.equal(on.clientReferences, 0);
  assert.equal(on.resident, true);
  // Shutdown acknowledgement is best effort; process exit confirms disposal.
  restoredPolicy.shutdown();
  assert.equal((await restoredPolicy.exited)[0], 0);
  await writeFile(join(policyRoot, ".autostart", "worker"), "corrupt");
  await assert.rejects(
    execute(join(binaryDirectory, "muon-service-host.exe"), policyArguments, {
      timeout: 10000,
    }),
    /Invalid service autostart preference/,
  );
  console.log(JSON.stringify({ policy: true, passed: true }));
} finally {
  for (const socket of sockets) socket.destroy();
  for (const child of children) {
    if (child.exitCode !== null || child.signalCode !== null) continue;
    const exit = once(child, "exit");
    child.kill("SIGKILL");
    await exit;
  }
  await rm(directory, { recursive: true, force: true });
}

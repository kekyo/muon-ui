import { spawn } from "node:child_process";
import { copyFile, mkdir, mkdtemp, rm, writeFile } from "node:fs/promises";
import { join } from "node:path";
import { tmpdir } from "node:os";

const commandArgs = process.argv.slice(2);
if (commandArgs.length === 0) {
  throw new Error("Usage: run-dbus-session-command.mjs <command> [args...]");
}

const runProcess = async (command, args, options) =>
  await new Promise((resolvePromise, reject) => {
    const child = spawn(command, args, options);
    child.once("error", reject);
    child.once("exit", (code, signal) => {
      if (signal !== null) {
        reject(new Error(`${command} exited with signal ${signal}`));
        return;
      }
      resolvePromise(code ?? 0);
    });
  });

const dataDirectory = await mkdtemp(join(tmpdir(), "muon-dbus-session-"));
try {
  const serviceDirectory = join(dataDirectory, "dbus-1", "services");
  const configPath = join(dataDirectory, "session.conf");
  await mkdir(serviceDirectory, { recursive: true });
  await copyFile(
    "/usr/share/dbus-1/services/org.a11y.Bus.service",
    join(serviceDirectory, "org.a11y.Bus.service"),
  );
  await writeFile(
    configPath,
    `<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>session</type>
  <keep_umask/>
  <listen>unix:tmpdir=/tmp</listen>
  <auth>EXTERNAL</auth>
  <servicedir>${serviceDirectory}</servicedir>
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
  <limit name="service_start_timeout">2000</limit>
  <limit name="auth_timeout">240000</limit>
  <limit name="pending_fd_timeout">150000</limit>
  <limit name="max_incoming_bytes">1000000000</limit>
  <limit name="max_incoming_unix_fds">250000000</limit>
  <limit name="max_outgoing_bytes">1000000000</limit>
  <limit name="max_outgoing_unix_fds">250000000</limit>
  <limit name="max_message_size">1000000000</limit>
  <limit name="max_completed_connections">100000</limit>
  <limit name="max_incomplete_connections">10000</limit>
  <limit name="max_connections_per_user">100000</limit>
  <limit name="max_pending_service_starts">10000</limit>
  <limit name="max_names_per_connection">50000</limit>
  <limit name="max_match_rules_per_connection">50000</limit>
  <limit name="max_replies_per_connection">50000</limit>
</busconfig>
`,
    "utf8",
  );

  const [command, ...args] = commandArgs;
  const environment = {
    ...process.env,
  };
  delete environment.AT_SPI_BUS_ADDRESS;
  delete environment.GNOME_KEYRING_CONTROL;
  delete environment.NO_AT_BRIDGE;
  delete environment.SSH_AUTH_SOCK;

  process.exitCode = await runProcess(
    "dbus-run-session",
    [`--config-file=${configPath}`, "--", command, ...args],
    {
      env: environment,
      stdio: "inherit",
    },
  );
} finally {
  await rm(dataDirectory, { recursive: true, force: true });
}

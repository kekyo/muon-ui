// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { spawn } from 'node:child_process';

/**
 * Runs a build tool without a shell and awaits process completion.
 * @param command - Executable path.
 * @param args - Literal argument vector.
 * @param cwd - Working directory.
 * @param environment - Child environment, including any external credentials.
 * @param output - Optional progress sink; undefined captures output only.
 * @returns Captured standard output and standard error.
 */
export const runAndroidCommand = async (
  command: string,
  args: readonly string[],
  cwd: string,
  environment: NodeJS.ProcessEnv,
  output: ((text: string) => void) | undefined
): Promise<string> =>
  await new Promise<string>((resolve, reject) => {
    const child = spawn(command, args, {
      cwd,
      env: environment,
      stdio: ['ignore', 'pipe', 'pipe'],
    });
    let captured = '';
    const receive = (chunk: Buffer) => {
      const text = chunk.toString();
      captured += text;
      output?.(text);
    };
    child.stdout.on('data', receive);
    child.stderr.on('data', receive);
    child.once('error', reject);
    child.once('close', (code, signal) => {
      if (code === 0) resolve(captured);
      else
        reject(
          new Error(`${command} failed (${signal ?? code}):\n${captured}`)
        );
    });
  });

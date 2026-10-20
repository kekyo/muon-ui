// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFile } from 'node:child_process';
import { mkdtemp, mkdir, readFile, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { promisify } from 'node:util';
import { expect, it } from 'vitest';

const execute = promisify(execFile);
const script = fileURLToPath(
  new URL('./scripts/prepare-notification-permission-test.mjs', import.meta.url)
);

it.each([
  { api: 24, installed: true, mutations: 0 },
  { api: 37, installed: false, mutations: 0 },
  { api: 37, installed: true, mutations: 3 },
])(
  'prepares only the selected test package on API $api, installed=$installed',
  async (fixture) => {
    const root = await mkdtemp(join(tmpdir(), 'muon-permission-reset-'));
    try {
      const bin = join(root, 'platform-tools');
      const log = join(root, 'calls.jsonl');
      await mkdir(bin);
      await writeFile(
        join(bin, 'adb'),
        `#!/usr/bin/env node
import { appendFileSync } from 'node:fs';
const args=process.argv.slice(2);
appendFileSync(${JSON.stringify(log)}, JSON.stringify(args)+'\\n');
const command=args.slice(2).join(' ');
if(command==='shell getprop ro.build.version.sdk') console.log(${fixture.api});
if(command==='shell pm list packages --user current dev.muon.testhost') console.log(${JSON.stringify(fixture.installed ? 'package:dev.muon.testhost\npackage:dev.muon.testhost.test' : 'package:dev.muon.testhost.test')});
`,
        { mode: 0o755 }
      );
      await execute(process.execPath, [script], {
        env: {
          ...process.env,
          ANDROID_SDK_ROOT: root,
          ANDROID_SERIAL: 'selected-device',
        },
      });
      const calls = (await readFile(log, 'utf8'))
        .trim()
        .split('\n')
        .map((line) => JSON.parse(line) as string[]);
      expect(
        calls.every((args) => args[0] === '-s' && args[1] === 'selected-device')
      ).toBe(true);
      const mutations = calls.filter(
        (args) =>
          args[3] === 'pm' &&
          ['revoke', 'clear-permission-flags'].includes(args[4]!)
      );
      expect(mutations).toHaveLength(fixture.mutations);
      if (fixture.mutations > 0) {
        expect(mutations.map((args) => args.slice(4))).toEqual([
          [
            'revoke',
            'dev.muon.testhost',
            'android.permission.POST_NOTIFICATIONS',
          ],
          [
            'clear-permission-flags',
            'dev.muon.testhost',
            'android.permission.POST_NOTIFICATIONS',
            'user-set',
          ],
          [
            'clear-permission-flags',
            'dev.muon.testhost',
            'android.permission.POST_NOTIFICATIONS',
            'user-fixed',
          ],
        ]);
      }
    } finally {
      await rm(root, { recursive: true, force: true });
    }
  }
);

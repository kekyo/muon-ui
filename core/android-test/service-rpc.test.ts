// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFile, spawn } from 'node:child_process';
import { randomUUID } from 'node:crypto';
import { EventEmitter, on, once } from 'node:events';
import { mkdtemp } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { createInterface } from 'node:readline';
import { promisify } from 'node:util';
import { Client } from '@modelcontextprotocol/sdk/client/index.js';
import { InMemoryTransport } from '@modelcontextprotocol/sdk/inMemory.js';
import { createConnection } from '@playwright/mcp';
import { expect, it } from 'vitest';

const execute = promisify(execFile);
const enabled = process.env.MUON_ANDROID_SERVICE_RPC_TEST === '1';
it.skipIf(!enabled).each(['alpha.v1-test', 'quickjs'])(
  'isolates %s page RPC owners over the Android service FD transport',
  async (serviceId) => {
    const serial = process.env.ANDROID_SERIAL;
    if (!serial) throw new Error('ANDROID_SERIAL is required');
    const adb = async (args: string[]) =>
      (await execute('adb', ['-s', serial, ...args])).stdout;
    const app = 'dev.muon.servicefixture';
    const directory = await mkdtemp(join(tmpdir(), 'muon-android-services-'));
    const events = new EventEmitter();
    const logcat = spawn('adb', [
      '-s',
      serial,
      'logcat',
      '-v',
      'brief',
      '-T',
      '1',
      'MuonServiceRpcReady:I',
      '*:S',
    ]);
    const lines = createInterface({ input: logcat.stdout });
    lines.on('line', (line) => events.emit('ready', line));
    let port: string | undefined;
    let client: Client | undefined;
    let closeMcp: (() => Promise<void>) | undefined;
    const start = async () => {
      const label = randomUUID();
      const abort = new AbortController();
      const ready = (async () => {
        const signal = AbortSignal.any([
          AbortSignal.timeout(30_000),
          abort.signal,
        ]);
        for await (const [line] of on(events, 'ready', { signal })) {
          if (String(line).includes(label)) return;
        }
      })();
      try {
        const [result] = await Promise.all([
          adb([
            'shell',
            'am',
            'start',
            '-W',
            '-f',
            '0x18000000',
            '-n',
            app + '/dev.muon.runtime.MuonServiceRpcActivity',
            '--es',
            'owner',
            label,
          ]),
          ready,
        ]);
        expect(result).not.toMatch(/Error|does not exist/u);
        return label;
      } finally {
        abort.abort();
        await Promise.allSettled([ready]);
      }
    };
    const tool = async (name: string, args: Record<string, unknown>) => {
      const result = await client!.callTool({ name, arguments: args });
      const text = (result.content as { type: string; text?: string }[])
        .filter((x) => x.type === 'text')
        .map((x) => x.text ?? '')
        .join('\n');
      if (result.isError) throw new Error(text);
      return text;
    };
    const evaluate = async <T>(source: string): Promise<T> => {
      let text: string;
      try {
        text = await tool('browser_evaluate', {
          function: `async () => { ${source} }`,
        });
      } catch (cause) {
        throw new Error(`Service page evaluation failed: ${source}`, { cause });
      }
      const value = /### Result\n([\s\S]*?)(?:\n### |$)/u.exec(text)?.[1];
      if (!value) throw new Error(text);
      return JSON.parse(value) as T;
    };
    const select = async (label: string) => {
      const tabs = await tool('browser_tabs', { action: 'list' });
      const line = tabs.split('\n').find((line) => line.includes(label));
      const index = line && /^- (\d+):/u.exec(line)?.[1];
      if (!index) throw new Error(tabs);
      await tool('browser_tabs', { action: 'select', index: Number(index) });
    };
    try {
      await adb(['shell', 'am', 'force-stop', app]);
      await adb([
        'shell',
        'run-as',
        app,
        'rm',
        '-rf',
        'files/service-autostart',
      ]);
      const first = await start();
      const pid = (await adb(['shell', 'pidof', app])).trim();
      port = (
        await adb([
          'forward',
          'tcp:0',
          'localabstract:webview_devtools_remote_' + pid,
        ])
      ).trim();
      const server = await createConnection({
        browser: { cdpEndpoint: 'http://127.0.0.1:' + port },
        outputDir: directory,
      });
      closeMcp = async () => await server.close();
      const [a, b] = InMemoryTransport.createLinkedPair();
      client = new Client({ name: 'muon-android-services', version: '1' });
      await server.connect(b);
      await client.connect(a);
      await select(first);
      expect(
        await evaluate(`globalThis.handle = await muon.services.acquire('${serviceId}');
      const value = 'Android\\0日本語😀';
      return await handle.call('echo', [value]);`)
      ).toBe('Android\0日本語😀');
      expect(
        await evaluate(`const bytes = new Uint8Array(1024 * 1024); bytes[0]=37; bytes[bytes.length-1]=91;
      let result;
      try { result = await handle.call('echoBytes',[bytes]); }
      catch(error) { throw new Error(JSON.stringify({error:String(error),state:await muon.services.getState('${serviceId}')})); }
      return [result.length,result[0],result[result.length-1],
        await handle.call('echoInt64',[-9223372036854775808n]),
        await handle.call('echoUint64',[18446744073709551615n])].map(x=>String(x));`)
      ).toEqual([
        '1048576',
        '37',
        '91',
        '-9223372036854775808',
        '18446744073709551615',
      ]);
      expect(
        await evaluate(
          `return await handle.call('callback',['outer',async value => await handle.call('echo',[value + ':nested'])]);`
        )
      ).toBe('outer:nested');
      if (serviceId === 'quickjs') {
        expect(
          await evaluate(`const broken = await muon.services.acquire('quickjs-fault');
          let failure=false; try { await broken.call('spin',[]); } catch { failure=true; }
          let changed; const failed=new Promise(resolve=>changed=resolve);
          const watch=await muon.services.watchState('quickjs-fault',state=>{if(state.state==='failed') changed();});
          await failed; await watch.close(); await broken.release();
          const state=await muon.services.getState('quickjs-fault');
          const native=await muon.services.acquire('alpha.v1-test');
          const value=await native.call('echo',['native-after-fault']); await native.release();
          return [failure,state.state,state.clientReferences,state.resident,await handle.call('echo',['other-heap']),value];`)
        ).toEqual([
          true,
          'failed',
          0,
          true,
          'other-heap',
          'native-after-fault',
        ]);
      }
      expect(
        await evaluate(`const second = await muon.services.acquire('${serviceId}'); await second.release();
      const abort = new AbortController(); let entered;
      const ready = new Promise(resolve=>entered=resolve);
      const pending = handle.call('callback',['cancel',()=>{ entered(); return new Promise(()=>{}); }],{signal:abort.signal});
      const result = (async()=>{try { await pending; return 'unexpected'; } catch(error) { return error.code; }})();
      await ready; abort.abort(); return await result;`)
      ).toBe('cancelled');
      await evaluate(`let entered; const ready = new Promise(resolve=>entered=resolve);
      globalThis.pending = (async()=>{try { return await handle.call('callback',['first',()=>{entered(); return new Promise(()=>{});}]); } catch(error) { return error.code; }})();
      await ready; return true;`);
      const second = await start();
      await select(second);
      await evaluate(`globalThis.handle = await muon.services.acquire('${serviceId}');
      let entered; const ready = new Promise(resolve=>entered=resolve);
      globalThis.pending = handle.call('callback',['second',value=>{entered(); return new Promise(resolve=>globalThis.finishCallback=()=>resolve(value+':alive'));}]);
      globalThis.received = new Promise(resolve=>globalThis.receive=resolve);
      globalThis.subscription = await handle.subscribe('progress',value=>receive(value));
      await ready; return true;`);
      await select(first);
      // Navigation destroys the native document owner but leaves the Activity and
      // the second WebView alive. No force-stop or service-process kill is involved.
      await tool('browser_navigate', {
        url: 'https://main.asset.muon.invalid/empty.html',
      });
      await select(second);
      expect(
        await evaluate(`let changed;
      const ownerGone = new Promise(resolve=>changed=resolve);
      const watch = await muon.services.watchState('${serviceId}',state=>{if(state.clientReferences===1) changed();});
      await ownerGone; await watch.close(); finishCallback();
      await handle.call('emit',[41]);
      return [await pending,await received,await handle.call('echo',['still-running'])];`)
      ).toEqual(['second:alive', 41, 'still-running']);
      expect(
        await evaluate(`await subscription.close(); await handle.release();
      const state = await muon.services.getState('${serviceId}');
      return [state.state,state.clientReferences];`)
      ).toEqual(['running', 0]);
      await evaluate(`globalThis.handle = await muon.services.acquire('${serviceId}');
        let entered; const ready = new Promise(resolve=>entered=resolve);
        globalThis.pending = (async()=>{try { await handle.call('callback',['disconnect',()=>{entered(); return new Promise(()=>{});}]); return false; } catch { return true; }})();
        await ready; return true;`);
      const hostPid = (
        await adb(['shell', 'pidof', app + ':muon_services'])
      ).trim();
      expect(hostPid).toMatch(/^\d+$/u);
      await adb(['shell', 'run-as', app, 'kill', '-9', hostPid]);
      expect(await evaluate(`return await pending;`)).toBe(true);
      expect(
        await evaluate(
          `try { await muon.services.acquire('${serviceId}'); return false; } catch { return true; }`
        )
      ).toBe(true);
      const replacement = await start();
      await select(replacement);
      expect(
        await evaluate(
          `globalThis.handle = await muon.services.acquire('${serviceId}'); return await handle.call('echo',['replacement']);`
        )
      ).toBe('replacement');
      await select(second);
      expect(
        await evaluate(
          `try { await handle.release(); return false; } catch { return true; }`
        )
      ).toBe(true);
      await select(replacement);
      expect(
        await evaluate(
          `return [await handle.call('echo',['unaffected']), (await muon.services.getState('${serviceId}')).clientReferences];`
        )
      ).toEqual(['unaffected', 1]);
      await evaluate(`await handle.release(); return true;`);
      expect(
        await evaluate(`await muon.services.setAutostart('${serviceId}',false);
        let stopped; const done = new Promise(resolve=>stopped=resolve);
        const watch = await muon.services.watchState('${serviceId}',state=>{if(state.state==='stopped') stopped();});
        await done; await watch.close(); return (await muon.services.getState('${serviceId}')).state;`)
      ).toBe('stopped');
      if (serviceId === 'quickjs') {
        expect(
          await evaluate(`const native = await muon.services.acquire('alpha.v1-test');
          const value = await native.call('echo',['native-after-quickjs-stop']); await native.release(); return value;`)
        ).toBe('native-after-quickjs-stop');
      }
    } finally {
      const results = await Promise.allSettled([client?.close(), closeMcp?.()]);
      lines.close();
      const exited = once(logcat, 'exit');
      logcat.kill();
      await exited;
      if (port) await adb(['forward', '--remove', 'tcp:' + port]);
      await adb(['shell', 'am', 'force-stop', app]);
      for (const result of results)
        if (result.status === 'rejected') throw result.reason;
    }
  },
  120_000
);

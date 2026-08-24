/* Packaged ES module used to verify the Android QuickJS sidecar. */

import path, { basename } from 'node:path';
import pathAlias from 'path';
import EventEmitter, { once as onceEvent } from 'node:events';
import { Buffer } from 'node:buffer';
import {
  clearImmediate,
  clearInterval,
  clearTimeout,
  setImmediate,
  setInterval,
  setTimeout,
} from 'node:timers';
import { setTimeout as setPromiseTimeout } from 'node:timers/promises';
import processModule, {
  arch as processArch,
  cwd as processCwd,
  hrtime as processHrtime,
  platform as processPlatform,
  uptime as processUptime,
} from 'node:process';
import processAlias from 'process';
import os, { arch as osArch, platform as osPlatform } from 'node:os';
import osAlias from 'os';
import util, {
  format as formatValue,
  inspect as inspectValue,
  isDeepStrictEqual,
  promisify,
  stripVTControlCharacters,
} from 'node:util';
import utilAlias from 'util';
import assert from 'node:assert';
import assertAlias from 'assert';
import strictAssert from 'node:assert/strict';
import querystring from 'node:querystring';
import querystringAlias from 'querystring';
import stringDecoderModule, { StringDecoder } from 'node:string_decoder';
import stringDecoderAlias from 'string_decoder';
import {
  PassThrough,
  Readable,
  Transform,
  Writable,
  isDestroyed,
  isReadable,
  isWritable,
} from 'node:stream';
import {
  finished as finishedStream,
  pipeline as pipelineStreams,
} from 'node:stream/promises';
import {
  URL,
  URLSearchParams,
  fileURLToPath,
  pathToFileURL,
  urlToHttpOptions,
} from 'node:url';
import net, {
  Server as NetServer,
  Socket,
  createConnection,
  createServer as createNetServer,
} from 'node:net';
import netAlias from 'net';
import dns from 'node:dns';
import dnsAlias from 'dns';
import dnsPromises from 'node:dns/promises';
import http, {
  ClientRequest,
  IncomingMessage,
  Server as HttpServer,
  ServerResponse,
  createServer as createHttpServer,
} from 'node:http';
import httpAlias from 'http';
import https from 'node:https';
import httpsAlias from 'https';

let counter = 0;
let retainedTcpSocket = null;

export const answer = 42;

export const increment = () => {
  counter += 1;
  return counter;
};

export const echo = async (value) => value;

export const invokeCallback = async (value, callback) => await callback(value);

export const importedPathBasename = (value) => {
  if (path !== pathAlias || basename(value) !== pathAlias.basename(value)) {
    throw new Error(
      'The packaged module did not receive one shared path module'
    );
  }
  return basename(value);
};

export const exerciseRuntimePrimitives = async () => {
  const eventValues = [];
  const emitter = new EventEmitter();
  const persistentListener = function (value) {
    eventValues.push(`on:${value}:${this === emitter}`);
  };
  emitter.prependListener('data', (value) => eventValues.push(`pre:${value}`));
  emitter.on('data', persistentListener);
  emitter.once('data', (value) => eventValues.push(`once:${value}`));
  const firstEmit = emitter.emit('data', 'first');
  emitter.removeListener('data', persistentListener);
  const secondEmit = emitter.emit('data', 'second');
  emitter.removeAllListeners('data');
  const emptyEmit = emitter.emit('data', 'third');

  const pendingEvent = onceEvent(emitter, 'ready');
  setTimeout((value) => emitter.emit('ready', value), 2, 'event-ready');
  const [awaitedEvent] = await pendingEvent;

  const buffer = Buffer.concat([
    Buffer.from('muon✓'),
    Buffer.from('21', 'hex'),
  ]);

  let cancelledTimeoutCalled = false;
  const cancelledTimeout = setTimeout(() => {
    cancelledTimeoutCalled = true;
  }, 2);
  clearTimeout(cancelledTimeout);
  const timeoutValue = await new Promise((resolve) => {
    setTimeout(resolve, 2, 'timeout-ready');
  });

  let cancelledImmediateCalled = false;
  const cancelledImmediate = setImmediate(() => {
    cancelledImmediateCalled = true;
  });
  clearImmediate(cancelledImmediate);
  const immediateValue = await new Promise((resolve) => {
    setImmediate(resolve, 'immediate-ready');
  });

  const intervalCount = await new Promise((resolve) => {
    let count = 0;
    const interval = setInterval(() => {
      count += 1;
      if (count === 2) {
        clearInterval(interval);
        resolve(count);
      }
    }, 2);
  });

  const zeroDelayIntervalCount = await new Promise((resolve) => {
    let count = 0;
    const interval = setInterval(async () => {
      count += 1;
      if (count === 100) {
        clearInterval(interval);
        resolve(count);
        return;
      }
      await Promise.resolve();
      clearInterval(interval);
      resolve(count);
    }, 0);
  });

  const referenceTimer = setTimeout(() => {}, 50);
  const initiallyReferenced = referenceTimer.hasRef();
  referenceTimer.unref();
  const referencedAfterUnref = referenceTimer.hasRef();
  referenceTimer.ref();
  const referencedAfterRef = referenceTimer.hasRef();
  clearTimeout(referenceTimer);

  const abortController = new AbortController();
  const abortedTimer = setPromiseTimeout(1000, 'late', {
    signal: abortController.signal,
  });
  setTimeout(() => abortController.abort(), 2);
  let abortName = '';
  let abortCode = '';
  try {
    await abortedTimer;
  } catch (error) {
    abortName = error.name;
    abortCode = error.code;
  }

  const staticSignal = AbortSignal.abort('static-reason');
  let staticReason = '';
  try {
    staticSignal.throwIfAborted();
  } catch (error) {
    staticReason = String(error);
  }

  const combinedController = new AbortController();
  const combinedSignal = AbortSignal.any([combinedController.signal]);
  combinedController.abort('combined-reason');

  const timeoutSignal = AbortSignal.timeout(2);
  const timeoutReasonName = await new Promise((resolve) => {
    timeoutSignal.addEventListener(
      'abort',
      () => resolve(timeoutSignal.reason.name),
      { once: true }
    );
  });

  await setPromiseTimeout(5);
  return {
    events: {
      values: eventValues,
      firstEmit,
      secondEmit,
      emptyEmit,
      awaitedEvent,
    },
    buffer: {
      text: buffer.toString(),
      hex: buffer.toString('hex'),
      base64: buffer.toString('base64'),
      byteLength: Buffer.byteLength('muon✓'),
      isBuffer: Buffer.isBuffer(buffer),
      isUint8Array: buffer instanceof Uint8Array,
      equalsCopy: buffer.equals(Buffer.from(buffer)),
    },
    timers: {
      timeoutValue,
      immediateValue,
      intervalCount,
      zeroDelayIntervalCount,
      cancelledTimeoutCalled,
      cancelledImmediateCalled,
      initiallyReferenced,
      referencedAfterUnref,
      referencedAfterRef,
    },
    abort: {
      abortName,
      abortCode,
      staticReason,
      combinedReason: combinedSignal.reason,
      timeoutReasonName,
    },
  };
};

export const exerciseProcessAndOs = async () => {
  let emittedValue = '';
  processModule.once('muon-test', (value) => {
    emittedValue = value;
  });
  processModule.emit('muon-test', 'event-ready');

  processModule.env.MUON_TEST_VALUE = 42;
  const environmentValue = processModule.env.MUON_TEST_VALUE;
  delete processModule.env.MUON_TEST_VALUE;

  const startUptime = processUptime();
  const startTime = processHrtime();
  const startBigint = processHrtime.bigint();
  await setPromiseTimeout(5);
  const elapsed = processHrtime(startTime);
  const elapsedNanoseconds = elapsed[0] * 1_000_000_000 + elapsed[1];

  return {
    process: {
      moduleAlias: processModule === processAlias,
      globalAlias: processModule === globalThis.process,
      isEventEmitter: processModule instanceof EventEmitter,
      emittedValue,
      arch: processArch,
      platform: processPlatform,
      cwd: processCwd(),
      argv: processModule.argv,
      execArgv: processModule.execArgv,
      pidIsPositiveInteger:
        Number.isInteger(processModule.pid) && processModule.pid > 0,
      version: processModule.version,
      quickjsVersion: processModule.versions.quickjs,
      releaseName: processModule.release.name,
      environmentValue,
      environmentDeleted: !('MUON_TEST_VALUE' in processModule.env),
      uptimeIncreased: processUptime() > startUptime,
      elapsedNanoseconds,
      bigintIncreased: processHrtime.bigint() > startBigint,
    },
    os: {
      moduleAlias: os === osAlias,
      arch: osArch(),
      platform: osPlatform(),
      type: os.type(),
      endianness: os.endianness(),
      eol: os.EOL,
      devNull: os.devNull,
      homedir: os.homedir(),
      tmpdir: os.tmpdir(),
      userInfo: os.userInfo(),
    },
  };
};

export const exerciseUtilityModules = async () => {
  const add = promisify((left, right, callback) => {
    setImmediate(callback, null, left + right);
  });
  const promisifiedValue = await add(20, 22);

  const fail = promisify((callback) => {
    const error = new Error('promisified failure');
    error.code = 'EUTIL';
    callback(error);
  });
  let promisifiedErrorCode = '';
  try {
    await fail();
  } catch (error) {
    promisifiedErrorCode = error.code;
  }

  const customSource = (callback) => callback(null, 'source');
  const customPromisified = async () => 'custom';
  customSource[promisify.custom] = customPromisified;

  const customInspectable = { hidden: true };
  customInspectable[inspectValue.custom] = () => 'MuonCustom';
  const circularLeft = { name: 'muon' };
  circularLeft.self = circularLeft;
  const circularRight = { name: 'muon' };
  circularRight.self = circularRight;

  assert.equal(1, '1');
  strictAssert.ok(true);
  strictAssert.strictEqual(42, 42);
  strictAssert.deepStrictEqual(
    { value: [1, Buffer.from('muon')] },
    { value: [1, Buffer.from('muon')] }
  );
  strictAssert.match('muon-runtime', /^muon/);
  strictAssert.throws(() => {
    throw new TypeError('expected sync error');
  }, TypeError);
  await strictAssert.rejects(async () => {
    throw new Error('expected async error');
  }, /expected async/);

  let assertionFailure;
  try {
    strictAssert.strictEqual(1, 2, 'different values');
  } catch (error) {
    assertionFailure = {
      isAssertionError: error instanceof strictAssert.AssertionError,
      name: error.name,
      code: error.code,
      message: error.message,
      actual: error.actual,
      expected: error.expected,
      operator: error.operator,
      generatedMessage: error.generatedMessage,
    };
  }

  let strictLegacyAliasRejected = false;
  try {
    strictAssert.equal(1, '1');
  } catch (error) {
    strictLegacyAliasRejected = error.code === 'ERR_ASSERTION';
  }

  const encodedQuery = querystring.stringify({
    foo: 'bar',
    abc: ['xyz', '123'],
    space: 'a b',
    symbol: '✓',
    nil: null,
    truth: true,
    object: { value: 1 },
  });
  const decodedQuery = querystring.parse(
    'foo=bar&abc=xyz&abc=123&space=a+b&bad=%zz'
  );

  const utf8Decoder = new StringDecoder('utf8');
  const utf8Parts = [
    utf8Decoder.write(Buffer.from([0xe2])),
    utf8Decoder.write(Buffer.from([0x82])),
    utf8Decoder.end(Buffer.from([0xac])),
  ];
  const incompleteUtf8Decoder = new StringDecoder('utf8');
  incompleteUtf8Decoder.write(Buffer.from([0xe2]));

  const utf16Decoder = new StringDecoder('utf16le');
  const utf16Parts = [
    utf16Decoder.write(Buffer.from([0x34, 0xd8, 0x1e])),
    utf16Decoder.end(Buffer.from([0xdd])),
  ];

  const base64Decoder = new StringDecoder('base64');
  const base64Parts = [
    base64Decoder.write(Buffer.from([0x6d, 0x75])),
    base64Decoder.end(Buffer.from([0x6f, 0x6e])),
  ];

  return {
    util: {
      moduleAlias: util === utilAlias,
      promisifiedValue,
      promisifiedErrorCode,
      customPromisified: promisify(customSource) === customPromisified,
      formatted: formatValue('name=%s count=%d json=%j %%', 'muon', 2, {
        ok: true,
      }),
      inspected: inspectValue({ name: 'muon', count: 2 }),
      customInspected: inspectValue(customInspectable),
      circularInspected: inspectValue(circularLeft),
      deepCircular: isDeepStrictEqual(circularLeft, circularRight),
      deepDifferent: isDeepStrictEqual(
        { value: new Set([1, 2]) },
        { value: new Set([1, 3]) }
      ),
      stripped: stripVTControlCharacters('\u001b[31mmuon\u001b[0m'),
      types: {
        bufferIsUint8Array: util.types.isUint8Array(Buffer.from('muon')),
        promiseIsPromise: util.types.isPromise(Promise.resolve()),
        mapIsMap: util.types.isMap(new Map()),
      },
    },
    assert: {
      moduleAlias: assert === assertAlias,
      strictLegacyAliasRejected,
      failure: assertionFailure,
    },
    querystring: {
      moduleAlias: querystring === querystringAlias,
      encodeAlias: querystring.encode === querystring.stringify,
      decodeAlias: querystring.decode === querystring.parse,
      encoded: encodedQuery,
      decoded: decodedQuery,
      custom: querystring.stringify({ key: ['one', 'two'] }, ';', ':'),
      malformed: querystring.unescape('%zz'),
    },
    stringDecoder: {
      moduleAlias: stringDecoderModule === stringDecoderAlias,
      utf8Parts,
      incompleteUtf8: incompleteUtf8Decoder.end(),
      utf16Parts,
      base64Parts,
      latin1: new StringDecoder('latin1').end(Buffer.from([0xe2])),
    },
  };
};

export const exerciseStreamAndUrl = async () => {
  const output = [];
  const source = Readable.from(
    [Buffer.from('muon'), Buffer.from('-'), Buffer.from('stream')],
    { objectMode: false }
  );
  const upperCase = new Transform({
    transform: (chunk, encoding, callback) => {
      callback(null, Buffer.from(chunk.toString().toUpperCase()));
    },
  });
  const destination = new Writable({
    highWaterMark: 4,
    write: (chunk, encoding, callback) => {
      setTimeout(() => {
        output.push(chunk.toString());
        callback();
      }, 1);
    },
  });
  await pipelineStreams(source, upperCase, destination);
  await finishedStream(destination);

  const asyncIteratorValues = [];
  for await (const chunk of Readable.from(
    [Buffer.from('async'), Buffer.from('-iterator')],
    { objectMode: false }
  )) {
    asyncIteratorValues.push(chunk.toString());
  }

  const passThroughValues = [];
  const passThrough = new PassThrough();
  passThrough.on('data', (chunk) => passThroughValues.push(chunk.toString()));
  passThrough.write('pass');
  passThrough.end('-through');
  await finishedStream(passThrough);

  const slowWrites = [];
  const slowDestination = new Writable({
    highWaterMark: 3,
    write: (chunk, encoding, callback) => {
      setTimeout(() => {
        slowWrites.push(chunk.toString());
        callback();
      }, 1);
    },
  });
  const acceptedWithoutBackpressure = slowDestination.write('four');
  if (!acceptedWithoutBackpressure) {
    await onceEvent(slowDestination, 'drain');
  }
  slowDestination.end('done');
  await finishedStream(slowDestination);

  const stateProbe = new PassThrough();
  const stateBeforeDestroy = {
    readable: isReadable(stateProbe),
    writable: isWritable(stateProbe),
    destroyed: isDestroyed(stateProbe),
  };
  stateProbe.destroy();
  const stateAfterDestroy = {
    readable: isReadable(stateProbe),
    writable: isWritable(stateProbe),
    destroyed: isDestroyed(stateProbe),
  };

  const url = new URL(
    '../child?alpha=1&alpha=2#section',
    'https://user:pass@example.com:8443/root/base/'
  );
  url.searchParams.append('space', 'a b');
  url.searchParams.set('alpha', '3');
  url.searchParams.sort();
  const httpOptions = urlToHttpOptions(url);

  const parameters = new URLSearchParams('?plus=a+b&empty=&dup=x&dup=y');
  parameters.delete('dup', 'x');

  const fileUrl = pathToFileURL('/data/user/0/app files/é.txt');

  return {
    stream: {
      output: output.join(''),
      asyncIteratorOutput: asyncIteratorValues.join(''),
      passThroughOutput: passThroughValues.join(''),
      acceptedWithoutBackpressure,
      slowWrites,
      stateBeforeDestroy,
      stateAfterDestroy,
    },
    url: {
      href: url.href,
      protocol: url.protocol,
      username: url.username,
      password: url.password,
      hostname: url.hostname,
      port: url.port,
      host: url.host,
      origin: url.origin,
      pathname: url.pathname,
      search: url.search,
      hash: url.hash,
      alpha: url.searchParams.getAll('alpha'),
      entries: [...url.searchParams],
      httpOptions,
      parameters: {
        text: parameters.toString(),
        plus: parameters.get('plus'),
        hasDuplicate: parameters.has('dup', 'y'),
        size: parameters.size,
      },
      fileHref: fileUrl.href,
      filePath: fileURLToPath(fileUrl),
      canParseRelative: URL.canParse('/next', url),
    },
  };
};

export const exerciseDnsAndTcp = async (port, closedPort) => {
  const callbackLookup = await new Promise((resolve, reject) => {
    dns.lookup('localhost', { family: 4 }, (error, address, family) => {
      if (error) {
        reject(error);
        return;
      }
      resolve({ address, family });
    });
  });
  const promiseLookup = await dnsPromises.lookup('localhost', { family: 4 });
  const allLookup = await dnsPromises.lookup('localhost', {
    family: 4,
    all: true,
  });
  const concurrentLookups = await Promise.all([
    dnsPromises.lookup('localhost', { family: 4 }),
    dnsPromises.lookup('127.0.0.1'),
    dnsPromises.lookup('localhost', { family: 4 }),
  ]);
  dns.setDefaultResultOrder('ipv4first');
  const resultOrder = dns.getDefaultResultOrder();
  dns.setDefaultResultOrder('verbatim');

  const events = [];
  let received = '';
  let connection = null;
  const socket = createConnection({
    host: '127.0.0.1',
    port,
    noDelay: true,
  });
  const initiallyConnecting = socket.connecting;
  const initiallyPending = socket.pending;
  const initialReadyState = socket.readyState;
  const isSocket = socket instanceof Socket;
  socket.setEncoding('utf8');
  socket.on('connect', () => {
    events.push('connect');
    connection = {
      local: socket.address(),
      remoteAddress: socket.remoteAddress,
      remoteFamily: socket.remoteFamily,
      remotePort: socket.remotePort,
      readyState: socket.readyState,
    };
  });
  socket.on('ready', () => events.push('ready'));
  socket.on('data', (chunk) => {
    events.push('data');
    received += chunk;
  });
  socket.on('end', () => events.push('end'));
  socket.on('close', () => events.push('close'));
  const closed = onceEvent(socket, 'close');
  socket.end('quickjs-tcp');
  const [hadError] = await closed;

  const failedEvents = [];
  let failureCode = '';
  let failureSyscall = '';
  let failureHadError = false;
  const failedSocket = createConnection({
    host: '127.0.0.1',
    port: closedPort,
  });
  await new Promise((resolve) => {
    failedSocket.once('error', (error) => {
      failedEvents.push('error');
      failureCode = error.code;
      failureSyscall = error.syscall;
    });
    failedSocket.once('close', (closedWithError) => {
      failedEvents.push('close');
      failureHadError = closedWithError;
      resolve();
    });
  });

  return {
    modules: {
      netAlias: net === netAlias,
      dnsAlias: dns === dnsAlias,
      promises: dns.promises === dnsPromises,
    },
    ip: {
      ipv4: net.isIP('127.0.0.1'),
      ipv6: net.isIP('2001:db8::1'),
      invalid: net.isIP('127.0.0.999'),
      isIpv4: net.isIPv4('127.0.0.1'),
      isIpv6: net.isIPv6('2001:db8::1'),
    },
    dns: {
      callbackLookup,
      promiseLookup,
      allLookup,
      concurrentLookups,
      resultOrder,
    },
    tcp: {
      initiallyConnecting,
      initiallyPending,
      initialReadyState,
      isSocket,
      received,
      events,
      connection,
      bytesWritten: socket.bytesWritten,
      bytesRead: socket.bytesRead,
      hadError,
      destroyed: socket.destroyed,
    },
    failure: {
      events: failedEvents,
      code: failureCode,
      syscall: failureSyscall,
      hadError: failureHadError,
      destroyed: failedSocket.destroyed,
    },
  };
};

export const exerciseNetServer = async () => {
  const events = [];
  let receivedByServer = '';
  let acceptedSocketState = null;
  let resolveConnectionCount;
  let rejectConnectionCount;
  const connectionCount = new Promise((resolve, reject) => {
    resolveConnectionCount = resolve;
    rejectConnectionCount = reject;
  });
  const server = createNetServer(
    { allowHalfOpen: true, noDelay: true },
    (socket) => {
      events.push('connection');
      acceptedSocketState = {
        isSocket: socket instanceof Socket,
        localAddress: socket.localAddress,
        localFamily: socket.localFamily,
        localPort: socket.localPort,
        remoteAddress: socket.remoteAddress,
        remoteFamily: socket.remoteFamily,
        readyState: socket.readyState,
      };
      server.getConnections((error, count) => {
        if (error) rejectConnectionCount(error);
        else resolveConnectionCount(count);
      });
      socket.setEncoding('utf8');
      socket.on('data', (chunk) => {
        receivedByServer += chunk;
        if (receivedByServer === 'loopback-input') {
          socket.end('loopback-response');
        }
      });
    }
  );
  server.on('close', () => events.push('close'));
  const listening = onceEvent(server, 'listening');
  server.listen({ port: 0, host: '127.0.0.1', backlog: 8 }, () => {
    events.push('listening');
  });
  await listening;
  const address = server.address();

  const client = createConnection({
    host: address.address,
    port: address.port,
  });
  client.setEncoding('utf8');
  let receivedByClient = '';
  client.on('data', (chunk) => {
    receivedByClient += chunk;
  });
  const connected = onceEvent(client, 'connect');
  const clientEnded = onceEvent(client, 'end');
  const clientClosed = onceEvent(client, 'close');
  await connected;
  client.end('loopback-input');
  await clientEnded;
  await clientClosed;
  const activeConnectionCount = await connectionCount;

  const closed = onceEvent(server, 'close');
  server.close();
  await closed;

  let nonLoopbackCode = '';
  try {
    createNetServer().listen(0, '0.0.0.0');
  } catch (error) {
    nonLoopbackCode = error.code;
  }

  return {
    moduleAlias: net === netAlias,
    isServer: server instanceof NetServer,
    listeningBeforeClose: address !== null,
    address,
    addressAfterClose: server.address(),
    events,
    receivedByServer,
    receivedByClient,
    acceptedSocketState,
    activeConnectionCount,
    finalConnectionCount: server.connections,
    nonLoopbackCode,
  };
};

export const retainTcpConnection = async (port, marker) => {
  const socket = createConnection({ host: '127.0.0.1', port });
  await onceEvent(socket, 'connect');
  await new Promise((resolve, reject) => {
    socket.write(marker, (error) => {
      if (error) reject(error);
      else resolve();
    });
  });
  retainedTcpSocket = socket;
  return {
    readyState: socket.readyState,
    destroyed: socket.destroyed,
  };
};

export const retainedTcpConnectionState = () => ({
  readyState: retainedTcpSocket?.readyState ?? 'missing',
  destroyed: retainedTcpSocket?.destroyed ?? true,
});

export const exerciseHttpAndFetch = async (port) => {
  const request = http.request({
    protocol: 'http:',
    hostname: 'localhost',
    port,
    path: '/node',
    method: 'POST',
    headers: {
      'X-Client': 'node',
      'X-Remove': 'removed',
    },
  });
  request.setHeader('X-Later', 'yes');
  request.removeHeader('X-Remove');
  const requestState = {
    isClientRequest: request instanceof ClientRequest,
    method: request.method,
    path: request.path,
    hasClientHeader: request.hasHeader('x-client'),
    laterHeader: request.getHeader('x-later'),
    removedHeader: request.hasHeader('x-remove'),
  };
  const responseEvent = onceEvent(request, 'response');
  request.write('node-');
  request.end('body');
  const [nodeResponse] = await responseEvent;
  const isIncomingMessage = nodeResponse instanceof IncomingMessage;
  nodeResponse.setEncoding('utf8');
  let nodeBody = '';
  nodeResponse.on('data', (chunk) => {
    nodeBody += chunk;
  });
  await onceEvent(nodeResponse, 'end');

  const getRequest = http.get(`http://localhost:${port}/node-get`, {
    headers: { 'X-Client': 'get' },
  });
  const [getResponse] = await onceEvent(getRequest, 'response');
  getResponse.setEncoding('utf8');
  let getBody = '';
  getResponse.on('data', (chunk) => {
    getBody += chunk;
  });
  await onceEvent(getResponse, 'end');

  const fetchHeaders = new Headers({ 'X-Fetch': 'yes' });
  const fetchResponse = await fetch(`http://localhost:${port}/fetch`, {
    method: 'POST',
    headers: fetchHeaders,
    body: 'fetch-body',
  });
  const fetchJson = await fetchResponse.json();

  const redirectResponse = await fetch(`http://localhost:${port}/redirect`);
  const redirectBody = await redirectResponse.text();

  const controller = new AbortController();
  const slowResponse = await fetch(`http://localhost:${port}/slow`, {
    signal: controller.signal,
  });
  const slowBody = slowResponse.text();
  controller.abort('stop');
  let abortName = '';
  let abortReason = '';
  try {
    await slowBody;
  } catch (error) {
    abortName = error.name;
    abortReason = controller.signal.reason;
  }

  return {
    moduleAlias: http === httpAlias,
    node: {
      request: requestState,
      isIncomingMessage,
      statusCode: nodeResponse.statusCode,
      statusMessage: nodeResponse.statusMessage,
      header: nodeResponse.headers['x-test'],
      rawHeaders: nodeResponse.rawHeaders,
      body: nodeBody,
      complete: nodeResponse.complete,
      getStatusCode: getResponse.statusCode,
      getBody,
    },
    fetch: {
      globals:
        typeof fetch === 'function' &&
        typeof Headers === 'function' &&
        typeof Request === 'function' &&
        typeof Response === 'function',
      isResponse: fetchResponse instanceof Response,
      status: fetchResponse.status,
      statusText: fetchResponse.statusText,
      ok: fetchResponse.ok,
      redirected: fetchResponse.redirected,
      url: fetchResponse.url,
      header: fetchResponse.headers.get('x-test'),
      json: fetchJson,
      redirectStatus: redirectResponse.status,
      redirectUrl: redirectResponse.url,
      redirectedResult: redirectResponse.redirected,
      redirectBody,
      abortName,
      abortReason,
      slowBodyUsed: slowResponse.bodyUsed,
    },
  };
};

export const exerciseHttpServer = async () => {
  const observedRequests = [];
  let responseState = null;
  let clientErrorCode = '';
  const server = createHttpServer(
    { headersTimeout: 5000, requestTimeout: 5000 },
    (request, response) => {
      request.setEncoding('utf8');
      let body = '';
      request.on('data', (chunk) => {
        body += chunk;
      });
      request.on('end', () => {
        observedRequests.push({
          method: request.method,
          url: request.url,
          body,
          host: request.headers.host,
          trailer: request.trailers['x-trailer'] ?? null,
          complete: request.complete,
        });
        if (request.url === '/node-server') {
          response.setHeader('X-Remove', 'removed');
          response.removeHeader('X-Remove');
          response.writeHead(201, 'Created', {
            'Content-Type': 'text/plain',
            'X-Server': 'quickjs',
          });
          response.on('finish', () => {
            responseState = {
              isServerResponse: response instanceof ServerResponse,
              headersSent: response.headersSent,
              finished: response.finished,
              statusCode: response.statusCode,
              statusMessage: response.statusMessage,
              serverHeader: response.getHeader('x-server'),
              removedHeader: response.hasHeader('x-remove'),
            };
          });
          response.write('server-');
          response.end('response');
          return;
        }
        if (request.url === '/fetch-server') {
          response.setHeader('Content-Type', 'application/json');
          response.end(
            JSON.stringify({
              method: request.method,
              body,
              runtime: 'quickjs',
            })
          );
          return;
        }
        response.setHeader('Transfer-Encoding', 'chunked');
        response.writeHead(202, 'Accepted');
        response.end(`chunked:${body}:${request.trailers['x-trailer']}`);
      });
    }
  );
  server.on('clientError', (error, socket) => {
    clientErrorCode = error.code;
    socket.end(
      'HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n'
    );
  });

  const listening = onceEvent(server, 'listening');
  server.listen(0, '127.0.0.1');
  await listening;
  const address = server.address();

  const request = http.request({
    hostname: 'localhost',
    port: address.port,
    path: '/node-server',
    method: 'POST',
    headers: { 'X-Client': 'node' },
  });
  const responseEvent = onceEvent(request, 'response');
  request.end('node-body');
  const [nodeResponse] = await responseEvent;
  nodeResponse.setEncoding('utf8');
  let nodeBody = '';
  nodeResponse.on('data', (chunk) => {
    nodeBody += chunk;
  });
  await onceEvent(nodeResponse, 'end');

  const fetchResponse = await fetch(
    `http://localhost:${address.port}/fetch-server`,
    { method: 'POST', body: 'fetch-body' }
  );
  const fetchValue = await fetchResponse.json();

  const chunkedSocket = createConnection({
    host: address.address,
    port: address.port,
  });
  chunkedSocket.setEncoding('latin1');
  let chunkedResponse = '';
  chunkedSocket.on('data', (chunk) => {
    chunkedResponse += chunk;
  });
  const chunkedConnected = onceEvent(chunkedSocket, 'connect');
  const chunkedEnded = onceEvent(chunkedSocket, 'end');
  const chunkedClosed = onceEvent(chunkedSocket, 'close');
  await chunkedConnected;
  chunkedSocket.end(
    `POST /chunked HTTP/1.1\r\nHost: ${address.address}:${address.port}\r\n` +
      'Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n' +
      '4\r\nWiki\r\n5\r\npedia\r\n0\r\nX-Trailer: yes\r\n\r\n'
  );
  await chunkedEnded;
  await chunkedClosed;

  const invalidSocket = createConnection({
    host: address.address,
    port: address.port,
  });
  invalidSocket.setEncoding('latin1');
  let invalidResponse = '';
  invalidSocket.on('data', (chunk) => {
    invalidResponse += chunk;
  });
  const invalidConnected = onceEvent(invalidSocket, 'connect');
  const invalidEnded = onceEvent(invalidSocket, 'end');
  const invalidClosed = onceEvent(invalidSocket, 'close');
  await invalidConnected;
  invalidSocket.end('BROKEN\r\n\r\n');
  await invalidEnded;
  await invalidClosed;

  const closed = onceEvent(server, 'close');
  server.close();
  await closed;

  return {
    moduleAlias: http === httpAlias,
    isServer: server instanceof HttpServer,
    address,
    responseState,
    node: {
      isIncomingMessage: nodeResponse instanceof IncomingMessage,
      statusCode: nodeResponse.statusCode,
      statusMessage: nodeResponse.statusMessage,
      serverHeader: nodeResponse.headers['x-server'],
      removedHeader: nodeResponse.headers['x-remove'] ?? null,
      body: nodeBody,
      complete: nodeResponse.complete,
    },
    fetch: {
      status: fetchResponse.status,
      value: fetchValue,
    },
    chunkedResponse,
    clientErrorCode,
    invalidResponse,
    observedRequests,
    addressAfterClose: server.address(),
  };
};

export const exerciseNetworkResourceLimits = async (tcpPort, closedPort) => {
  const dnsResults = await Promise.allSettled(
    Array.from({ length: 65 }, () =>
      dnsPromises.lookup('localhost', { family: 4 })
    )
  );

  const sockets = [];
  const socketResults = [];
  for (let index = 0; index < 65; index += 1) {
    const socket = createConnection({ host: '127.0.0.1', port: tcpPort });
    sockets.push(socket);
    socketResults.push(
      new Promise((resolve) => {
        socket.once('connect', () => resolve({ connected: true, code: '' }));
        socket.once('error', (error) =>
          resolve({ connected: false, code: error.code })
        );
      })
    );
  }
  const settledSockets = await Promise.all(socketResults);
  const socketCloses = sockets
    .filter((socket) => !socket.destroyed)
    .map((socket) => onceEvent(socket, 'close'));
  for (const socket of sockets) socket.destroy();
  await Promise.all(socketCloses);

  const servers = [];
  const serverErrors = [];
  for (let index = 0; index < 9; index += 1) {
    const server = createNetServer();
    try {
      server.listen(0, '127.0.0.1');
      servers.push(server);
    } catch (error) {
      serverErrors.push(error.code);
    }
  }
  const serverCloses = servers.map((server) => onceEvent(server, 'close'));
  for (const server of servers) server.close();
  await Promise.all(serverCloses);

  const httpResults = [];
  for (let index = 0; index < 17; index += 1) {
    const request = http.request({
      hostname: 'localhost',
      port: closedPort,
      path: `/limit-${index}`,
    });
    httpResults.push(
      new Promise((resolve) => {
        request.once('error', (error) => resolve(error.code));
        request.once('response', (response) => {
          response.resume();
          response.once('end', () => resolve(''));
        });
      })
    );
    request.end();
  }
  const httpCodes = await Promise.all(httpResults);

  return {
    dns: {
      fulfilled: dnsResults.filter((result) => result.status === 'fulfilled')
        .length,
      codes: dnsResults
        .filter((result) => result.status === 'rejected')
        .map((result) => result.reason.code),
    },
    tcp: {
      connected: settledSockets.filter((result) => result.connected).length,
      codes: settledSockets
        .filter((result) => !result.connected)
        .map((result) => result.code),
    },
    servers: {
      listening: servers.length,
      codes: serverErrors,
    },
    http: {
      codes: httpCodes,
    },
  };
};

const exchangeTcpLoadMarker = async (port, marker) => {
  let received = '';
  const socket = createConnection({
    host: '127.0.0.1',
    port,
    noDelay: true,
  });
  socket.setEncoding('utf8');
  socket.on('data', (chunk) => {
    received += chunk;
  });
  const closed = onceEvent(socket, 'close');
  socket.end(marker);
  const [hadError] = await closed;
  if (hadError) {
    throw new Error(`TCP load exchange failed for ${marker}`);
  }
  return received;
};

const fetchHttpLoadMarker = async (port, marker) => {
  const response = await fetch(`http://localhost:${port}/load/${marker}`);
  if (!response.ok) {
    throw new Error(`HTTP load request failed with status ${response.status}`);
  }
  return await response.text();
};

const exerciseNetworkLoadIteration = async (tcpPort, httpPort, marker) => {
  const [lookup, tcp, httpBody] = await Promise.all([
    dnsPromises.lookup('localhost', { family: 4 }),
    exchangeTcpLoadMarker(tcpPort, marker),
    fetchHttpLoadMarker(httpPort, marker),
  ]);
  return {
    dnsFamily: lookup.family,
    tcp,
    http: httpBody,
  };
};

export const exerciseConcurrentNetworkLoad = async (
  tcpPort,
  httpPort,
  runtimeMarker,
  iterationCount
) => {
  const iterations = [];
  for (let index = 0; index < iterationCount; index += 1) {
    iterations.push(
      exerciseNetworkLoadIteration(
        tcpPort,
        httpPort,
        `${runtimeMarker}-${index}`
      )
    );
  }
  return { iterations: await Promise.all(iterations) };
};

export const exerciseHttps = async (port, certificateAuthority) => {
  const rejectedRequest = https.get(`https://localhost:${port}/untrusted`);
  const [rejection] = await onceEvent(rejectedRequest, 'error');

  let unsafeOptionCode = '';
  try {
    https.get({
      hostname: 'localhost',
      port,
      path: '/unsafe',
      rejectUnauthorized: false,
    });
  } catch (error) {
    unsafeOptionCode = error.code;
  }

  const request = https.get({
    hostname: 'localhost',
    port,
    path: '/secure',
    headers: { 'X-Secure': 'yes' },
    ca: Buffer.from(certificateAuthority),
  });
  const [response] = await onceEvent(request, 'response');
  response.setEncoding('utf8');
  let body = '';
  response.on('data', (chunk) => {
    body += chunk;
  });
  await onceEvent(response, 'end');

  return {
    moduleAlias: https === httpsAlias,
    agent:
      https.globalAgent instanceof https.Agent &&
      https.globalAgent.defaultPort === 443 &&
      https.globalAgent.protocol === 'https:',
    isClientRequest: request instanceof ClientRequest,
    isIncomingMessage: response instanceof IncomingMessage,
    protocol: request.protocol,
    statusCode: response.statusCode,
    header: response.headers['x-secure'],
    body,
    complete: response.complete,
    rejectionCode: rejection.code,
    rejectionUrl: rejection.url,
    unsafeOptionCode,
  };
};

export const exhaustMemory = () => {
  const blocks = [];
  while (true) {
    blocks.push(new Uint8Array(1024 * 1024));
  }
};

export const spin = () => {
  while (true) {
    // The runtime interrupt handler must terminate runaway application code.
  }
};

globalThis.__muonBackendModule = Object.freeze({
  answer,
  increment,
  echo,
  invokeCallback,
  importedPathBasename,
  exerciseRuntimePrimitives,
  exerciseProcessAndOs,
  exerciseUtilityModules,
  exerciseStreamAndUrl,
  exerciseDnsAndTcp,
  exerciseNetServer,
  retainTcpConnection,
  retainedTcpConnectionState,
  exerciseHttpAndFetch,
  exerciseHttpServer,
  exerciseNetworkResourceLimits,
  exerciseConcurrentNetworkLoad,
  exerciseHttps,
  exhaustMemory,
  spin,
});

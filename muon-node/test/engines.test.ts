// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import {
  copyFile,
  mkdir,
  mkdtemp,
  readFile,
  rm,
  symlink,
  writeFile,
} from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

import { describe, expect, it } from 'vitest';
import { loadConfigFromFile } from 'vite';

import { createBridgePeer, muonNodeProtocol } from './support/bridge-peer.js';
import {
  createIncompatibleProjectFixture,
  createProjectFixture,
} from './support/project-fixture.js';

const withProcessVersion = async (
  version: string,
  action: () => Promise<void>
): Promise<void> => {
  const descriptor = Object.getOwnPropertyDescriptor(process, 'version');
  if (descriptor === undefined) {
    throw new Error('process.version descriptor is unavailable.');
  }
  Object.defineProperty(process, 'version', {
    ...descriptor,
    value: version,
  });
  try {
    await action();
  } finally {
    Object.defineProperty(process, 'version', descriptor);
  }
};

const moduleDirectory = dirname(fileURLToPath(import.meta.url));
const packageDirectory = resolve(moduleDirectory, '..');
const workspaceDirectory = resolve(packageDirectory, '..');

const readSupportedNodeRange = async (): Promise<string> => {
  const packageJson = JSON.parse(
    await readFile(join(packageDirectory, 'package.json'), 'utf8')
  ) as {
    engines?: {
      node?: unknown;
    };
  };
  if (typeof packageJson.engines?.node !== 'string') {
    throw new Error('muon-node package.json engines.node is unavailable.');
  }
  return packageJson.engines.node;
};

describe('muon Node project engines', () => {
  it('rejects a runtime outside the Node range supported by the sidecar itself', async () => {
    const supportedNodeRange = await readSupportedNodeRange();
    const project = await createProjectFixture();
    const peer = await createBridgePeer();
    try {
      await withProcessVersion('v20.18.0', async () => {
        await expect(
          peer.request('initialize', {
            protocol: muonNodeProtocol,
            projectRoot: project.root,
          })
        ).resolves.toMatchObject({
          ok: false,
          error: {
            code: 'ERR_MUON_NODE_ENGINE',
            message: expect.stringContaining(supportedNodeRange),
          },
        });
      });
    } finally {
      await peer.close();
      await project.dispose();
    }
  });

  it('accepts a project whose engines.node includes the hosted runtime', async () => {
    const project = await createProjectFixture();
    const peer = await createBridgePeer();
    try {
      await expect(
        peer.request('initialize', {
          protocol: muonNodeProtocol,
          projectRoot: project.root,
        })
      ).resolves.toMatchObject({
        ok: true,
        value: {
          nodeVersion: process.version,
        },
      });
    } finally {
      await peer.close();
      await project.dispose();
    }
  });

  it('rejects a project whose engines.node excludes the hosted runtime', async () => {
    const project = await createIncompatibleProjectFixture();
    const peer = await createBridgePeer();
    try {
      await expect(
        peer.request('initialize', {
          protocol: muonNodeProtocol,
          projectRoot: project.root,
        })
      ).resolves.toMatchObject({
        ok: false,
        error: {
          code: 'ERR_MUON_NODE_ENGINE',
          message: expect.stringContaining('>=999'),
        },
      });
    } finally {
      await peer.close();
      await project.dispose();
    }
  });

  it('defines the supported range from muon-node package.json in both Vite builds', async () => {
    const root = await mkdtemp(join(tmpdir(), 'muon-node-vite-engines-'));
    const fixtureNodeDirectory = join(root, 'muon-node');
    const fixtureUiDirectory = join(root, 'muon-ui');
    const supportedNodeRange = '>=30.4.0 <31';
    try {
      await mkdir(fixtureNodeDirectory, { recursive: true });
      await mkdir(fixtureUiDirectory, { recursive: true });
      await copyFile(
        join(packageDirectory, 'vite.config.ts'),
        join(fixtureNodeDirectory, 'vite.config.ts')
      );
      await copyFile(
        join(workspaceDirectory, 'muon-ui', 'vite.config.ts'),
        join(fixtureUiDirectory, 'vite.config.ts')
      );
      await writeFile(
        join(fixtureNodeDirectory, 'package.json'),
        `${JSON.stringify(
          {
            name: 'muon-node',
            engines: {
              node: supportedNodeRange,
            },
          },
          null,
          2
        )}\n`
      );
      await symlink(
        join(workspaceDirectory, 'node_modules'),
        join(root, 'node_modules'),
        process.platform === 'win32' ? 'junction' : 'dir'
      );

      const configEnvironment = {
        command: 'build' as const,
        mode: 'production',
      };
      const nodeConfig = await loadConfigFromFile(
        configEnvironment,
        join(fixtureNodeDirectory, 'vite.config.ts'),
        fixtureNodeDirectory
      );
      const uiConfig = await loadConfigFromFile(
        configEnvironment,
        join(fixtureUiDirectory, 'vite.config.ts'),
        fixtureUiDirectory
      );
      const expectedDefine = JSON.stringify(supportedNodeRange);

      expect(nodeConfig?.config.define).toMatchObject({
        __MUON_NODE_SUPPORTED_ENGINE_RANGE__: expectedDefine,
      });
      expect(uiConfig?.config.define).toMatchObject({
        __MUON_NODE_SUPPORTED_ENGINE_RANGE__: expectedDefine,
      });
    } finally {
      await rm(root, { recursive: true, force: true });
    }
  });
});

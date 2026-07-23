// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

import { describe, expect, it } from 'vitest';

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

describe('muon Node project engines', () => {
  it('rejects a runtime outside the Node range supported by the sidecar itself', async () => {
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
            message: expect.stringContaining('^20.19.0 || >=22.12.0'),
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
});

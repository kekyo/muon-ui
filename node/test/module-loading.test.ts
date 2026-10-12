// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { describe, expect, it } from 'vitest';

import {
  initializeBridgePeer,
  requireSuccessValue,
  type BridgePeer,
} from './support/bridge-peer.js';
import { createProjectFixture } from './support/project-fixture.js';

interface ModuleExportDescriptor {
  name: string;
  kind: 'function' | 'primitive';
  value?: unknown;
}

interface ImportedModule {
  moduleId: string;
  descriptor: {
    exports: readonly ModuleExportDescriptor[];
  };
}

const importModule = async (
  peer: BridgePeer,
  specifier: string
): Promise<ImportedModule> =>
  requireSuccessValue(
    await peer.request('importModule', {
      specifier,
    })
  ) as ImportedModule;

const callExport = async (
  peer: BridgePeer,
  moduleId: string,
  exportName: string,
  argumentsValue: readonly unknown[]
): Promise<unknown> =>
  requireSuccessValue(
    await peer.request('call', {
      moduleId,
      exportName,
      arguments: argumentsValue,
    })
  );

describe('muon Node module loading', () => {
  it('requires the node: prefix for Node built-in modules', async () => {
    const project = await createProjectFixture();
    const peer = await initializeBridgePeer(project.root);
    try {
      const imported = await importModule(peer, 'node:fs/promises');
      expect(imported).toMatchObject({
        moduleId: expect.any(String),
        descriptor: {
          exports: expect.arrayContaining([
            {
              name: 'readFile',
              kind: 'function',
            },
          ]),
        },
      });

      await expect(
        peer.request('importModule', {
          specifier: 'fs/promises',
        })
      ).resolves.toMatchObject({
        ok: false,
        error: {
          code: 'ERR_MUON_NODE_BUILTIN_SPECIFIER',
        },
      });
    } finally {
      await peer.close();
      await project.dispose();
    }
  });

  it('loads project-relative ESM and CommonJS with package semantics', async () => {
    const project = await createProjectFixture();
    const peer = await initializeBridgePeer(project.root);
    try {
      const esm = await importModule(peer, './backend.js');
      expect(esm.descriptor.exports).toEqual(
        expect.arrayContaining([
          {
            name: 'answer',
            kind: 'primitive',
            value: 42,
          },
          {
            name: 'bigintExport',
            kind: 'primitive',
            value: {
              kind: 'i64',
              value: '123',
            },
          },
          {
            name: 'bufferExport',
            kind: 'primitive',
            value: {
              kind: 'buffer',
              data: Buffer.from([1, 2, 3]).toString('base64'),
            },
          },
          {
            name: 'echo',
            kind: 'function',
          },
          {
            name: 'undefinedExport',
            kind: 'primitive',
            value: {
              kind: 'undefined',
            },
          },
        ])
      );
      expect(esm.descriptor.exports).not.toEqual(
        expect.arrayContaining([
          expect.objectContaining({
            name: 'objectExport',
          }),
        ])
      );
      expect(esm.descriptor.exports).not.toEqual(
        expect.arrayContaining([
          expect.objectContaining({
            name: 'arrayExport',
          }),
        ])
      );
      await expect(
        callExport(peer, esm.moduleId, 'echo', ['relative-esm'])
      ).resolves.toBe('relative-esm');

      const commonJs = await importModule(peer, './backend.cjs');
      expect(commonJs.descriptor.exports).toEqual(
        expect.arrayContaining([
          {
            name: 'cjsEcho',
            kind: 'function',
          },
          {
            name: 'cjsValue',
            kind: 'primitive',
            value: 'commonjs',
          },
        ])
      );
      await expect(
        callExport(peer, commonJs.moduleId, 'cjsEcho', ['relative-cjs'])
      ).resolves.toBe('relative-cjs');
      await expect(
        callExport(peer, commonJs.moduleId, 'cjsThis', ['receiver'])
      ).resolves.toBe('commonjs:receiver');
    } finally {
      await peer.close();
      await project.dispose();
    }
  });

  it('loads the configured project main entry through the dot specifier', async () => {
    const project = await createProjectFixture();
    const peer = await initializeBridgePeer(project.root);
    try {
      const main = await importModule(peer, '.');
      expect(main.descriptor.exports).toEqual(
        expect.arrayContaining([
          {
            name: 'answer',
            kind: 'primitive',
            value: 42,
          },
          {
            name: 'echo',
            kind: 'function',
          },
          {
            name: 'undefinedExport',
            kind: 'primitive',
            value: {
              kind: 'undefined',
            },
          },
        ])
      );
      await expect(
        callExport(peer, main.moduleId, 'echo', ['project-main'])
      ).resolves.toBe('project-main');
    } finally {
      await peer.close();
      await project.dispose();
    }
  });

  it('rolls back a module handle when its descriptor cannot cross the bridge', async () => {
    const project = await createProjectFixture();
    const peer = await initializeBridgePeer(project.root);
    try {
      await expect(
        peer.request('importModule', {
          specifier: './oversized-export.js',
        })
      ).resolves.toMatchObject({
        ok: false,
        error: {
          code: 'ERR_MUON_NODE_FRAME_TOO_LARGE',
        },
      });
      await expect(
        peer.request('release', {
          kind: 'module',
          handle: 'module-1',
        })
      ).resolves.toMatchObject({
        ok: true,
        value: {
          released: false,
        },
      });
    } finally {
      await peer.close();
      await project.dispose();
    }
  });

  it('describes callable CommonJS properties without changing the default receiver', async () => {
    const project = await createProjectFixture();
    const peer = await initializeBridgePeer(project.root);
    try {
      const callable = await importModule(peer, './callable.cjs');
      expect(callable.descriptor.exports).toEqual(
        expect.arrayContaining([
          {
            name: 'default',
            kind: 'function',
          },
          {
            name: 'label',
            kind: 'primitive',
            value: 'callable-commonjs',
          },
          {
            name: 'method',
            kind: 'function',
          },
        ])
      );
      await expect(
        callExport(peer, callable.moduleId, 'default', [])
      ).resolves.toBe(true);
      await expect(
        callExport(peer, callable.moduleId, 'method', ['receiver'])
      ).resolves.toBe('callable-commonjs:receiver');
    } finally {
      await peer.close();
      await project.dispose();
    }
  });

  it('rejects the facade-reserved $release export explicitly', async () => {
    const project = await createProjectFixture();
    const peer = await initializeBridgePeer(project.root);
    try {
      await expect(
        peer.request('importModule', {
          specifier: './reserved-export.js',
        })
      ).resolves.toMatchObject({
        ok: false,
        error: {
          code: 'ERR_MUON_NODE_RESERVED_EXPORT',
          message: expect.stringContaining('$release'),
        },
      });
    } finally {
      await peer.close();
      await project.dispose();
    }
  });

  it.each([
    ['./reserved-then.js', 'ESM'],
    ['./reserved-then.cjs', 'CommonJS'],
  ])(
    'rejects the facade-reserved then export from %s (%s)',
    async (specifier) => {
      const project = await createProjectFixture();
      const peer = await initializeBridgePeer(project.root);
      try {
        await expect(
          peer.request('importModule', {
            specifier,
          })
        ).resolves.toMatchObject({
          ok: false,
          error: {
            code: 'ERR_MUON_NODE_RESERVED_EXPORT',
            message: expect.stringContaining('then'),
          },
        });
      } finally {
        await peer.close();
        await project.dispose();
      }
    }
  );

  it('resolves bare ESM and CommonJS packages from the project', async () => {
    const project = await createProjectFixture();
    const peer = await initializeBridgePeer(project.root);
    try {
      const esm = await importModule(peer, 'fixture-bare-esm');
      expect(esm.descriptor.exports).toEqual(
        expect.arrayContaining([
          {
            name: 'packageKind',
            kind: 'primitive',
            value: 'bare-esm',
          },
          {
            name: 'fromBareEsm',
            kind: 'function',
          },
        ])
      );
      await expect(
        callExport(peer, esm.moduleId, 'fromBareEsm', ['value'])
      ).resolves.toBe('esm:value');

      const commonJs = await importModule(peer, 'fixture-bare-cjs');
      expect(commonJs.descriptor.exports).toEqual(
        expect.arrayContaining([
          {
            name: 'packageKind',
            kind: 'primitive',
            value: 'bare-cjs',
          },
          {
            name: 'fromBareCjs',
            kind: 'function',
          },
        ])
      );
      await expect(
        callExport(peer, commonJs.moduleId, 'fromBareCjs', ['value'])
      ).resolves.toBe('cjs:value');
    } finally {
      await peer.close();
      await project.dispose();
    }
  });
});

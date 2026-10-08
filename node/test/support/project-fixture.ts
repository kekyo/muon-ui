// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { cp, mkdir, mkdtemp, readdir, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { basename, join } from 'node:path';
import { fileURLToPath } from 'node:url';

export interface ProjectFixture {
  root: string;
  dispose(): Promise<void>;
}

const fixtureDirectory = fileURLToPath(new URL('../fixtures', import.meta.url));

const createFixtureCopy = async (
  fixtureName: string
): Promise<ProjectFixture> => {
  const temporaryRoot = await mkdtemp(
    join(tmpdir(), `muon-node-${fixtureName}-`)
  );
  const root = join(temporaryRoot, 'project');
  await cp(join(fixtureDirectory, fixtureName), root, {
    recursive: true,
  });
  return {
    root,
    dispose: async (): Promise<void> => {
      await rm(temporaryRoot, {
        recursive: true,
        force: true,
      });
    },
  };
};

export const createProjectFixture = async (): Promise<ProjectFixture> => {
  const fixture = await createFixtureCopy('project');
  const dependencySource = join(fixture.root, 'dependencies');
  const nodeModules = join(fixture.root, 'node_modules');
  await mkdir(nodeModules, {
    recursive: true,
  });
  const dependencies = await readdir(dependencySource, {
    withFileTypes: true,
  });
  for (const dependency of dependencies) {
    if (!dependency.isDirectory()) {
      continue;
    }
    const source = join(dependencySource, dependency.name);
    await cp(source, join(nodeModules, basename(source)), {
      recursive: true,
    });
  }
  await rm(dependencySource, {
    recursive: true,
    force: true,
  });
  return fixture;
};

export const createIncompatibleProjectFixture =
  async (): Promise<ProjectFixture> =>
    await createFixtureCopy('incompatible-project');

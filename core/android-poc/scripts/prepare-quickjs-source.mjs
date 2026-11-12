// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { copyFileSync, mkdirSync, rmSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { prepareMuonQuickJsSource } from '../../common/prepare-quickjs-source.mjs';

const argumentsSet = new Set(process.argv.slice(2));
for (const argument of argumentsSet)
  if (argument !== '--clean') throw new Error(`Unknown argument: ${argument}`);

const androidRoot = resolve(
  dirname(fileURLToPath(import.meta.url)),
  '../android'
);
const generatedAssetRoot = join(androidRoot, '.generated', 'quickjs-assets');
if (argumentsSet.has('--clean'))
  rmSync(generatedAssetRoot, { recursive: true, force: true });
const { sourceRoot } = await prepareMuonQuickJsSource(
  join(androidRoot, '.native-dependencies', 'quickjs'),
  argumentsSet.has('--clean')
);
const licenseDestination = join(
  generatedAssetRoot,
  'third-party',
  'quickjs-LICENSE'
);
mkdirSync(dirname(licenseDestination), { recursive: true });
copyFileSync(join(sourceRoot, 'LICENSE'), licenseDestination);

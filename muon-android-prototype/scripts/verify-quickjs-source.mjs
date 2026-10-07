// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { createHash } from 'node:crypto';
import { readFileSync, statSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const projectRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const androidRoot = join(projectRoot, 'android');
const dependencyRoot = join(androidRoot, '.native-dependencies', 'quickjs');
const version = '2026-06-04';
const archiveUrl = `https://bellard.org/quickjs/quickjs-${version}.tar.xz`;
const archiveSha256 =
  'b376e839b322978313d929fd20663b11ba58b75df5a46c126dd19ea2fa70ad2a';
const sourceRoot = join(dependencyRoot, `quickjs-${version}`);
const requiredSourceFiles = [
  'LICENSE',
  'VERSION',
  'cutils.c',
  'dtoa.c',
  'libregexp.c',
  'libunicode.c',
  'quickjs.c',
  'quickjs.h',
];

const expectCondition = (condition, message) => {
  if (!condition) {
    throw new Error(message);
  }
};

const sha256 = (filePath) =>
  createHash('sha256').update(readFileSync(filePath)).digest('hex');

const manifest = JSON.parse(
  readFileSync(join(dependencyRoot, 'manifest.json'), 'utf8')
);
expectCondition(manifest.schemaVersion === 1, 'QuickJS schema mismatch');
expectCondition(manifest.version === version, 'QuickJS version mismatch');
expectCondition(
  manifest.sourceArchiveUrl === archiveUrl,
  'QuickJS source URL mismatch'
);
expectCondition(
  manifest.sourceArchiveSha256 === archiveSha256,
  'QuickJS source archive hash mismatch'
);
expectCondition(
  sha256(join(dependencyRoot, `quickjs-${version}.tar.xz`)) === archiveSha256,
  'Cached QuickJS archive hash mismatch'
);
expectCondition(
  readFileSync(join(sourceRoot, 'VERSION'), 'utf8').trim() === version,
  'QuickJS VERSION mismatch'
);
for (const relativePath of requiredSourceFiles) {
  const filePath = join(sourceRoot, relativePath);
  expectCondition(statSync(filePath).size > 0, `${relativePath} is empty`);
  expectCondition(
    manifest.files[relativePath] === sha256(filePath),
    `${relativePath} hash mismatch`
  );
}
const generatedLicense = join(
  androidRoot,
  '.generated',
  'quickjs-assets',
  'third-party',
  'quickjs-LICENSE'
);
expectCondition(
  sha256(generatedLicense) === sha256(join(sourceRoot, 'LICENSE')),
  'Packaged QuickJS license mismatch'
);

console.log('muon_android_quickjs_source_test: PASS');

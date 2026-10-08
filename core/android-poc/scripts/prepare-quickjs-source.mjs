// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import {
  copyFileSync,
  existsSync,
  mkdirSync,
  mkdtempSync,
  readFileSync,
  renameSync,
  rmSync,
  writeFileSync,
} from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const projectRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const androidRoot = join(projectRoot, 'android');
const dependencyRoot = join(androidRoot, '.native-dependencies', 'quickjs');
const generatedAssetRoot = join(androidRoot, '.generated', 'quickjs-assets');
const version = '2026-06-04';
const archiveName = `quickjs-${version}.tar.xz`;
const archiveUrl = `https://bellard.org/quickjs/${archiveName}`;
const archiveSha256 =
  'b376e839b322978313d929fd20663b11ba58b75df5a46c126dd19ea2fa70ad2a';
const requiredSourceFiles = [
  'LICENSE',
  'VERSION',
  'cutils.c',
  'cutils.h',
  'dtoa.c',
  'dtoa.h',
  'libregexp.c',
  'libregexp.h',
  'libunicode-table.h',
  'libunicode.c',
  'libunicode.h',
  'list.h',
  'quickjs-atom.h',
  'quickjs-opcode.h',
  'quickjs.c',
  'quickjs.h',
];

const sha256Buffer = (contents) =>
  createHash('sha256').update(contents).digest('hex');

const sha256File = (filePath) => sha256Buffer(readFileSync(filePath));

const canonicalJson = (value) => `${JSON.stringify(value)}\n`;

const isCurrentSource = (sourceRoot, manifestPath) => {
  if (!existsSync(manifestPath)) {
    return false;
  }
  try {
    const manifest = JSON.parse(readFileSync(manifestPath, 'utf8'));
    if (
      manifest.schemaVersion !== 1 ||
      manifest.version !== version ||
      manifest.sourceArchiveUrl !== archiveUrl ||
      manifest.sourceArchiveSha256 !== archiveSha256
    ) {
      return false;
    }
    if (readFileSync(join(sourceRoot, 'VERSION'), 'utf8').trim() !== version) {
      return false;
    }
    return requiredSourceFiles.every(
      (relativePath) =>
        manifest.files?.[relativePath] ===
        sha256File(join(sourceRoot, relativePath))
    );
  } catch {
    return false;
  }
};

const downloadArchive = async (archivePath) => {
  if (existsSync(archivePath)) {
    const cached = readFileSync(archivePath);
    if (sha256Buffer(cached) === archiveSha256) {
      return cached;
    }
    rmSync(archivePath, { force: true });
  }

  console.log(`Downloading ${archiveUrl}...`);
  const response = await fetch(archiveUrl);
  if (!response.ok) {
    throw new Error(
      `Unable to download QuickJS source archive: HTTP ${response.status}`
    );
  }
  const downloaded = Buffer.from(await response.arrayBuffer());
  const actualSha256 = sha256Buffer(downloaded);
  if (actualSha256 !== archiveSha256) {
    throw new Error(`QuickJS source archive hash mismatch: ${actualSha256}`);
  }
  writeFileSync(archivePath, downloaded);
  return downloaded;
};

const extractArchive = (archive, destination) => {
  mkdirSync(destination, { recursive: true });
  const result = spawnSync(
    'tar',
    ['-xJf', '-', '--strip-components=1', '-C', destination],
    {
      input: archive,
      stdio: ['pipe', 'inherit', 'inherit'],
    }
  );
  if (result.error) {
    throw result.error;
  }
  if (result.status !== 0) {
    throw new Error(`tar failed with exit status ${result.status}`);
  }
};

const argumentsSet = new Set(process.argv.slice(2));
for (const argument of argumentsSet) {
  if (argument !== '--clean') {
    throw new Error(`Unknown argument: ${argument}`);
  }
}

if (argumentsSet.has('--clean')) {
  rmSync(dependencyRoot, { force: true, recursive: true });
  rmSync(generatedAssetRoot, { force: true, recursive: true });
}
mkdirSync(dependencyRoot, { recursive: true });
const archivePath = join(dependencyRoot, archiveName);
const sourceRoot = join(dependencyRoot, `quickjs-${version}`);
const manifestPath = join(dependencyRoot, 'manifest.json');

if (!isCurrentSource(sourceRoot, manifestPath)) {
  const archive = await downloadArchive(archivePath);
  const temporaryRoot = mkdtempSync(join(dependencyRoot, '.install-'));
  const temporarySource = join(temporaryRoot, `quickjs-${version}`);
  try {
    extractArchive(archive, temporarySource);
    if (
      readFileSync(join(temporarySource, 'VERSION'), 'utf8').trim() !== version
    ) {
      throw new Error('The QuickJS VERSION file does not match the archive');
    }
    const files = Object.fromEntries(
      requiredSourceFiles.map((relativePath) => [
        relativePath,
        sha256File(join(temporarySource, relativePath)),
      ])
    );
    rmSync(sourceRoot, { force: true, recursive: true });
    renameSync(temporarySource, sourceRoot);
    writeFileSync(
      manifestPath,
      canonicalJson({
        schemaVersion: 1,
        version,
        sourceArchiveUrl: archiveUrl,
        sourceArchiveSha256: archiveSha256,
        files,
      })
    );
  } finally {
    rmSync(temporaryRoot, { force: true, recursive: true });
  }
} else {
  console.log(`QuickJS ${version} source is current.`);
}

const licenseDestination = join(
  generatedAssetRoot,
  'third-party',
  'quickjs-LICENSE'
);
mkdirSync(dirname(licenseDestination), { recursive: true });
copyFileSync(join(sourceRoot, 'LICENSE'), licenseDestination);
console.log(`QuickJS ${version} source preparation: PASS`);

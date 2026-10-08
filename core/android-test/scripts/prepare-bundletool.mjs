// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { createHash } from 'node:crypto';
import {
  existsSync,
  mkdirSync,
  readFileSync,
  rmSync,
  writeFileSync,
} from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const projectRoot = resolve(
  process.argv[2] ?? resolve(dirname(fileURLToPath(import.meta.url)), '..')
);
const version = '1.18.3';
const archiveName = `bundletool-all-${version}.jar`;
const archiveUrl =
  `https://github.com/google/bundletool/releases/download/${version}/` +
  archiveName;
const archiveSha256 =
  'a099cfa1543f55593bc2ed16a70a7c67fe54b1747bb7301f37fdfd6d91028e29';
const archivePath = join(
  projectRoot,
  'android',
  '.generated',
  'bundletool',
  archiveName
);

const sha256 = (contents) =>
  createHash('sha256').update(contents).digest('hex');

if (existsSync(archivePath)) {
  const cached = readFileSync(archivePath);
  if (sha256(cached) === archiveSha256) {
    console.log(`Bundletool ${version} is current.`);
    process.exit(0);
  }
  rmSync(archivePath, { force: true });
}

console.log(`Downloading ${archiveUrl}...`);
const response = await fetch(archiveUrl);
if (!response.ok) {
  throw new Error(`Unable to download bundletool: HTTP ${response.status}`);
}
const downloaded = Buffer.from(await response.arrayBuffer());
const downloadedSha256 = sha256(downloaded);
if (downloadedSha256 !== archiveSha256) {
  throw new Error(`Bundletool archive hash mismatch: ${downloadedSha256}`);
}
mkdirSync(dirname(archivePath), { recursive: true });
writeFileSync(archivePath, downloaded);
console.log(`Bundletool ${version}: PASS`);

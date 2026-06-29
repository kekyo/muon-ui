import { execFile } from "node:child_process";
import { mkdtemp, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { promisify } from "node:util";

const execFileAsync = promisify(execFile);
const scriptDirectory = dirname(fileURLToPath(import.meta.url));

const prepareExecutablePath = process.argv[2];
if (prepareExecutablePath === undefined) {
  console.error("Usage: node test-resource-update.mjs <muon-prepare>");
  process.exit(1);
}

const root = await mkdtemp(join(tmpdir(), "muon-prepare-resource-"));
try {
  const inputPath = join(root, "input.exe");
  const outputPath = join(root, "output.exe");
  const iconPath = join(root, "app.ico");
  const updatesPath = join(root, "updates.json");
  await execFileAsync(process.execPath, [
    join(scriptDirectory, "create-windows-resource-fixture.mjs"),
    inputPath,
    iconPath,
  ]);
  await writeFile(
    updatesPath,
    `${JSON.stringify(
      {
        version: {
          language: 1033,
          codePage: 1200,
          fixed: {
            fileVersion: "1.2.3.4",
            productVersion: "2.3.4.5",
            fileOS: "windows32",
            fileType: "app",
          },
          strings: {
            CompanyName: "Muon Tester",
            FileDescription: "Resource Test",
            FileVersion: "1.2.3",
            ProductName: "Resource Product",
            ProductVersion: "2.3.4",
          },
        },
        icons: [{ id: 1, language: 1033, path: "app.ico" }],
      },
      null,
      2,
    )}\n`,
  );
  await execFileAsync(prepareExecutablePath, [
    "resource",
    "--input",
    inputPath,
    "--updates-json",
    updatesPath,
    "--output",
    outputPath,
    "--quiet",
  ]);
  await execFileAsync(process.execPath, [
    join(scriptDirectory, "assert-windows-icon.mjs"),
    outputPath,
    iconPath,
  ]);
  await execFileAsync(process.execPath, [
    join(scriptDirectory, "assert-windows-version.mjs"),
    outputPath,
    "--file-version",
    "1.2.3.4",
    "--product-version",
    "2.3.4.5",
    "CompanyName=Muon Tester",
    "FileDescription=Resource Test",
    "ProductName=Resource Product",
  ]);

  const invalidPath = join(root, "not-pe.exe");
  await writeFile(invalidPath, "not a PE file\n");
  let failed = false;
  try {
    await execFileAsync(prepareExecutablePath, [
      "resource",
      "--input",
      invalidPath,
      "--updates-json",
      updatesPath,
      "--output",
      join(root, "invalid-output.exe"),
      "--quiet",
    ]);
  } catch {
    failed = true;
  }
  if (!failed) {
    throw new Error("muon-prepare resource accepted a non-PE input.");
  }
} finally {
  await rm(root, { recursive: true, force: true });
}

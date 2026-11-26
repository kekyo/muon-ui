// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { X509Certificate } from "node:crypto";
import { readFile, stat } from "node:fs/promises";
import { resolve } from "node:path";

/** Web deployment resolved for an APK, including its optional host-scoped public trust anchor. */
export interface MuonAndroidWebDeployment {
  /** Explicit resource origins; undefined retains the bundled runtime's default network behavior. */
  resourceOrigins: readonly string[] | undefined;
  /** Paths served by this application's HTTP services on IPv4 loopback. */
  loopbackResourcePaths: readonly string[];
  /** Optional application adapter, evaluated after the runtime in each trusted main document. */
  documentStartScript: string | undefined;
  /** Public CA in DER format, scoped to the startup hostname during packaging. */
  certificate: Buffer | undefined;
}

const bundled = "https://main.asset.muon.invalid";
const httpsUrl = (input: unknown, label: string) => {
  if (
    typeof input !== "string" ||
    !/^https:\/\/[^/]/iu.test(input) ||
    /[\s\\*\u0000-\u001f\u007f]/u.test(input)
  )
    throw new Error(label + " must be an absolute HTTPS URL.");
  let url: URL;
  try {
    url = new URL(input);
  } catch {
    throw new Error(label + " is not a valid URL.");
  }
  if (
    url.username ||
    url.password ||
    input.split("://")[1]?.split(/[/?#]/u)[0]?.includes("@")
  )
    throw new Error(label + " does not accept credentials.");
  return url;
};

/** Normalize the launch URL and APK-owned web options before any Gradle project is generated.
 * @param page Explicit HTTPS or bundled asset entry.
 * @param value The android.web options.
 * @param directory Base for paths supplied by this configuration.
 * @returns Normalized start page and deployment used by the Android runtime.
 */
export const resolveAndroidWebDeployment = async (
  page: string,
  value: unknown,
  directory: string,
) => {
  const start = httpsUrl(
    page.replace(/^asset:\/\/main\//u, bundled + "/"),
    "browser.startPage",
  );
  if (
    value !== undefined &&
    (value === null || typeof value !== "object" || Array.isArray(value))
  )
    throw new Error("android.web must be an object.");
  const options = (value ?? {}) as Record<string, unknown>;
  if (
    Object.keys(options).some(
      (k) =>
        ![
          "resourceOrigins",
          "loopbackResourcePaths",
          "documentStartScript",
          "certificateAuthority",
        ].includes(k),
    )
  )
    throw new Error("Unknown android.web option.");
  const items = (name: string, parse: (v: string) => string): string[] => {
    const entries = options[name] === undefined ? [] : options[name];
    if (!Array.isArray(entries) || entries.some((v) => typeof v !== "string"))
      throw new Error("android.web." + name + " must contain strings.");
    return [...new Set(entries.map(parse))];
  };
  const resourceOrigins =
    options.resourceOrigins === undefined && start.origin === bundled
      ? undefined
      : items("resourceOrigins", (entry) => {
          const url = httpsUrl(entry, "android.web.resourceOrigins");
          if (
            url.pathname !== "/" ||
            url.search ||
            url.hash ||
            !/^https:\/\/[^/?#]+\/?$/iu.test(entry)
          )
            throw new Error(
              "android.web.resourceOrigins must contain origins without paths.",
            );
          return url.origin;
        });
  const loopbackResourcePaths = items("loopbackResourcePaths", (path) => {
    if (!/^\/(?:[a-z\d_~-][a-z\d._~-]*\/)+$/iu.test(path))
      throw new Error(
        "android.web.loopbackResourcePaths must contain absolute directory paths.",
      );
    return path;
  });
  const file = async (name: string) => {
    if (options[name] === undefined) return undefined;
    const setting = options[name];
    if (typeof setting !== "string" || !setting.trim())
      throw new Error("android.web." + name + " must name a file.");
    const path = resolve(directory, setting);
    if (!(await stat(path)).isFile())
      throw new Error("android.web." + name + " must name a file.");
    return path;
  };
  const documentStartScript = await file("documentStartScript");
  const certificatePath = await file("certificateAuthority");
  let certificate: Buffer | undefined;
  if (certificatePath) {
    if (start.origin === bundled)
      throw new Error(
        "android.web.certificateAuthority needs a hosted HTTPS start page.",
      );
    const source = await readFile(certificatePath);
    if (source.includes("PRIVATE KEY"))
      throw new Error(
        "android.web.certificateAuthority must contain only a public certificate.",
      );
    const ca = new X509Certificate(source);
    if (
      !ca.ca ||
      Date.parse(ca.validFrom) > Date.now() ||
      Date.parse(ca.validTo) < Date.now()
    )
      throw new Error(
        "android.web.certificateAuthority must be a currently valid CA certificate.",
      );
    certificate = ca.raw;
  }
  return {
    startPage: start.href,
    web: {
      resourceOrigins,
      loopbackResourcePaths,
      documentStartScript,
      certificate,
    } satisfies MuonAndroidWebDeployment,
  };
};

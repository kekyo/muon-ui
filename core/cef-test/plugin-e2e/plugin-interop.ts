// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { expect, it } from "vitest";

import {
  MUON_APP_URL,
  MUON_PORT,
  TEST_BROWSER_PLUGIN_ALLOW_PATTERNS,
  TEST_NETWORK_ALLOW_PATTERNS,
  TEST_PLUGIN_ALLOW_PATTERNS,
  cdpCommandTimeoutMs,
  connectToMuonCdp,
  describeMuonPluginBridge,
  evaluateRejection,
  expectDebugMuonStartupFailure,
  join,
  mkdtemp,
  openPopupTarget,
  readFile,
  readFunctionWrapperDiagnostics,
  rm,
  startDebugMuon,
  stopMuon,
  tmpdir,
  waitForFunctionWrapperDiagnosticBaseline,
  withMuon,
  withMuonEnvironment,
  withTrackedMuon,
} from "./shared.js";
import type { CdpDriver } from "./shared.js";

describeMuonPluginBridge("muon plugin bridge - plugin interop", () => {
  it("recreates the plugin namespace API after navigation", async () => {
    await withMuon(["muon_test_plugin_alpha"], async (driver) => {
      await expect(
        driver.evaluate("window.muon.test.alpha.alphaName()"),
      ).resolves.toBe("alpha");
      await driver.navigate(
        "data:text/html,<title>muon reloaded</title>",
        cdpCommandTimeoutMs,
      );
      await expect(driver.evaluate("document.title")).resolves.toBe(
        "muon reloaded",
      );
      await expect(
        driver.evaluate("window.muon.test.alpha.alphaName()"),
      ).resolves.toBe("alpha");
    });
  });

  it("exposes functions from the selected plugin", async () => {
    await withMuon(["muon_test_plugin_alpha"], async (driver) => {
      await expect(
        driver.evaluate("typeof window.muon.test.alpha.alphaName"),
      ).resolves.toBe("function");
      await expect(
        driver.evaluate("typeof window.muon.test.beta"),
      ).resolves.toBe("undefined");
      await expect(
        driver.evaluate("typeof window.muon.test.alpha.send"),
      ).resolves.toBe("undefined");
      await expect(
        driver.evaluate(
          "window.muon.test.alpha.alphaName() instanceof Promise",
        ),
      ).resolves.toBe(true);
      await expect(
        driver.evaluate("window.muon.test.alpha.alphaName()"),
      ).resolves.toBe("alpha");
      await expect(
        driver.evaluate("window.muon.test.alpha.alphaAdd(12, 30)"),
      ).resolves.toBe(42);
    });
  });

  it("passes string config entries to external plugins", async () => {
    const running = await startDebugMuon(
      ["muon_test_plugin_alpha"],
      TEST_NETWORK_ALLOW_PATTERNS,
      {},
      undefined,
      ["muon.test.alpha.alphaName", "muon.test.alpha.alphaConfig"],
      ["muon_test_plugin_alpha"],
      TEST_BROWSER_PLUGIN_ALLOW_PATTERNS,
      [],
      null,
      true,
      undefined,
      undefined,
      undefined,
      undefined,
      undefined,
      undefined,
      undefined,
      undefined,
      undefined,
      {},
      {},
      {
        muon_test_plugin_alpha: {
          "alpha.config": "configured\nvalue",
        },
      },
    );
    let driver: CdpDriver | undefined = undefined;
    try {
      driver = await connectToMuonCdp({
        port: MUON_PORT,
        timeoutMs: cdpCommandTimeoutMs,
      });
      await driver.navigate(
        "data:text/html,<title>muon plugin config</title>",
        cdpCommandTimeoutMs,
      );
      await expect(
        driver.evaluate("window.muon.test.alpha.alphaConfig()"),
      ).resolves.toBe("configured\nvalue");
    } catch (error) {
      throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
    } finally {
      await stopMuon(running, driver);
    }
  });

  it("filters external plugin functions by full public path", async () => {
    const running = await startDebugMuon(
      ["muon_test_plugin_alpha"],
      TEST_NETWORK_ALLOW_PATTERNS,
      {},
      undefined,
      ["muon.test.alpha.alphaName"],
    );
    let driver: CdpDriver | undefined = undefined;
    try {
      driver = await connectToMuonCdp({
        port: MUON_PORT,
        timeoutMs: cdpCommandTimeoutMs,
      });
      await driver.navigate(
        "data:text/html,<title>muon plugin allow</title>",
        cdpCommandTimeoutMs,
      );
      await expect(
        driver.evaluate("Object.keys(window.muon.test.alpha).sort()"),
      ).resolves.toEqual(["alphaName"]);
      await expect(
        driver.evaluate("window.muon.test.alpha.alphaName()"),
      ).resolves.toBe("alpha");
      await expect(
        driver.evaluate("typeof window.muon.test.alpha.alphaAdd"),
      ).resolves.toBe("undefined");
    } catch (error) {
      throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
    } finally {
      await stopMuon(running, driver);
    }
  });

  it("routes calls across multiple plugins", async () => {
    await withMuon(
      ["muon_test_plugin_alpha", "muon_test_plugin_beta"],
      async (driver) => {
        await expect(
          driver.evaluate("window.muon.test.alpha.alphaName()"),
        ).resolves.toBe("alpha");
        await expect(
          driver.evaluate("window.muon.test.beta.betaName()"),
        ).resolves.toBe("beta");
        await expect(
          driver.evaluate(
            'window.muon.test.beta.betaDescribe(true, 7, 2.5, "units")',
          ),
        ).resolves.toBe("true:7:2.50:units");
      },
    );
  });

  it("deep-freezes plugin definitions after initialization", async () => {
    await withMuon(
      ["muon_test_plugin_alpha", "muon_test_plugin_beta"],
      async (driver) => {
        const values = await driver.evaluate<{
          alphaBefore: string;
          alphaAfter: string;
          beta: string;
          frozen: {
            muon: boolean;
            test: boolean;
            alpha: boolean;
            alphaName: boolean;
            beta: boolean;
            executor: boolean;
            executorSpawn: boolean;
          };
          mutationResults: Record<string, string>;
          alphaNameUnchanged: boolean;
          rootExtraType: string;
          functionExtraType: string;
          alphaKeys: string[];
          executorKeys: string[];
          internalRpcType: string;
        }>(`(async () => {
          "use strict";
          const captureMutation = (mutate) => {
            try {
              mutate();
              return "none";
            } catch (error) {
              return error?.constructor?.name ?? typeof error;
            }
          };
          const originalAlphaName = window.muon.test.alpha.alphaName;
          const alphaBefore = await window.muon.test.alpha.alphaName();
          const mutationResults = {
            addRootProperty: captureMutation(() => {
              window.muon.extra = true;
            }),
            deleteFunction: captureMutation(() => {
              delete window.muon.test.alpha.alphaName;
            }),
            replaceFunction: captureMutation(() => {
              window.muon.test.alpha.alphaName = () => "mutated";
            }),
            addFunctionProperty: captureMutation(() => {
              window.muon.test.alpha.alphaName.extra = true;
            }),
          };
          return {
            alphaBefore,
            alphaAfter: await window.muon.test.alpha.alphaName(),
            beta: await window.muon.test.beta.betaName(),
            frozen: {
              muon: Object.isFrozen(window.muon),
              test: Object.isFrozen(window.muon.test),
              alpha: Object.isFrozen(window.muon.test.alpha),
              alphaName: Object.isFrozen(window.muon.test.alpha.alphaName),
              beta: Object.isFrozen(window.muon.test.beta),
              executor: Object.isFrozen(window.muon.executor),
              executorSpawn: Object.isFrozen(window.muon.executor.spawn),
            },
            mutationResults,
            alphaNameUnchanged:
              window.muon.test.alpha.alphaName === originalAlphaName,
            rootExtraType: typeof window.muon.extra,
            functionExtraType: typeof window.muon.test.alpha.alphaName.extra,
            alphaKeys: Object.keys(window.muon.test.alpha).sort(),
            executorKeys: Object.keys(window.muon.executor).sort(),
            runType: typeof window.muon.executor.run,
            internalRpcType: typeof window.muon.executor.__spawnRpc,
          };
        })()`);
        expect(values).toEqual({
          alphaBefore: "alpha",
          alphaAfter: "alpha",
          beta: "beta",
          frozen: {
            muon: true,
            test: true,
            alpha: true,
            alphaName: true,
            beta: true,
            executor: true,
            executorSpawn: true,
          },
          mutationResults: {
            addRootProperty: "TypeError",
            deleteFunction: "TypeError",
            replaceFunction: "TypeError",
            addFunctionProperty: "TypeError",
          },
          alphaNameUnchanged: true,
          rootExtraType: "undefined",
          functionExtraType: "undefined",
          alphaKeys: ["alphaAdd", "alphaConfig", "alphaName"],
          executorKeys: [
            "boolType",
            "bufferViewType",
            "float32Type",
            "float64Type",
            "int16Type",
            "int32Type",
            "int64Type",
            "int8Type",
            "loadLibrary",
            "pointerType",
            "spawn",
            "stringType",
            "uint16Type",
            "uint32Type",
            "uint64Type",
            "uint8Type",
            "usizeType",
            "voidType",
          ],
          runType: "undefined",
          internalRpcType: "function",
        });
      },
    );
  });

  it("routes same-named functions through their namespaces", async () => {
    await withMuon(
      ["muon_test_plugin_same_name_alpha", "muon_test_plugin_same_name_beta"],
      async (driver) => {
        await expect(
          driver.evaluate("window.muon.test.sameNameAlpha.sharedName()"),
        ).resolves.toBe("same-alpha");
        await expect(
          driver.evaluate("window.muon.test.sameNameBeta.sharedName()"),
        ).resolves.toBe("same-beta");
      },
    );
  });

  it("routes namespaces exposed by a single plugin library", async () => {
    await withMuon(["muon_test_plugin_multi_namespace"], async (driver) => {
      await expect(
        driver.evaluate("window.muon.test.multiA.name()"),
      ).resolves.toBe("multi-a");
      await expect(
        driver.evaluate("window.muon.test.multiB.name()"),
      ).resolves.toBe("multi-b");
    });
  });

  it("fails startup when plugin namespaces match exactly", async () => {
    await expectDebugMuonStartupFailure(
      ["muon_test_plugin_alpha", "muon_test_plugin_duplicate_namespace"],
      "Duplicate plugin namespace: muon.test.alpha",
    );
  });

  it("fails startup when an external plugin reuses the built-in browser namespace", async () => {
    await expectDebugMuonStartupFailure(
      ["muon_test_plugin_builtin_browser_conflict"],
      "Reserved plugin namespace: muon.browser",
    );
  });

  it("fails startup when external plugins reuse the filesystem dialogs namespace", async () => {
    await expectDebugMuonStartupFailure(
      ["muon_test_plugin_builtin_fs_dialogs_conflict"],
      "Reserved plugin namespace: muon.fs.dialogs",
    );
  });

  it("fails startup when namespaces match inside one plugin library", async () => {
    await expectDebugMuonStartupFailure(
      ["muon_test_plugin_duplicate_namespace_in_plugin"],
      "Duplicate plugin namespace: muon.test.duplicateInside",
    );
  });

  it("fails startup when a function path is reused as a namespace path", async () => {
    await expectDebugMuonStartupFailure(
      [
        "muon_test_plugin_alpha",
        "muon_test_plugin_function_path_namespace_conflict",
      ],
      "Plugin namespace path conflicts with a function path: muon.test.alpha.alphaName",
    );
  });

  it("fails startup when one plugin library has a namespace path conflict", async () => {
    await expectDebugMuonStartupFailure(
      ["muon_test_plugin_namespace_path_conflict_in_plugin"],
      "Plugin namespace path conflicts with a function path: muon.test.inner.leaf",
    );
  });

  it("fails startup when a plugin namespace is invalid", async () => {
    await expectDebugMuonStartupFailure(
      ["muon_test_plugin_invalid_namespace"],
      "Plugin namespace is invalid: muon.test.invalid-name",
    );
  });

  it("exposes host closure helpers to plugins", async () => {
    await withMuon(["muon_test_plugin_helpers"], async (driver) => {
      await expect(
        driver.evaluate("window.muon.test.helpers.helperClosureAdd(23)"),
      ).resolves.toBe(123);
      await expect(
        driver.evaluate(
          "(async () => typeof (await window.muon.test.helpers.helperClosureRelease()))()",
        ),
      ).resolves.toBe("undefined");
      await expect(
        driver.evaluate(
          "window.muon.test.helpers.helperCompletionAdd((value) => value * 2, 21)",
        ),
      ).resolves.toBe(43);
      await expect(
        driver.evaluate(`window.muon.test.helpers.helperCompletionAdd(
          async (value) => {
            await Promise.resolve();
            return value * 2;
          },
          21
        )`),
      ).resolves.toBe(43);
      await expect(
        evaluateRejection(
          driver,
          "window.muon.test.helpers.helperCompletionAdd(() => 'wrong', 5)",
        ),
      ).resolves.toBe("Renderer function returned a non-i32 value");
      await expect(
        evaluateRejection(
          driver,
          "window.muon.test.helpers.helperCompletionAdd(async () => 'wrong', 5)",
        ),
      ).resolves.toBe("Renderer function returned a non-i32 value");
      await expect(
        evaluateRejection(
          driver,
          "window.muon.test.helpers.helperCompletionAdd(() => Promise.reject(new Error('helper rejected')), 5)",
        ),
      ).resolves.toBe("helper rejected");
    });
  });

  it("keeps function pointer identity stable across async plugin calls", async () => {
    await withMuon(["muon_test_plugin_function_lifetime"], async (driver) => {
      await expect(
        driver.evaluate(`(() => {
          const callback = () => undefined;
          return window.muon.test.functionLifetime.lifetimeSamePointer(callback, callback);
        })()`),
      ).resolves.toBe(true);
      await expect(
        driver.evaluate(`(() => {
          const first = () => undefined;
          const second = () => undefined;
          return window.muon.test.functionLifetime.lifetimeDifferentPointer(first, second);
        })()`),
      ).resolves.toBe(true);
      await expect(
        driver.evaluate(`(() => {
          const callback = () => undefined;
          return window.muon.test.functionLifetime.lifetimeAsyncSamePointer(callback, callback);
        })()`),
      ).resolves.toBe(true);
    });
  });

  it("releases fresh renderer callbacks after completed plugin calls", async () => {
    await withMuon(["muon_test_plugin_function_lifetime"], async (driver) => {
      const baseline = await readFunctionWrapperDiagnostics(driver);
      await expect(
        driver.evaluate(`(async () => {
          const references = [];
          for (let index = 0; index < 3; index += 1) {
            let callback = () => index;
            references.push(new WeakRef(callback));
            const isNull = await window.muon.test.functionLifetime.lifetimeNullPointer(callback);
            if (isNull) {
              throw new Error("renderer callback unexpectedly decoded as null");
            }
            callback = null;
          }
          globalThis.__muonCompletedCallbackReferences = references;
          return references.length;
        })()`),
      ).resolves.toBe(3);

      await driver.send("HeapProfiler.collectGarbage");
      await expect(
        driver.evaluate(
          "globalThis.__muonCompletedCallbackReferences.filter((reference) => reference.deref() !== undefined).length",
        ),
      ).resolves.toBe(0);
      await waitForFunctionWrapperDiagnosticBaseline(
        driver,
        baseline,
        "fresh renderer callbacks",
      );
    });
  });

  it("releases duplicated renderer callbacks after overlapping calls", async () => {
    await withMuon(["muon_test_plugin_function_lifetime"], async (driver) => {
      await expect(
        driver.evaluate(`(async () => {
          let callback = () => undefined;
          globalThis.__muonOverlappingCallbackReference = new WeakRef(callback);
          const results = await Promise.all([
            window.muon.test.functionLifetime.lifetimeOverlapSamePointer(
              callback,
              callback,
            ),
            window.muon.test.functionLifetime.lifetimeOverlapSamePointer(
              callback,
              callback,
            ),
          ]);
          callback = null;
          return results;
        })()`),
      ).resolves.toEqual([true, true]);

      await driver.send("HeapProfiler.collectGarbage");
      await expect(
        driver.evaluate(
          "globalThis.__muonOverlappingCallbackReference.deref() === undefined",
        ),
      ).resolves.toBe(true);
    });
  });

  it("releases renderer callback transfers after argument encoding fails", async () => {
    await withMuon(["muon_test_plugin_function_lifetime"], async (driver) => {
      await expect(
        driver.evaluate(`(async () => {
          let callback = () => undefined;
          globalThis.__muonFailedEncodingCallbackReference = new WeakRef(callback);
          try {
            await window.muon.test.functionLifetime.lifetimeSamePointer(
              callback,
              1,
            );
            return "resolved";
          } catch (error) {
            return String(error && error.message ? error.message : error);
          } finally {
            callback = null;
          }
        })()`),
      ).resolves.toBe("Invalid argument 1: expected function");

      await driver.send("HeapProfiler.collectGarbage");
      await expect(
        driver.evaluate(
          "globalThis.__muonFailedEncodingCallbackReference.deref() === undefined",
        ),
      ).resolves.toBe(true);
    });
  });

  it("marshals null function values", async () => {
    await withMuon(["muon_test_plugin_function_lifetime"], async (driver) => {
      const values = await driver.evaluate(`(async () => {
        const inputNull = await window.muon.test.functionLifetime.lifetimeNullPointer(null);
        const inputUndefined = await window.muon.test.functionLifetime.lifetimeNullPointer(undefined);
        const returned = await window.muon.test.functionLifetime.lifetimeReturnNullFunction();
        const callbackNull = await window.muon.test.functionLifetime.lifetimeNullCallbackRoundtrip(
          (value) => {
            if (value !== null) {
              throw new Error("expected null function argument");
            }
            return null;
          },
        );
        const callbackUndefined = await window.muon.test.functionLifetime.lifetimeNullCallbackRoundtrip(
          (value) => {
            if (value !== null) {
              throw new Error("expected null function argument");
            }
            return undefined;
          },
        );
        return {
          inputNull,
          inputUndefined,
          returned,
          callbackNull,
          callbackUndefined,
        };
      })()`);

      expect(values).toEqual({
        inputNull: true,
        inputUndefined: true,
        returned: null,
        callbackNull: null,
        callbackUndefined: null,
      });
    });
  });

  it("lets plugins retain function pointers beyond completion", async () => {
    await withMuon(["muon_test_plugin_function_lifetime"], async (driver) => {
      const baseline = await readFunctionWrapperDiagnostics(driver);
      const retained = await driver.evaluate(`(async () => {
        let callback = () => undefined;
        globalThis.__muonRetainedCallbackReference = new WeakRef(callback);
        const retained = await window.muon.test.functionLifetime.lifetimeRetain(callback);
        const matched = await window.muon.test.functionLifetime.lifetimeRetainedMatches(callback);
        callback = null;
        return { retained, matched };
      })()`);

      await driver.send("HeapProfiler.collectGarbage");
      const retainedType = await driver.evaluate(
        "typeof globalThis.__muonRetainedCallbackReference.deref()",
      );

      await expect(
        driver.evaluate(
          "window.muon.test.functionLifetime.lifetimeFinalizeRetained()",
        ),
      ).resolves.toBeUndefined();
      await driver.send("HeapProfiler.collectGarbage");
      const released = await driver.evaluate(
        "globalThis.__muonRetainedCallbackReference.deref() === undefined",
      );

      expect(retained).toEqual({
        retained: true,
        matched: true,
      });
      expect(retainedType).toBe("function");
      expect(released).toBe(true);

      await expect(
        driver.evaluate(`(() => {
          const callback = () => undefined;
          return window.muon.test.functionLifetime.lifetimeAsyncRetainFinalize(
            callback,
          );
        })()`),
      ).resolves.toBe(true);
      await waitForFunctionWrapperDiagnosticBaseline(
        driver,
        baseline,
        "explicitly retained renderer callback",
      );
    });
  });

  it("rejects retained callbacks after their renderer context is released", async () => {
    await withMuon(["muon_test_plugin_function_lifetime"], async (driver) => {
      await expect(
        driver.evaluate(`(async () => {
          const callback = () => undefined;
          return await window.muon.test.functionLifetime.lifetimeRetain(callback);
        })()`),
      ).resolves.toBe(true);

      await driver.navigate(
        "data:text/html,<title>muon retained callback released</title>",
        cdpCommandTimeoutMs,
      );
      const rejection = await evaluateRejection(
        driver,
        "window.muon.test.functionLifetime.lifetimeInvokeRetained()",
      );
      await expect(
        driver.evaluate(
          "window.muon.test.functionLifetime.lifetimeFinalizeRetained()",
        ),
      ).resolves.toBeUndefined();
      expect([
        "Renderer function context is unavailable",
        "Renderer frame is unavailable",
      ]).toContain(rejection);
    });
  });

  it("marshals recursive function arguments and returns", async () => {
    await withMuon(["muon_test_plugin_recursive_functions"], async (driver) => {
      await expect(
        driver.evaluate(
          "window.muon.test.recursiveFunctions.recursiveInvoke((value) => value + 1)",
        ),
      ).resolves.toBe(42);
      await expect(
        driver.evaluate(
          "window.muon.test.recursiveFunctions.recursiveReturnFunction((base) => (value) => base + value)",
        ),
      ).resolves.toBe(42);
      await expect(
        driver.evaluate(
          "window.muon.test.recursiveFunctions.recursiveFunctionArgRoundtrip((fn) => fn)",
        ),
      ).resolves.toBe(42);
      await expect(
        driver.evaluate(`(async () => Array.from(new Uint8Array(
            await window.muon.test.recursiveFunctions.recursiveBufferReturnFunction((buffer) => {
              const first = Array.from(new Uint8Array(buffer));
              if (first.join(",") !== "12,13,14,15") {
                throw new Error("unexpected outer buffer");
              }
              return (innerBuffer) => {
                const second = Array.from(new Uint8Array(innerBuffer));
                if (second.join(",") !== "21,22,23,24") {
                  throw new Error("unexpected inner buffer");
                }
                return Uint8Array.from([201, 202, 203, 204]).buffer;
              };
            }),
          )))()`),
      ).resolves.toEqual([31, 32, 33, 34]);
    });
  });

  it("marshals every supported argument and return type", async () => {
    await withMuon(["muon_test_plugin_types"], async (driver) => {
      await expect(
        driver.evaluate("window.muon.test.types.echoBool(false)"),
      ).resolves.toBe(false);
      await expect(
        driver.evaluate("window.muon.test.types.echoI8(-128)"),
      ).resolves.toBe(-128);
      await expect(
        driver.evaluate("window.muon.test.types.echoU8(255)"),
      ).resolves.toBe(255);
      await expect(
        driver.evaluate("window.muon.test.types.echoI16(-32768)"),
      ).resolves.toBe(-32768);
      await expect(
        driver.evaluate("window.muon.test.types.echoU16(65535)"),
      ).resolves.toBe(65535);
      await expect(
        driver.evaluate("window.muon.test.types.echoI32(-2147483648)"),
      ).resolves.toBe(-2147483648);
      await expect(
        driver.evaluate("window.muon.test.types.echoU32(4294967295)"),
      ).resolves.toBe(4294967295);
      const i64Values = await driver.evaluate(`(async () => {
        const i64Safe = await window.muon.test.types.echoI64(9007199254740991);
        const i64Truncated = await window.muon.test.types.echoI64(1.9);
        const i64NonFinite = await window.muon.test.types.echoI64(Infinity);
        const u64SignedMax = await window.muon.test.types.echoU64(-1);
        const u64Zero = await window.muon.test.types.echoU64(0);
        return {
          i64SafeType: typeof i64Safe,
          i64Safe,
          i64Truncated,
          i64NonFinite,
          u64SignedMaxType: typeof u64SignedMax,
          u64SignedMax,
          u64Zero,
        };
      })()`);
      expect(i64Values).toEqual({
        i64SafeType: "number",
        i64Safe: 9007199254740991,
        i64Truncated: 1,
        i64NonFinite: 0,
        u64SignedMaxType: "number",
        u64SignedMax: -1,
        u64Zero: 0,
      });
      const f32Values = await driver.evaluate(`(async () => {
        const f32MaxSource = 3.4028234663852886e38;
        const f32MinSource = -3.4028234663852886e38;
        const f32Max = await window.muon.test.types.echoF32(f32MaxSource);
        const f32Min = await window.muon.test.types.echoF32(f32MinSource);
        return {
          value: await window.muon.test.types.echoF32(1.25),
          maxMatches: f32Max === Math.fround(f32MaxSource),
          minMatches: f32Min === Math.fround(f32MinSource),
        };
      })()`);
      expect(f32Values).toEqual({
        value: 1.25,
        maxMatches: true,
        minMatches: true,
      });
      await expect(
        driver.evaluate("window.muon.test.types.echoF64(1.25)"),
      ).resolves.toBe(1.25);
      const pointerValues = await driver.evaluate<{
        bitSize: number;
        zero: number;
        zeroType: string;
        safe: number;
        safeType: string;
        wide: number;
        wideType: string;
        wideSource: number;
        inputNull: number;
        inputUndefined: number;
        returnedNull: number;
      }>(`(async () => {
        const bitSize = await window.muon.test.types.pointerBitSize();
        const wideSource = bitSize > 32 ? 4294967296 : 2147483647;
        const zero = await window.muon.test.types.echoPointer(0);
        const safe = await window.muon.test.types.echoPointer(4096);
        const wide = await window.muon.test.types.echoPointer(wideSource);
        const inputNull = await window.muon.test.types.echoPointer(null);
        const inputUndefined = await window.muon.test.types.echoPointer(undefined);
        const returnedNull = await window.muon.test.types.returnNullPointer();
        return {
          bitSize,
          zero,
          zeroType: typeof zero,
          safe,
          safeType: typeof safe,
          wide,
          wideType: typeof wide,
          wideSource,
          inputNull,
          inputUndefined,
          returnedNull,
        };
      })()`);
      expect(pointerValues).toEqual({
        bitSize: pointerValues.bitSize,
        zero: 0,
        zeroType: "number",
        safe: 4096,
        safeType: "number",
        wide: pointerValues.wideSource,
        wideType: "number",
        wideSource: pointerValues.bitSize > 32 ? 4294967296 : 2147483647,
        inputNull: 0,
        inputUndefined: 0,
        returnedNull: 0,
      });
      await expect(
        driver.evaluate('window.muon.test.types.echoString("hello")'),
      ).resolves.toBe("hello");
      const nullStrings = await driver.evaluate(`(async () => {
        const inputNull = await window.muon.test.types.echoString(null);
        const inputUndefined = await window.muon.test.types.echoString(undefined);
        const returned = await window.muon.test.types.returnNullString();
        const callbackNull = await window.muon.test.types.stringNullCallbackRoundtrip(
          (value) => {
            if (value !== null) {
              throw new Error("expected null string argument");
            }
            return null;
          },
        );
        const callbackUndefined = await window.muon.test.types.stringNullCallbackRoundtrip(
          (value) => {
            if (value !== null) {
              throw new Error("expected null string argument");
            }
            return undefined;
          },
        );
        return {
          inputNull,
          inputUndefined,
          returned,
          callbackNull,
          callbackUndefined,
        };
      })()`);
      expect(nullStrings).toEqual({
        inputNull: null,
        inputUndefined: null,
        returned: null,
        callbackNull: null,
        callbackUndefined: null,
      });
      const int64Callbacks = await driver.evaluate(`(async () => {
        const i64 = await window.muon.test.types.i64CallbackRoundtrip((value) => {
          if (typeof value !== "number" || value !== -42) {
            throw new Error("expected i64 number argument");
          }
          return value + 1;
        }, -42.8);
        const u64 = await window.muon.test.types.u64CallbackRoundtrip((value) => {
          if (typeof value !== "number" || value !== -1) {
            throw new Error("expected u64 number argument");
          }
          return value;
        }, -1);
        return {
          i64Type: typeof i64,
          i64,
          u64Type: typeof u64,
          u64,
        };
      })()`);
      expect(int64Callbacks).toEqual({
        i64Type: "number",
        i64: -41,
        u64Type: "number",
        u64: -1,
      });
      const pointerCallback = await driver.evaluate<{
        type: string;
        result: number;
        expected: number;
      }>(`(async () => {
        const bitSize = await window.muon.test.types.pointerBitSize();
        const source = bitSize > 32 ? 4294967296 : 4096;
        const result = await window.muon.test.types.pointerCallbackRoundtrip((value) => {
          if (typeof value !== "number" || value !== source) {
            throw new Error("expected pointer number argument");
          }
          return value + 1;
        }, source);
        return {
          type: typeof result,
          result,
          expected: source + 1,
        };
      })()`);
      expect(pointerCallback).toEqual({
        type: "number",
        result: pointerCallback.expected,
        expected: pointerCallback.expected,
      });
      const bufferValues = await driver.evaluate(`(async () => {
        const source = Uint8Array.from([3, 1, 4, 1, 5, 9, 2, 6]);
        const checksum = await window.muon.test.types.bufferChecksum(source.buffer);
        const transformed = Array.from(
          new Uint8Array(await window.muon.test.types.transformBuffer(source.buffer)),
        );
        const normal = Array.from(
          new Uint8Array(await window.muon.test.types.returnNormalBuffer()),
        );
        const shared = Array.from(
          new Uint8Array(await window.muon.test.types.returnSharedBuffer()),
        );
        const subarraySource = Uint8Array.from([99, 10, 20, 30, 88]);
        const subarrayChecksum = await window.muon.test.types.bufferChecksum(
          subarraySource.subarray(1, 4),
        );
        const dataViewSource = Uint8Array.from([1, 2, 40, 50, 60, 7]);
        const dataViewChecksum = await window.muon.test.types.bufferChecksum(
          new DataView(dataViewSource.buffer, 2, 3),
        );
        const originalBeforeMutation = Array.from(source);
        const mutated = Array.from(
          new Uint8Array(await window.muon.test.types.mutateBufferCopy(source.buffer)),
        );
        const originalAfterMutation = Array.from(source);
        const callbackResult = await window.muon.test.types.bufferCallbackRoundtrip(
          (buffer) => {
            const received = Array.from(new Uint8Array(buffer));
            if (received.join(",") !== "7,8,9,10") {
              throw new Error("unexpected callback buffer");
            }
            return Uint8Array.from([101, 102, 103, 104]).buffer;
          },
        );
        const proxy = await window.muon.test.types.returnBufferFunction();
        let proxyResult = [];
        try {
          proxyResult = Array.from(
            new Uint8Array(
              await proxy(Uint8Array.from([10, 11, 12, 13, 14, 15, 16, 17])),
            ),
          );
        } finally {
          proxy.release();
        }
        return {
          checksum,
          transformed,
          normal,
          shared,
          subarrayChecksum,
          dataViewChecksum,
          originalBeforeMutation,
          mutated,
          originalAfterMutation,
          callbackResult,
          proxyResult,
        };
      })()`);
      expect(bufferValues).toEqual({
        checksum: 31,
        transformed: [163, 167, 172, 160, 164, 161, 164, 166],
        normal: [31, 34, 37, 40, 43, 46, 49, 52],
        shared: [91, 92, 93, 94, 95, 96, 97, 98],
        subarrayChecksum: 60,
        dataViewChecksum: 150,
        originalBeforeMutation: [3, 1, 4, 1, 5, 9, 2, 6],
        mutated: [94, 88, 95, 88, 92, 80, 89, 93],
        originalAfterMutation: [3, 1, 4, 1, 5, 9, 2, 6],
        callbackResult: 1,
        proxyResult: [27, 28, 29, 30, 31, 32, 33, 34],
      });
      await expect(
        driver.evaluate(
          "(async () => typeof (await window.muon.test.types.returnVoid()))()",
        ),
      ).resolves.toBe("undefined");
    });
  });

  it("releases plugin function proxy wrappers independently", async () => {
    await withMuon(["muon_test_plugin_types"], async (driver) => {
      const baseline = await readFunctionWrapperDiagnostics(driver);
      const values = await driver.evaluate<{
        sameObject: boolean;
        releaseDescriptor: {
          configurable: boolean;
          enumerable: boolean;
          type: string;
          writable: boolean;
        } | null;
        symbolDisposeDescriptor: {
          configurable: boolean;
          enumerable: boolean;
          type: string;
          writable: boolean;
        } | null;
        releaseEnumerated: boolean;
        releaseFunctionsMatch: boolean;
        releaseErrors: string[];
        releasedCall: {
          isPromise: boolean;
          message: string;
          status: string;
          synchronousThrow: boolean;
        };
        reargument: { message: string; status: string };
        secondCall: { message: string; result: number[]; status: string };
        secondReleaseError: string;
        oldDisposeType: string;
      }>(`(async () => {
        const first = await window.muon.test.types.returnBufferFunction();
        const second = await window.muon.test.types.returnBufferFunction();
        const descriptor = Object.getOwnPropertyDescriptor(first, "release");
        const releaseDescriptor = descriptor === undefined
          ? null
          : {
              configurable: descriptor.configurable,
              enumerable: descriptor.enumerable,
              type: typeof descriptor.value,
              writable: descriptor.writable,
            };
        const symbolDescriptor = Object.getOwnPropertyDescriptor(
          first,
          Symbol.dispose,
        );
        const symbolDisposeDescriptor = symbolDescriptor === undefined
          ? null
          : {
              configurable: symbolDescriptor.configurable,
              enumerable: symbolDescriptor.enumerable,
              type: typeof symbolDescriptor.value,
              writable: symbolDescriptor.writable,
            };
        const releaseErrors = [];
        try {
          first.release();
          first[Symbol.dispose]();
        } catch (error) {
          releaseErrors.push(
            String(error && error.message ? error.message : error),
          );
        }

        const releasedCall = {
          isPromise: false,
          message: "",
          status: "",
          synchronousThrow: false,
        };
        try {
          const result = first(
            Uint8Array.from([10, 11, 12, 13, 14, 15, 16, 17]),
          );
          releasedCall.isPromise = result instanceof Promise;
          try {
            await result;
            releasedCall.status = "fulfilled";
          } catch (error) {
            releasedCall.status = "rejected";
            releasedCall.message = String(
              error && error.message ? error.message : error,
            );
          }
        } catch (error) {
          releasedCall.synchronousThrow = true;
          releasedCall.status = "threw";
          releasedCall.message = String(
            error && error.message ? error.message : error,
          );
        }

        const reargument = { message: "", status: "" };
        try {
          await window.muon.test.types.bufferCallbackRoundtrip(first);
          reargument.status = "fulfilled";
        } catch (error) {
          reargument.status = "rejected";
          reargument.message = String(
            error && error.message ? error.message : error,
          );
        }

        const secondCall = { message: "", result: [], status: "" };
        try {
          secondCall.result = Array.from(
            new Uint8Array(
              await second(
                Uint8Array.from([10, 11, 12, 13, 14, 15, 16, 17]),
              ),
            ),
          );
          secondCall.status = "fulfilled";
        } catch (error) {
          secondCall.status = "rejected";
          secondCall.message = String(
            error && error.message ? error.message : error,
          );
        }

        let secondReleaseError = "";
        try {
          second[Symbol.dispose]();
        } catch (error) {
          secondReleaseError = String(
            error && error.message ? error.message : error,
          );
        }
        return {
          sameObject: first === second,
          releaseDescriptor,
          symbolDisposeDescriptor,
          releaseEnumerated: Object.keys(first).includes("release"),
          releaseFunctionsMatch: first.release === first[Symbol.dispose],
          releaseErrors,
          releasedCall,
          reargument,
          secondCall,
          secondReleaseError,
          oldDisposeType: typeof first.dispose,
        };
      })()`);

      expect(values.sameObject).toBe(false);
      expect(values.releaseDescriptor).toEqual({
        configurable: false,
        enumerable: false,
        type: "function",
        writable: false,
      });
      expect(values.symbolDisposeDescriptor).toEqual({
        configurable: false,
        enumerable: false,
        type: "function",
        writable: false,
      });
      expect(values.releaseEnumerated).toBe(false);
      expect(values.releaseFunctionsMatch).toBe(true);
      expect(values.releaseErrors).toEqual([]);
      expect(values.releasedCall.synchronousThrow).toBe(false);
      expect(values.releasedCall.isPromise).toBe(true);
      expect(values.releasedCall.status).toBe("rejected");
      expect(values.releasedCall.message.toLowerCase()).toContain("released");
      expect(values.reargument.status).toBe("rejected");
      expect(values.reargument.message.toLowerCase()).toContain("released");
      expect(values.secondCall).toEqual({
        message: "",
        result: [27, 28, 29, 30, 31, 32, 33, 34],
        status: "fulfilled",
      });
      expect(values.secondReleaseError).toBe("");
      expect(values.oldDisposeType).toBe("undefined");
      await waitForFunctionWrapperDiagnosticBaseline(
        driver,
        baseline,
        "released plugin function proxies",
      );
    });
  });

  it("rejects plugin function proxy use from another V8 context", async () => {
    const pluginNames = ["muon_test_plugin_recursive_functions"];
    const running = await startDebugMuon(
      pluginNames,
      TEST_NETWORK_ALLOW_PATTERNS,
      {},
      undefined,
      TEST_PLUGIN_ALLOW_PATTERNS,
      pluginNames,
      TEST_BROWSER_PLUGIN_ALLOW_PATTERNS,
      [],
      ["asset://main/**"],
    );
    let driver: CdpDriver | undefined = undefined;
    const popupDrivers: CdpDriver[] = [];
    try {
      driver = await connectToMuonCdp({
        port: MUON_PORT,
        timeoutMs: cdpCommandTimeoutMs,
      });
      await expect(
        driver.evaluate(`(async () => {
          let proxy;
          const roundtrip = await window.muon.test.recursiveFunctions
            .recursiveFunctionArgRoundtrip((value) => {
              proxy = value;
              return value;
            });
          if (roundtrip !== 42 || typeof proxy !== "function") {
            throw new Error("unexpected recursive function result");
          }
          globalThis.__muonCrossContextProxy = proxy;
          return roundtrip;
        })()`),
      ).resolves.toBe(42);

      const popupTarget = await openPopupTarget(
        driver,
        MUON_APP_URL,
        "",
        "muonProxyContext",
      );
      const popupDriver = await connectToMuonCdp({
        port: MUON_PORT,
        targetId: popupTarget.id,
        timeoutMs: cdpCommandTimeoutMs,
      });
      popupDrivers.push(popupDriver);
      const result = await popupDriver.evaluate<{
        releaseMessage: string;
        releaseStatus: string;
        invokeMessage: string;
        invokeStatus: string;
        invokeValue: number | null;
        reargumentMessage: string;
        reargumentStatus: string;
        reargumentValue: number | null;
      }>(`(async () => {
          if (window.opener === null) {
            throw new Error("proxy owner window is unavailable");
          }
          const proxy = window.opener.__muonCrossContextProxy;
          if (typeof proxy !== "function") {
            throw new Error("proxy owner value is unavailable");
          }
          const result = {
            releaseMessage: "",
            releaseStatus: "fulfilled",
            invokeMessage: "",
            invokeStatus: "fulfilled",
            invokeValue: null,
            reargumentMessage: "",
            reargumentStatus: "fulfilled",
            reargumentValue: null,
          };
          try {
            result.invokeValue = await proxy(41);
          } catch (error) {
            result.invokeStatus = "rejected";
            result.invokeMessage = String(
              error && error.message ? error.message : error,
            );
          }
          try {
            result.reargumentValue = await window.muon.test
              .recursiveFunctions.recursiveInvoke(proxy);
          } catch (error) {
            result.reargumentStatus = "rejected";
            result.reargumentMessage = String(
              error && error.message ? error.message : error,
            );
          }
          try {
            proxy.release();
          } catch (error) {
            result.releaseStatus = "threw";
            result.releaseMessage = String(
              error && error.message ? error.message : error,
            );
          }
          return result;
        })()`);

      expect(result.invokeStatus).toBe("rejected");
      expect(result.invokeValue).toBeNull();
      expect(result.invokeMessage.toLowerCase()).toContain("context");
      expect(result.reargumentStatus).toBe("rejected");
      expect(result.reargumentValue).toBeNull();
      expect(result.reargumentMessage.toLowerCase()).toContain("context");
      expect(result.releaseStatus).toBe("threw");
      expect(result.releaseMessage.toLowerCase()).toContain("context");
    } catch (error) {
      throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
    } finally {
      if (driver !== undefined) {
        try {
          await driver.evaluate(`(() => {
            const proxy = globalThis.__muonCrossContextProxy;
            if (typeof proxy === "function") {
              proxy.release();
            }
            delete globalThis.__muonCrossContextProxy;
          })()`);
        } catch {
          // The owner context may already be unavailable during cleanup.
        }
      }
      for (const popupDriver of popupDrivers) {
        popupDriver.close();
      }
      await stopMuon(running, driver);
    }
  });

  it("releases plugin function proxy wrappers after garbage collection", async () => {
    await withMuon(["muon_test_plugin_recursive_functions"], async (driver) => {
      const baseline = await readFunctionWrapperDiagnostics(driver);
      await expect(
        driver.evaluate(`(() => {
          globalThis.__muonPluginProxyReady = false;
          globalThis.__muonPluginProxyError = "";
          globalThis.__muonPluginProxyType = "";
          setTimeout(async () => {
            try {
              const value = await window.muon.test.recursiveFunctions
                .recursiveFunctionArgRoundtrip((proxy) => {
                  globalThis.__muonPluginProxyReference = new WeakRef(proxy);
                  globalThis.__muonPluginProxyType = typeof proxy;
                  return proxy;
                });
              if (value !== 42) {
                throw new Error("unexpected recursive function result");
              }
            } catch (error) {
              globalThis.__muonPluginProxyError = String(
                error && error.message ? error.message : error,
              );
            } finally {
              setTimeout(() => {
                globalThis.__muonPluginProxyReady = true;
              }, 0);
            }
          }, 0);
          return "scheduled";
        })()`),
      ).resolves.toBe("scheduled");

      let ready = false;
      for (let attempt = 0; attempt < 20 && !ready; attempt += 1) {
        ready = await driver.evaluate<boolean>(
          "globalThis.__muonPluginProxyReady",
        );
      }
      expect(ready).toBe(true);
      await expect(
        driver.evaluate("globalThis.__muonPluginProxyError"),
      ).resolves.toBe("");
      await expect(
        driver.evaluate("globalThis.__muonPluginProxyType"),
      ).resolves.toBe("function");

      let collected = false;
      for (let attempt = 0; attempt < 3 && !collected; attempt += 1) {
        await driver.send("HeapProfiler.collectGarbage");
        collected = await driver.evaluate<boolean>(
          "globalThis.__muonPluginProxyReference.deref() === undefined",
        );
      }
      expect(collected).toBe(true);
      await waitForFunctionWrapperDiagnosticBaseline(
        driver,
        baseline,
        "garbage-collected plugin function proxy",
      );
    });
  });

  it("resolves asynchronous plugin completions once", async () => {
    await withMuon(["muon_test_plugin_types"], async (driver) => {
      await expect(
        driver.evaluate("window.muon.test.types.resolveAsync(41)"),
      ).resolves.toBe(42);
      await expect(
        driver.evaluate("window.muon.test.types.resolveTwice()"),
      ).resolves.toBe("first");
    });
  });

  it("shares the cardio dispatcher state with external plugins", async () => {
    await withMuon(["muon_test_plugin_cardio"], async (driver) => {
      await expect(
        driver.evaluate("window.muon.test.cardio.dispatcherAvailableAtInit()"),
      ).resolves.toBe(true);
      await expect(
        driver.evaluate("window.muon.test.cardio.dispatcherAvailable()"),
      ).resolves.toBe(true);
    });
  });

  it("waits for asynchronous plugin shutdown before unloading", async () => {
    const markerDirectory = await mkdtemp(
      join(tmpdir(), "muon-plugin-stop-marker-"),
    );
    const markerPath = join(markerDirectory, "marker.txt");
    try {
      await withMuonEnvironment(
        ["muon_test_plugin_cardio"],
        { MUON_TEST_PLUGIN_STOP_MARKER: markerPath },
        async (driver) => {
          await expect(
            driver.evaluate(
              "window.muon.test.cardio.dispatcherAvailableAtInit()",
            ),
          ).resolves.toBe(true);
        },
      );
      await expect(readFile(markerPath, "utf8")).resolves.toBe(
        "stop-started\nstop-completed\nunloaded\n",
      );
    } finally {
      await rm(markerDirectory, { recursive: true, force: true });
    }
  });

  it("balances libffi completion closures after repeated completions", async () => {
    await withTrackedMuon(["muon_test_plugin_types"], async (driver) => {
      const result = await driver.evaluate<{
        total: number;
        rejected: number;
        lastTwice: string;
      }>(`(async () => {
        const iterationCount = 40;
        let total = 0;
        let rejected = 0;
        let lastTwice = "";
        for (let index = 0; index < iterationCount; index += 1) {
          const [syncValue, asyncValue, twiceValue, voidValue] = await Promise.all([
            window.muon.test.types.echoI32(index),
            window.muon.test.types.resolveAsync(index),
            window.muon.test.types.resolveTwice(),
            window.muon.test.types.returnVoid(),
          ]);
          if (typeof voidValue !== "undefined") {
            throw new Error("returnVoid resolved with a value");
          }
          total += syncValue + asyncValue;
          lastTwice = twiceValue;
          try {
            await window.muon.test.types.rejectValue();
          } catch (error) {
            const message = String(error && error.message ? error.message : error);
            if (message !== "plugin failure") {
              throw error;
            }
            rejected += 1;
          }
        }
        return { total, rejected, lastTwice };
      })()`);

      expect(result).toEqual({
        total: 1600,
        rejected: 40,
        lastTwice: "first",
      });
    });
  });

  it("rejects invalid calls and plugin failures", async () => {
    await withMuon(["muon_test_plugin_types"], async (driver) => {
      const failures = await driver.evaluate<string[]>(`(async () => {
        const pointerBitSize = await window.muon.test.types.pointerBitSize();
        const calls = [
          () => window.muon.test.types.echoI32(),
          () => window.muon.test.types.echoI32(1, 2),
          () => window.muon.test.types.echoBool(1),
          () => window.muon.test.types.echoI8(-129),
          () => window.muon.test.types.echoU8(256),
          () => window.muon.test.types.echoI16(32768),
          () => window.muon.test.types.echoU16(65536),
          () => window.muon.test.types.echoI32(1.5),
          () => window.muon.test.types.echoU32(-1),
          () => window.muon.test.types.echoU32(4294967296),
          () => window.muon.test.types.echoI64(1n),
          () => window.muon.test.types.echoI64("1"),
          () => window.muon.test.types.echoU64(1n),
          () => window.muon.test.types.echoU64("1"),
          () => window.muon.test.types.echoF32(3.5e38),
          () => window.muon.test.types.echoF32(NaN),
          () => window.muon.test.types.echoF64(NaN),
          () => window.muon.test.types.echoPointer(-1),
          () => window.muon.test.types.echoPointer(1.5),
          () => window.muon.test.types.echoPointer(Infinity),
          () => window.muon.test.types.echoPointer("1"),
          () => window.muon.test.types.echoPointer(Symbol()),
          () => window.muon.test.types.echoPointer(
            pointerBitSize > 32 ? 18446744073709551616 : 4294967296,
          ),
          () => window.muon.test.types.bufferChecksum(null),
          () => window.muon.test.types.bufferChecksum(undefined),
          () => window.muon.test.types.bufferChecksum({}),
        ];
        const results = [];
        for (const call of calls) {
          try {
            await call();
            results.push("resolved");
          } catch (error) {
            results.push(String(error && error.message ? error.message : error));
          }
        }
        return results;
      })()`);
      expect(failures.every((message) => message !== "resolved")).toBe(true);
      expect(failures.every((message) => message.includes("Invalid"))).toBe(
        true,
      );

      await expect(
        evaluateRejection(
          driver,
          "window.muon.test.types.i64CallbackRoundtrip(() => 1n, 1)",
        ),
      ).resolves.toBe("Renderer function returned a non-i64 value");
      await expect(
        evaluateRejection(
          driver,
          "window.muon.test.types.u64CallbackRoundtrip(() => 1n, 1)",
        ),
      ).resolves.toBe("Renderer function returned a non-u64 value");

      await expect(
        evaluateRejection(driver, "window.muon.test.types.rejectValue()"),
      ).resolves.toBe("plugin failure");
    });
  });
});

# Androidサポートの現状

## 1. 目的と調査基準

この文書は、2026年8月25日時点の`feature/android`ブランチについて、Android対応がどこまで実装、検証、公開されているかを整理した現状資料である。

「試作内で動くこと」と「muon-ui利用者向けの正式機能であること」を区別する。主な根拠は次のとおりである。

- 公開targetとbuild/pack実装: [muon-ui/src/targets.ts](muon-ui/src/targets.ts)、[muon-ui/src/build.ts](muon-ui/src/build.ts)、[muon-ui/src/pack.ts](muon-ui/src/pack.ts)
- Android試作の実装と実行方法: [muon-android-prototype](muon-android-prototype)、[muon-android-prototype/README.md](muon-android-prototype/README.md)
- Android APIの実装と方針: [muon-android-prototype/src/android-api.ts](muon-android-prototype/src/android-api.ts)、[android-api-compatibility.md](android-api-compatibility.md)
- WebViewネットワークの検証記録: [filter-limitation.md](filter-limitation.md)
- NDKプラグインの実装と端末検証記録: [plan.md](plan.md)
- 今回実行したリポジトリ全体テスト: `npm test`

## 2. 結論

現在のAndroid対応は、固定構成の試作アプリとしては、WebView、Muon RPC、組み込みAPI、NDKプラグイン、限定的なQuickJSランタイム、両ABIのAPK/AAB生成まで成立している。一方、公開`muon-ui`パッケージにはAndroid targetがなく、一般の利用者プロジェクトからAndroid成果物を作る正式なbuild/pack経路もない。

したがって、現在の位置付けは「技術試作として広い範囲を実装済み、利用者向けAndroidサポートとしては未公開」である。

| 項目                    | 現状                                        | 正式サポート判定                    |
| ----------------------- | ------------------------------------------- | ----------------------------------- |
| Android WebView backend | 固定試作アプリで実装済み                    | 未公開                              |
| public target           | Linux/Windowsの5 targetのみ                 | 未対応                              |
| ABI                     | `x86_64`、`arm64-v8a`を試作でbuild          | 試作内対応                          |
| Androidバージョン       | `minSdk 24`、`compileSdk/targetSdk 37`      | 試作内の固定値                      |
| APK/AAB/APKS            | debug/release APK、release AAB/APKSを生成   | 試作内対応                          |
| 本番署名                | releaseも標準debug keyを使用                | 未対応                              |
| Muon組み込みAPI         | browser、environments、fsの一部             | 試作内対応                          |
| NDKプラグイン           | build-time registryと5個のtest plugin       | 試作内対応                          |
| JavaScript runtime      | QuickJSによる限定Node.js風API               | 試作内対応、Node.js互換ではない     |
| `node.project`          | Android設定検証で拒否                       | 未対応                              |
| VM/実機                 | 基礎ランタイムは過去にVMとPixel 6で検証済み | 現行QuickJS追加後の再検証記録が不足 |
| 利用者向け文書          | 試作READMEと検証資料のみ                    | 未対応                              |

## 3. 公開muon-uiとの境界

### 3.1 Android targetは公開されていない

[targets.ts](muon-ui/src/targets.ts)の`MuonTarget`と`allMuonTargets`には、Linux 3種とWindows 2種だけがある。`android`、`android-arm64-v8a`、`android-x86_64`は存在しない。

[pack.ts](muon-ui/src/pack.ts)の成果物形式は`zip`、`tar.gz`、`deb`、`nsis`であり、`apk`、`aab`、`apks`は公開pack形式ではない。公開CLIの`muon build`と`muon pack`へAndroid SDK、NDK、Gradle、署名設定を渡す契約もない。

### 3.2 試作は一般の利用者プロジェクトを入力にしない

現在のAndroidアプリは[muon-android-prototype](muon-android-prototype)内の固定Gradle projectである。

- application IDとnamespaceは`dev.muon.prototype`で固定されている。
- Java package、AIDL、JNI、テスト、起動Activityも試作namespaceへ結び付いている。
- Web assetは同workspaceのVite出力を直接参照する。
- CMakeとbuild scriptはrepository root、`muon-core`、`deps/cardio`、`deps/tra-ffic`などの相対配置を前提とする。
- Android plugin registryは同repository内のC++ sourceだけを受け付ける。
- アプリ名、version、icon、theme、permission、asset originなどは利用者設定から生成されない。
- [android-config.ts](muon-android-prototype/src/android-config.ts)にはAndroid向け設定検証器があるが、合成済み`muon.json`を読み込む製品build pipelineには接続されていない。

このため、npmへpackした`muon-ui`だけをクリーンな利用者projectへ導入してAndroidアプリを生成することはできない。

## 4. Android試作で実装済みの範囲

### 4.1 固定ツールチェーンと対象環境

試作の現在値は次のとおりである。

| 項目                  | 値                                     |
| --------------------- | -------------------------------------- |
| Android API           | minSdk 24、compileSdk 37、targetSdk 37 |
| ABI                   | `x86_64`、`arm64-v8a`                  |
| Java                  | 17                                     |
| Android Gradle Plugin | 9.2.1                                  |
| Gradle Wrapper        | 9.4.1                                  |
| Build Tools           | 36.0.0                                 |
| Android NDK           | 29.0.14206865                          |
| CMake                 | 4.1.2                                  |
| bundletool            | 1.18.3                                 |
| cardio                | submodule 1.1.0                        |
| tra-ffic              | submodule 1.0.0                        |
| libffi                | 3.8.0                                  |
| QuickJS               | 2026-06-04                             |

QuickJS、libffi、bundletoolなどのdownloadはversionとSHA-256を固定し、使用前に検査する。native dependency manifestには入力commit、toolchain、configure引数、patch、成果物情報を記録する。

### 4.2 WebView、asset、RPC

試作はCEFを使わず、AndroidX WebKit上のWebViewをbackendとする。

- 起動ページは`https://main.asset.muon.invalid/index.html`で固定されている。
- 構成済みasset hostは`WebViewAssetLoader`が必ず処理し、存在しないassetはローカル404、GET/HEAD以外はローカル405にする。通常ネットワークへfallbackしない。
- file accessとcontent accessを無効化し、mixed contentを拒否する。
- `WEB_MESSAGE_LISTENER`、`WEB_MESSAGE_ARRAY_BUFFER`、`DOCUMENT_START_SCRIPT`と必要なService Worker機能がないWebView providerでは起動を失敗させる。
- RPCとJavaScript runtime bridgeは、完全一致する信頼済みoriginのmain frameからのmessageだけを受理する。
- Promise request/response、native error、cancel、文字列、scalar、64 bit整数、ArrayBuffer、callback、plugin proxy、context解放を扱う。
- Activity再生成時はWebViewごとに新しいRPC contextを作り、process内のnative plugin runtimeは定義済みのlifecycleに従って共有、停止、再生成する。

### 4.3 公開しているMuon API

Android simple modeの組み込み公開リストは[android-api.ts](muon-android-prototype/src/android-api.ts)に限定される。

| namespace           | 実装済み関数                                                                                                                                                                                                                                             |
| ------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `muon.browser`      | `reload`、`toggleFullscreen`、`enterFullscreen`、`exitFullscreen`、`zoomIn`、`zoomOut`、`resetZoom`、`close`                                                                                                                                             |
| `muon.environments` | `getVariables`、`getConfigValues`、`getProcessId`、`getRuntimeInfo`                                                                                                                                                                                      |
| `muon.fs`           | `readFile`、`writeFile`、`readTextFile`、`writeTextFile`、`stat`、`lstat`、`exists`、`access`、`readdir`、`mkdir`、`rm`、`unlink`、`rmdir`、`rename`、`copyFile`、`appendFile`、`appendTextFile`、`truncate`、`realpath`、`readlink`、`symlink`、`watch` |

主な制約は次のとおりである。

- `muon.fs`はAndroid appが実pathとしてアクセスできる範囲だけを扱う。Android sandboxを越える権限は与えない。
- `content://`、Storage Access Framework、shared storageとの接続はない。
- `muon.launcher`、`muon.executor`、`muon.fs.dialogs`は公開しない。
- desktop window、system tray、launcher updater、任意process起動、runtime外部library loadに相当するAPIは提供しない。
- desktopと意味が異なる設定は[android-config.ts](muon-android-prototype/src/android-config.ts)で拒否または警告するが、この検証器はまだ製品buildへ接続されていない。

### 4.4 WebViewのネットワーク契約

WebView自身の通常ネットワークはMuonが包括的にfilterしない。通信可否はAndroid Manifest、実行時permission、Network Security Config、WebViewのTLS/CORS/mixed-content処理、ページのCSPに従う。

- 試作Manifestは`INTERNET`と`ACCESS_LOCAL_NETWORK`を常に宣言する。
- Android 17、target SDK 37以降のLAN通信では、Activity側で`ACCESS_LOCAL_NETWORK`の実行時permissionを要求する必要がある。
- cleartextは既定拒否し、試験とloopback server用の`localhost`だけを例外にする。
- CEF版の`network.allow`、`network.authorizedOrigin`、`network.localAccess`はWebView通信にもQuickJS通信にも適用しない。
- Muon独自のsecurity境界は、構成済みasset hostのfail-closed処理と、RPCを信頼済みasset originのmain frameへ限定する処理である。

これはCEF版と同じネットワーク保証ではない。POST、redirect、WebSocket、Service WorkerなどをWebView本来の経路で利用できる代わりに、Muonによる宛先allowlistや要求元origin単位の制御を失う。

### 4.5 NDKプラグイン

Android pluginは[android-plugins.json](muon-android-prototype/android-plugins.json)でbuild前に確定し、両ABI向けnative libraryとしてAPK/AABへ同梱する。

- 現在は`alpha`、`cardio`、`function_lifetime`、`recursive_functions`、`types`の5 test pluginを登録する。
- `allow`とstring `config`をruntime metadataへ反映する。
- registry generatorは重複name/soname、unsupported ABI、不正source、空allowなどを拒否する。
- package verifierはABI、ELF machine、16 KiB LOAD alignment、`muon_init_plugin` export、package内容を検査する。
- 起動後に任意pathを探索、download、loadしない。
- desktop用の`plugin.path`、`signature`、`salt`はAndroidでは使用しない。

native runtimeはCEF非依存の`MuonPluginRuntime` core、cardio 1.1.0のAndroid dispatcher host、tra-ffic 1.0.0とlibffi 3.8.0を使う。x86_64の16 KiBページでは、libffi sourceのbuild用copyへmuon所有patchを適用し、外部submodule自体は変更しない。

### 4.6 組み込みQuickJS runtime

試作は`muon.node.createNode()`という呼び出し形で、非公開の`:muon_javascript` Service processに独立したQuickJS runtimeを作る。

このruntimeはNode.jsではない。API名は既存の生成形に合わせただけであり、Node.js version、V8、libuv、npm ecosystemとの互換性を意味しない。

実装済みの組み込みmoduleは次のとおりである。

| 分類               | module                                                                                      |
| ------------------ | ------------------------------------------------------------------------------------------- |
| filesystem/path    | `node:fs`、`node:fs/promises`、`node:path`                                                  |
| runtime primitives | `node:events`、`node:buffer`、`node:timers`、`node:timers/promises`                         |
| stream/URL         | `node:stream`、`node:stream/promises`、`node:url`                                           |
| environment        | `node:process`、`node:os`                                                                   |
| utility            | `node:util`、`node:assert`、`node:assert/strict`、`node:querystring`、`node:string_decoder` |
| crypto             | `node:crypto`のSHA-256、HMAC、乱数、UUID、timing-safe比較の限定実装                         |
| network            | `node:dns`、`node:dns/promises`、`node:net`、`node:http`、`node:https`                      |

各moduleは`node:`なしのaliasも持つ。`Buffer`、timer、EventTarget、AbortController、URL、fetch、process、限定版cryptoなどのglobalもある。

主な境界は次のとおりである。

- 同梱済みの固定ES moduleと組み込みmoduleだけをimportできる。
- `require`、CommonJS、`node_modules`探索、package `exports`、JSON module、npm、native addon、Node-API、REPLはない。
- `child_process`、`worker_threads`、`vm`、`v8`、`module`、`inspector`、`tls`、`http2`、`zlib`など、一覧外のmoduleは利用できない。
- `node:fs`は`files/javascript-runtime`配下の仮想rootへ閉じ込める。`content://`とshared storageは扱わない。
- TCP client、DNS、HTTP/HTTPS clientを提供する。TCP/HTTP serverはloopbackだけでlistenできる。
- HTTPSはcertificate検証を必須とし、`rejectUnauthorized: false`を認めない。
- renderer bridgeはprimitive、有限number、64 bit範囲のbigint、binary、JSON value、callbackを転送する。stateful objectをrendererへ直接返さない。
- process全体で同時16 runtime、runtimeごとに64 MiB heap、1 MiB native stack、2秒の連続JS実行などの上限を設ける。
- `release()`はmodule handle、timer、DNS、socket、listener、HTTP requestとnative runtimeを回収し、未完了処理をrejectする。

現在のapplication moduleは試験用[backend.mjs](muon-android-prototype/android/app/src/main/assets/muon-javascript/backend.mjs)で固定されている。一般の利用者projectを収集、bundle、検証して同梱するpipelineはない。また、Android設定検証器は引き続き`node.project`を拒否する。

## 5. Build、package、署名

試作workspaceの全体buildは次で実行する。

```bash
npm test --workspace muon-android-prototype
```

このcommandは次を行う。

1. Vitest、format、TypeScript、Vite build
2. 両ABIのnative dependency生成
3. QuickJS sourceの取得またはcache検証
4. Android plugin registry生成
5. debug/release APK、debug instrumentation APK、release AAB、release APKS生成
6. QuickJS、native dependency、registry、ELF、APK/AAB/APKS内容の検査

主な成果物は次のとおりである。

| 成果物            | path                                                                              |
| ----------------- | --------------------------------------------------------------------------------- |
| debug APK         | `muon-android-prototype/android/app/build/outputs/apk/debug/app-debug.apk`        |
| local release APK | `muon-android-prototype/android/app/build/outputs/apk/release/app-release.apk`    |
| release AAB       | `muon-android-prototype/android/app/build/outputs/bundle/release/app-release.aab` |
| release APKS      | `muon-android-prototype/android/app/build/outputs/apks/release/app-release.apks`  |

release成果物もローカル試験用の標準Android debug keyで署名する。本番keystore、alias、秘密情報供給、Play App Signing、production署名状態のreportingは未実装である。

## 6. テストと検証状況

### 6.1 今回の全体テスト

2026年8月25日にrepository rootで`npm test`を実行した。Android workspace部分は次のとおりPASSした。

- Android Vitest: 5 files、52 tests
- Viteのformat、TypeScript validation、production build
- `x86_64`と`arm64-v8a`のnative build
- debug/release APK、debug instrumentation APK、release AAB/APKSの生成
- QuickJS source、native dependency、plugin registry、release package verifier

repository全体では、既知の非Android課題であるWindows Settings uninstall E2Eがmenu item待機でtimeoutし、`muon-ui`は306/307件となった。Android workspaceに失敗はなかったが、root `npm test`の最終終了codeは1であり、全体GREENではない。

それ以外に確認できた主な結果は、`muon-node` 40件PASS、`muon-core` CTest 42/42件PASS、`muon-core-tester` 206件PASS・26件skipである。

### 6.2 端末接続gate

現在のsourceにはAndroid instrumentation testが合計44件ある。

| test class                         | 件数 | 主な範囲                                                         |
| ---------------------------------- | ---: | ---------------------------------------------------------------- |
| `MuonActivityTest`                 |   18 | asset、RPC、API、plugin、fs、Activity再生成、複数QuickJS runtime |
| `MuonJavaScriptRuntimeServiceTest` |   18 | module、fs、crypto、network、負荷、上限、異常JS、Service再起動   |
| `MuonNativeRuntimeTest`            |    5 | cardio、plugin lifecycle、libffi closure、startup failure        |
| `MuonNetworkFilterTest`            |    3 | 通常WebView通信、asset 404、callback制約                         |

接続gateは次の2種類である。

- `test:android`: Android 17/API 37、`x86_64`、実ページサイズ16 KiBのlocal emulator
- `test:android:pixel6`: Android 17/API 37、`arm64-v8a`、実ページサイズ4 KiBの物理Pixel 6

各gateは44件のinstrumentation、署名済みrelease APK、AAB由来端末別split APKを実行し、時間待ちではなくpage-ready eventを完了判定に使う。

[plan.md](plan.md)には、QuickJS拡張前のWebView、RPC、NDK plugin、cardio、tra-ffic/libffiについて、16 KiB x86_64 VMと4 KiB Pixel 6でPASSした記録がある。今回の調査時には接続端末がなかったため、QuickJSを含む現行44件は端末上で再実行していない。したがって、現行HEADについて端末gateがGREENであるとはこの文書では断定しない。

arm64-v8a成果物の16 KiB整列検査はあるが、16 KiBページのarm64実機またはVMでのruntime実行記録はない。

## 7. 現時点の不足と不整合

### 7.1 製品化に必要な不足

1. `android` public target、公開設定型、CLI引数、成果物型がない。
2. 試作Gradle/CMake/JNI/Java/assetを再利用可能なbackend packageへ分離していない。
3. 利用者のVite成果物、`muon.json`、metadata、icon、permission、plugin、JavaScript moduleを入力にするpipelineがない。
4. production署名、秘密情報管理、AAB store配布、成果物metadataがない。
5. npm packageへAndroid templateと全依存resourceを含め、クリーン環境でbuildする検証がない。
6. Android SDK/NDK/JDK/CMake/adb不足を公開CLIで事前診断しない。
7. 現行QuickJS追加後のVMとPixel 6の完全な端末回帰結果が記録されていない。
8. `content://`、file picker、permission UI、notification、foreground Service、app link、in-app updateなどのAndroid固有機能は未対応である。

### 7.2 JavaScript runtime契約の不整合

現在は`muon.node.createNode()`がQuickJSを返す一方、次の事実がある。

- runtimeはNode.jsではなく、限定互換moduleだけを提供する。
- `node.project`はAndroid設定検証で拒否する。
- [android-api-compatibility.md](android-api-compatibility.md)はNode.js sidecarと`createNode()`を非対応と記載したままである。
- public APIとしてNode.js互換範囲、versioning、feature detection、project packaging、migrationを定義していない。

正式公開前に、QuickJSをAndroid固有JavaScript runtimeとして別名で公開するか、限定Node.js互換facadeとして明示的に契約するか、実際のNode.js runtimeを採用するかを決める必要がある。現状のAPI名だけからNode.js互換と受け取れる状態は製品契約にできない。

### 7.3 文書の不整合

[muon-android-prototype/README.md](muon-android-prototype/README.md)は現在の試作に最も近い。一方、次の資料には履歴と現状が混在する。

- [android-api-compatibility.md](android-api-compatibility.md)は`x86_64`のみ、NDK plugin後続、Node.js API非対応というQuickJS追加前の記述を含む。
- [filter-limitation.md](filter-limitation.md)はdeny-all案、native HTTP代理案、通常ネットワーク採用後の確定方針を同じ文書に残す。
- rootの[README.md](README.md)と[README_ja.md](README_ja.md)はAndroidを利用者向けtargetとして案内しない。これは公開状態としては正しいが、正式対応時には更新が必要である。

## 8. 現状判定

Android supportを層ごとに判定すると次のとおりである。

1. 移植可能性: 成立している。
2. 固定試作アプリ: 広い範囲で実装済みである。
3. build時の両ABI成果物検査: 成立している。
4. 過去の基礎runtime VM/実機検証: 完了している。
5. 現行QuickJS込み端末回帰: sourceとgateはあるが、今回の調査では未実行である。
6. 一般のmuon-ui projectからのAndroid build/pack: 未対応である。
7. production配布と利用者向け正式support: 未対応である。

次に必要なのは試作機能をさらに無秩序に増やすことではなく、公開契約を確定し、試作を正式backendへ分離し、公開build/pack、署名、クリーンpackage検証、端末gate、利用者向け文書を一つの製品経路として接続することである。

# Android向けQuickJSの試作

Android WebView上の`muon.node.createNode()`から、別プロセスのQuickJSを利用する実験用アプリです。QuickJSの実装と固有テストをこのディレクトリにまとめています。

利用者アプリの作成は[Androidアプリのビルド・配布](../../docs/ja/android.md)を参照してください。共有ランタイムは[core/android](../android)を使用します。製品の回帰テストとテストプラグインは[core/android-test](../android-test)に分離しており、QuickJSは公開APKへ組み込みません。

## 対応環境

- Android API 24以降
- `x86_64`と`arm64-v8a`
- 4 KiBおよび16 KiBページに整列したnative artifact
- Java 17
- Android SDK Platform 37、Build Tools 36.0.0
- Android NDK 29.0.14206865、CMake 4.1.2
- root packageが指定するNode.jsとnpm dependency

repositoryはrecursive submoduleを取得した状態で使用してください。初回buildでは、固定した公式libffi 3.8.0 source archive、QuickJS 2026-06-04 source archive、[公式bundletool standalone jar](https://github.com/google/bundletool/releases/tag/1.18.3)をdownloadします。各downloadは使用前にSHA-256を検証します。native dependencyの生成manifestには入力commit、toolchain、configure引数、patch一覧も記録します。

## 組み込みJavaScript runtime

この試作のsimpleモードでは、Node.js版と同じ生成形の`muon.node.createNode()`から、独立したQuickJS runtimeを作成できます。

```javascript
const runtime = await muon.node.createNode();

try {
  const application = await runtime.importModule('.');
  console.log(await application.increment());

  const fs = await runtime.importModule('node:fs/promises');
  await fs.mkdir('example', { recursive: true });
  await fs.writeFile('example/message.txt', 'hello');
  console.log(await fs.readFile('example/message.txt', 'utf8'));
} finally {
  await runtime.release();
}
```

`createNode()`というAPI名は既存コードの生成形を維持するためのもので、runtime自体はNode.jsではありません。Node.js package、npm、CommonJS、Node.js標準library全体との互換性はありません。同梱applicationのES moduleと、次の組み込みmoduleの限定実装を利用できます。

| module                                | 主な対応範囲                                                                      |
| ------------------------------------- | --------------------------------------------------------------------------------- |
| `node:fs/promises`, `node:fs`         | private storage内のread、write、append、copy、directory、metadata、rename、remove |
| `node:path`                           | POSIX形式のpath操作                                                               |
| `node:events`                         | `EventEmitter`、`once`、AbortSignal連携                                           |
| `node:buffer`                         | `Buffer`の生成、変換、比較、検索                                                  |
| `node:timers`, `node:timers/promises` | timeout、interval、immediate、AbortSignal                                         |
| `node:stream`, `node:stream/promises` | readable、writable、duplex、transform、pipeline                                   |
| `node:url`                            | `URL`、`URLSearchParams`、file URL変換、HTTP option変換                           |
| `node:process`                        | Android ABI、pid、単調時刻とruntime内の仮想cwd、env                               |
| `node:os`                             | Androidのplatform、ABI、endiannessと仮想home、tmp、user                           |
| `node:util`                           | format、inspect、deep equality、promisify、VT除去、代表的な`types`判定            |
| `node:assert`, `node:assert/strict`   | sync/async assertionと`AssertionError`                                            |
| `node:querystring`                    | parse、stringify、escape、unescape                                                |
| `node:string_decoder`                 | UTF-8、UTF-16LE、base64、latin1、ASCII、hexの分割入力decode                       |
| `node:crypto`                         | SHA-256 hash/HMAC、乱数、UUID v4、`timingSafeEqual`                               |
| `node:dns`, `node:dns/promises`       | `lookup`とresult order                                                            |
| `node:net`                            | TCP clientとloopback限定TCP server                                                |
| `node:http`                           | HTTP clientとloopback限定HTTP/1.0、HTTP/1.1 server                                |
| `node:https`                          | certificate検証を必須とするHTTPS client                                           |

各moduleは`node:`なしのspecifierでも同じinstanceをimportできます。globalには`Buffer`、timer、`Event`、`EventTarget`、`DOMException`、`AbortController`、`AbortSignal`、`URL`、`URLSearchParams`、`Headers`、`Request`、`Response`、`fetch`、`process`、限定版`crypto`があります。API名と基本的なevent順序はNode.jsまたはWeb APIへ寄せています。表と以下の詳細にないexportやcall shapeは対応対象ではありません。

### Filesystem API

`node:fs/promises`は次のPromise APIを提供します。

- `access(path[, F_OK])`
- `appendFile(path, data[, options])`
- `copyFile(source, destination[, mode])`
- `mkdir(path[, { recursive }])`
- `readFile(path[, encoding])`
- `readdir(path)`
- `stat(path)`、`lstat(path)`
- `rename(source, destination)`
- `rmdir(path)`
- `rm(path[, { recursive, force }])`
- `unlink(path)`
- `writeFile(path, data)`

`node:fs`は同じ操作のerror-first callback形式を提供し、default exportの`promises`からPromise APIも参照できます。`muon.node`のmodule facadeから利用する場合、入れ子の`fs.promises`ではなく`node:fs/promises`を直接importしてください。

filesystemはapplication privateな`files/javascript-runtime`配下へ閉じ込められ、複数runtimeで共有します。絶対pathもこの仮想rootから解決し、`..`による脱出とroot自体の削除を拒否します。text encodingはUTF-8、binary dataは`Buffer`、`Uint8Array`、`ArrayBuffer`を対象とします。`copyFile`のmodeは`0`と`constants.COPYFILE_EXCL`、`access`のmodeは`constants.F_OK`だけを提供します。`rmdir`は空directoryだけを削除し、再帰削除には`rm`を使用します。

`stat`と`lstat`の結果はbridge可能な`size`、`isFile`、`isDirectory`の値に限定され、Node.jsの`Stats` classではありません。現在の`lstat`はsymbolic link固有情報を返しません。file descriptor、stream、watch、permission/owner/time変更、hard link、symbolic link、`content://`、shared storageは提供しません。deprecatedな`fs.exists`も提供しません。

### process、os、utility API

`process`はQuickJS runtimeごとの`EventEmitter`で、`platform`は`android`、`arch`は実行ABI、`cwd()`は仮想root `/`を返します。`env`の変更はそのruntime内だけで保持され、Android processのenvironmentや他runtimeへ反映しません。`hrtime`、`hrtime.bigint`、`uptime`は単調時計を使います。`chdir`、process終了、signal、stdio、IPC、実際のNode.js version情報は提供しません。

`os.homedir()`、`os.tmpdir()`、`os.userInfo()`も仮想filesystemに対応する値です。端末のCPU、memory、network interface、load average、hostnameを公開するAPIではありません。

`node:util`は表に記載した限定exportだけを提供します。`promisify`は通常のerror-first callbackと`util.promisify.custom`に対応しますが、`callbackify`は提供しません。`inspect`とdeep equalityは代表的なArray、Map、Set、typed array、循環参照を扱いますが、Node.js内部型や全optionの完全な表示互換性は保証しません。

`node:assert`と`node:assert/strict`は`ok`、equal系、deep equal系、match系、throws系、rejects系、`ifError`、`fail`を提供します。`node:querystring`は最大key数、重複key、custom encoder/decoderを扱います。新規コードで標準URL queryを扱う場合は`URLSearchParams`も利用できます。

### crypto API

`node:crypto`は次の限定exportを提供します。

- `createHash('sha256')`、one-shot `hash('sha256', data)`
- `createHmac('sha256', key)`
- `getHashes()`
- `randomBytes()`、`randomFill()`、`randomFillSync()`、`randomInt()`
- `randomUUID()`、`getRandomValues()`
- `timingSafeEqual()`

乱数はAndroidのOS乱数源を使用します。1回の乱数要求は1 MiB、`getRandomValues()`はWeb APIと同じ65536 byte、1つのhashまたはHMACへの入力は16 MiBまでです。global `crypto`は`getRandomValues()`と`randomUUID()`だけを提供します。Web Cryptoの`subtle`、key import/export、cipher、signature、certificate、TLS API、SHA-256以外のhashは提供しません。

### Node.js互換性の境界

application entry pointは同梱されたES moduleです。`runtime.importModule('.')`と組み込みmoduleのspecifierだけを解決し、端末上のpackage探索やpackage manager実行は行いません。`require()`、CommonJS、`node_modules`、package `exports`、JSON module、native addon、Node-API、REPLは提供しません。

`child_process`、`cluster`、`worker_threads`、`vm`、`v8`、`module`、`async_hooks`、`diagnostics_channel`、`inspector`、`readline`、`tty`、`dgram`、`tls`、`http2`、`zlib`など、表にないNode.js組み込みmoduleはmodule-not-foundになります。

statefulなstream、socket、HTTP request、hashなどは同梱application module内で使用します。rendererとのbridgeはprimitive、有限number、64 bit範囲の`bigint`、`ArrayBuffer`/typed array、JSON value、renderer callbackを転送します。stateful objectやclass instanceをrendererへ直接返さず、application module側で処理してbridge可能な結果を返してください。

各`createNode()`は非公開の`:muon_javascript` Service process内に独立したQuickJS runtimeを作ります。`release()`はmodule handle、timer、DNS要求、socket、listener、HTTP要求とruntimeを回収し、native資源とServiceのlive-runtime登録を解放してから完了します。未完了処理はActivity破棄、Service切断、またはruntime終了時にrejectされます。

### ネットワーク境界

prototypeはネットワークAPIを常に組み込むため、main Manifestに`INTERNET`と`ACCESS_LOCAL_NETWORK`を宣言します。Android 17、target SDK 37以降の`ACCESS_LOCAL_NETWORK`はdangerousな実行時権限であり、Manifest宣言だけではLANへ接続できません。Muonのprivate Serviceはpermission UIを表示しないため、LAN機能を使うアプリはActivity側で用途を説明し、通信開始前に権限を要求してください。拒否または後から取り消された場合は、通常のDNS、TCP、HTTP errorとして呼び出し側で処理します。詳細は[Android local network permission](https://developer.android.com/privacy-and-security/local-network-permission)を参照してください。

releaseのNetwork Security Configはcleartextを既定拒否し、`localhost`だけをHTTP試験とloopback server接続の例外にします。外部HTTPやLAN上のHTTPは許可せず、HTTPSではAndroid system CA storeを使います。`node:https`の`ca`で要求単位の追加trust storeを指定できますが、`rejectUnauthorized: false`による検証無効化はできません。Network Security Configは`fetch`、`node:http`、`node:https`が使うAndroid HTTP clientに適用されますが、`node:net`のraw TCPを暗号化するものではありません。詳細は[Android Network Security Configuration](https://developer.android.com/privacy-and-security/security-config)を参照してください。

CEF版の`network.allow`、`network.authorizedOrigin`、`network.localAccess`はQuickJS要求へ適用しません。組み込みapplication codeはアプリ自体と同じ権限を持ち、宛先allowlistもorigin単位の権限分離もありません。配布物へ含めるJavaScriptと接続先はアプリ開発者が管理してください。一方、受信serverは別端末から到達できないIPv4またはIPv6 loopback addressに限定します。

### 実行時上限

| 対象                                                   |                                           上限 |
| ------------------------------------------------------ | ---------------------------------------------: |
| 同時QuickJS runtime                                    |                                 16 process全体 |
| heap / native stack / 連続JS実行                       |              runtimeごとに64 MiB / 1 MiB / 2秒 |
| 未完了DNS `lookup`                                     |                                runtimeごとに64 |
| TCP socket                                             | runtimeごとに64。clientとaccepted socketの合計 |
| TCP listener                                           |                   runtimeごとに8。loopback限定 |
| 未完了HTTP/HTTPS client要求                            |                                runtimeごとに16 |
| Android HTTP worker / 待機queue                        |                    Service process全体で4 / 64 |
| loopback HTTP serverのrequest header                   |                                         16 KiB |
| HTTP client request body、server request/response body |                                         16 MiB |
| QuickJS filesystem binary read                         |                                         16 MiB |
| hashまたはHMACへの入力                                 |                                         16 MiB |
| 1回のOS乱数要求                                        |                                          1 MiB |
| timer delay                                            |                                           60秒 |
| TCP listener backlog                                   |                                       4096以下 |

上限到達時は`ERR_MUON_DNS_OPERATION_LIMIT`、`ERR_MUON_TCP_SOCKET_LIMIT`、`ERR_MUON_TCP_SERVER_LIMIT`、`ERR_HTTP_OPERATION_LIMIT`を返します。上限はruntime終了時にも回収され、他runtimeの操作は維持されます。

## Buildとpackage

リポジトリルートでQuickJS実験用のworkspaceを検証します。

```bash
npm test --workspace muon-android-poc
```

このcommandはWeb assetと両ABI native dependencyをbuildし、registryを生成して、debug/release APK、release AAB、AAB由来APKSを作成します。その後、native dependency、registry、ELF、APK/APKS packageを検査します。

主な出力は次の場所です。

| 成果物            | path                                                       |
| ----------------- | ---------------------------------------------------------- |
| debug APK         | `android/app/build/outputs/apk/debug/app-debug.apk`        |
| local release APK | `android/app/build/outputs/apk/release/app-release.apk`    |
| release AAB       | `android/app/build/outputs/bundle/release/app-release.aab` |
| release APKS      | `android/app/build/outputs/apks/release/app-release.apks`  |

local release成果物は試験用に標準Android debug keyで署名します。配布やstore uploadには使用せず、製品側で正式なrelease signingを設定してください。

## 16 KiB Android VM gate

VM試験は対象serialを明示してrepository rootから実行します。

```bash
ANDROID_SERIAL=emulator-5556 \
  npm run test:android --workspace muon-android-poc
```

このgateは`ANDROID_SERIAL`がlocal emulatorであり、Android API 37、`x86_64`、実ページサイズ16384であることを開始時に検証します。その後、debug instrumentation全件、署名済みrelease APKのinstall/start、bundletoolがAABから選択した端末別split APKのinstall/startを行い、WebViewのpage-ready eventを待ちます。instrumentationには4個の独立runtimeから各8組のDNS、raw TCP、HTTPを同時実行し、全runtime解放後に新runtimeで再通信する負荷試験を含みます。時間経過やpollingで成功を推測しません。

## Pixel 6 arm64 gate

ローカルVM gateの完了後、Pixel 6をUSB接続して対象serialを明示します。

```bash
ANDROID_SERIAL=YOUR_PIXEL_6_SERIAL \
  npm run test:android:pixel6 --workspace muon-android-poc
```

このgateは対象が物理Pixel 6（`oriole`）、Android API 37、`arm64-v8a`、実ページサイズ4096であることを開始時に検証します。VM gateと同じdebug instrumentation全件、署名済みrelease APK、AAB由来の端末別split APKを実機へinstall/startし、WebViewのpage-ready eventまで確認します。

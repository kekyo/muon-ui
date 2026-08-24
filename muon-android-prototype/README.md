# muon Android WebView prototype

このdirectoryは、muonのAndroid WebView backendとnative plugin packageを結合検証する試作hostです。公開済みの`muon-ui` Android targetやPlay Store配布物ではありません。

## 対応環境

- Android API 24以降
- `x86_64`と`arm64-v8a`
- 4 KiBおよび16 KiBページに整列したnative artifact
- Java 17
- Android SDK Platform 37、Build Tools 36.0.0
- Android NDK 29.0.14206865、CMake 4.1.2
- root packageが指定するNode.jsとnpm dependency

repositoryはrecursive submoduleを取得した状態で使用してください。初回buildでは、固定した公式libffi 3.8.0 source archive、QuickJS 2026-06-04 source archive、[公式bundletool standalone jar](https://github.com/google/bundletool/releases/tag/1.18.3)をdownloadします。各downloadは使用前にSHA-256を検証します。native dependencyの生成manifestには入力commit、toolchain、configure引数、patch一覧も記録します。

## Android pluginを同梱する

Android pluginはruntimeにdownloadまたは探索せず、[android-plugins.json](./android-plugins.json)に記載してAPK/AABへbuild時に同梱します。各entryには次を指定します。

```json
{
  "name": "sample_plugin",
  "soname": "libsample_plugin.so",
  "source": "../path/inside-this-repository/sample_plugin.cpp",
  "artifacts": {
    "x86_64": "lib/x86_64/libsample_plugin.so",
    "arm64-v8a": "lib/arm64-v8a/libsample_plugin.so"
  },
  "allow": ["sample.namespace.*"],
  "config": {
    "sample.key": "sample-value"
  }
}
```

- `source`はこのdirectoryからの相対pathで、同じrepository内に存在するC++20 sourceでなければなりません。
- pluginはMuon plugin APIの`muon_init_plugin`をexportする必要があります。
- `soname`は`lib<name>.so`形式にします。
- `x86_64`と`arm64-v8a`の両方を必ず宣言します。生成物は対応する`lib/<abi>/`へ配置されます。
- `allow`は空にできません。runtimeはload済みmetadataへこのpolicyを適用し、許可された関数だけをWebViewへ公開します。
- `config`のkeyとvalueはstringです。
- desktop用の`path`、`signature`、`salt`はAndroid registryでは使用できません。

registry generatorは入力を検証し、C++ load tableとCMake plugin targetを同じ正規化済みentryから生成します。重複名、重複soname、unsupported ABI、欠落artifact、ELF machine不一致、16 KiB未整列、`muon_init_plugin`欠落はpackage検査で失敗します。

## 組み込みJavaScript runtime

Android simple modeでは、Node.js版と同じ生成形の`muon.node.createNode()`から、独立したQuickJS runtimeを作成できます。

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

| module                                | 主な対応範囲                                                             |
| ------------------------------------- | ------------------------------------------------------------------------ |
| `node:fs/promises`, `node:fs`         | application private storage内のfile、directory、metadata、rename、remove |
| `node:path`                           | POSIX path操作                                                           |
| `node:events`                         | `EventEmitter`と`once`                                                   |
| `node:buffer`                         | `Buffer`の生成、変換、比較、検索                                         |
| `node:timers`, `node:timers/promises` | timeout、interval、immediate、AbortSignal                                |
| `node:stream`, `node:stream/promises` | readable、writable、duplex、transform、pipeline                          |
| `node:url`                            | `URL`、`URLSearchParams`、file URL変換                                   |
| `node:dns`, `node:dns/promises`       | `lookup`とresult order                                                   |
| `node:net`                            | TCP clientとloopback限定TCP server                                       |
| `node:http`                           | HTTP clientとloopback限定HTTP/1.0、HTTP/1.1 server                       |
| `node:https`                          | certificate検証を必須とするHTTPS client                                  |

各moduleは`node:`なしのspecifierでも同じinstanceをimportできます。globalには`Buffer`、timer、`Event`、`EventTarget`、`DOMException`、`AbortController`、`AbortSignal`、`URL`、`URLSearchParams`、`Headers`、`Request`、`Response`、`fetch`があります。API名と基本的なevent順序はNode.jsまたはWeb APIへ寄せていますが、実装していないoptionやexportは互換性のためのno-opにせず、明示的なerrorまたはmodule-not-foundとして扱います。

primitive、有限number、64 bit範囲の`bigint`、`ArrayBuffer`/typed array、JSON value、renderer callbackをbridgeで転送します。filesystemはapplication privateな`files/javascript-runtime`配下へ閉じ込められ、複数runtimeで共有します。

各`createNode()`は非公開の`:muon_javascript` Service process内に独立したQuickJS runtimeを作ります。`release()`はmodule handle、timer、DNS要求、socket、listener、HTTP要求とruntimeを回収します。未完了処理はActivity破棄、Service切断、またはruntime終了時にrejectされます。

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
| TCP listener backlog                                   |                                       4096以下 |

上限到達時は`ERR_MUON_DNS_OPERATION_LIMIT`、`ERR_MUON_TCP_SOCKET_LIMIT`、`ERR_MUON_TCP_SERVER_LIMIT`、`ERR_HTTP_OPERATION_LIMIT`を返します。上限はruntime終了時にも回収され、他runtimeの操作は維持されます。

## Buildとpackage

repository rootでAndroid workspace全体を実行します。

```bash
npm test --workspace muon-android-prototype
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
  npm run test:android --workspace muon-android-prototype
```

このgateは`ANDROID_SERIAL`がlocal emulatorであり、Android API 37、`x86_64`、実ページサイズ16384であることを開始時に検証します。その後、debug instrumentation全件、署名済みrelease APKのinstall/start、bundletoolがAABから選択した端末別split APKのinstall/startを行い、WebViewのpage-ready eventを待ちます。時間経過やpollingで成功を推測しません。

## Pixel 6 arm64 gate

ローカルVM gateの完了後、Pixel 6をUSB接続して対象serialを明示します。

```bash
ANDROID_SERIAL=YOUR_PIXEL_6_SERIAL \
  npm run test:android:pixel6 --workspace muon-android-prototype
```

このgateは対象が物理Pixel 6（`oriole`）、Android API 37、`arm64-v8a`、実ページサイズ4096であることを開始時に検証します。VM gateと同じdebug instrumentation全件、署名済みrelease APK、AAB由来の端末別split APKを実機へinstall/startし、WebViewのpage-ready eventまで確認します。

## Runtimeとlifecycleの制約

process内にはcardio 1.1.0の`dispatcher_host_android_auto`と共通`MuonPluginRuntime`を一組だけ作り、Android main Looperへ接続します。各Activity/WebViewは独立sessionを持ちます。最後の通常sessionが閉じるとpluginの非同期`Stop()`を開始し、逆順unloadとcardio host破棄を同じLooper上で完了します。停止中に新Activityが生成された場合は、Stop完了eventを受けてから新runtimeへattachします。

Androidがapplication processを強制終了した場合、Activity lifecycle callbackやplugin `Stop()`の実行は保証されません。pluginは永続dataの確定や外部transactionの整合性をprocess終了時の`Stop()`だけに依存させず、各操作の完了時に保存してください。

libffi 3.8.0のx86_64静的トランポリンは4 KiB table固定のため、16 KiB VMでは[muon所有patch](./patches/libffi/0001-android-x86_64-16k-static-trampoline.patch)を展開後のbuild用copyへ適用します。libffi submodule自体は変更しません。instrumentationはclosureの実行mappingがexecutableかつnon-writableであることと、allocation/free balanceを実測します。

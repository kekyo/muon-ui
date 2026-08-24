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

`createNode()`というAPI名は既存コードの生成形を維持するためのもので、runtime自体はNode.jsではありません。Node.js packageや標準library全体との互換性はありません。現在は同梱application moduleの`.`と、`node:fs/promises`、`node:fs`、`node:path`、`node:timers/promises`の限定実装をimportできます。`fs`、`path`、`timers/promises`という接頭辞なしのspecifierも利用できます。

primitive、有限number、64 bit範囲の`bigint`、`ArrayBuffer`/typed array、JSON value、renderer callbackをbridgeで転送します。filesystemはapplication privateな`files/javascript-runtime`配下へ閉じ込められ、複数runtimeで共有します。

各`createNode()`は非公開の`:muon_javascript` Service process内に独立したQuickJS runtimeを作ります。prototypeでは同時runtime数を16、各runtimeのheapを64 MiB、stackを1 MiB、連続したJavaScript実行を2秒に制限します。`release()`はmodule handleとruntimeを回収します。未完了処理はActivity破棄、Service切断、またはruntime終了時にrejectされます。

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

## Runtimeとlifecycleの制約

process内にはcardio 1.1.0の`dispatcher_host_android_auto`と共通`MuonPluginRuntime`を一組だけ作り、Android main Looperへ接続します。各Activity/WebViewは独立sessionを持ちます。最後の通常sessionが閉じるとpluginの非同期`Stop()`を開始し、逆順unloadとcardio host破棄を同じLooper上で完了します。停止中に新Activityが生成された場合は、Stop完了eventを受けてから新runtimeへattachします。

Androidがapplication processを強制終了した場合、Activity lifecycle callbackやplugin `Stop()`の実行は保証されません。pluginは永続dataの確定や外部transactionの整合性をprocess終了時の`Stop()`だけに依存させず、各操作の完了時に保存してください。

libffi 3.8.0のx86_64静的トランポリンは4 KiB table固定のため、16 KiB VMでは[muon所有patch](./patches/libffi/0001-android-x86_64-16k-static-trampoline.patch)を展開後のbuild用copyへ適用します。libffi submodule自体は変更しません。instrumentationはclosureの実行mappingがexecutableかつnon-writableであることと、allocation/free balanceを実測します。

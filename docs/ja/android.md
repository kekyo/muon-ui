# Androidアプリをビルド・配布する

muon-uiのnpmパッケージから、Webアセットを内蔵したAndroid APKを作れます。Viteのビルドと公開CLIを使い、GradleやJNIのソースを編集する必要はありません。配布したAPKの起動に開発サーバーは不要です。

今回の公開範囲はdebug APKと署名済みrelease APKです。FCM、QuickJS、Node.js sidecar、AABの生成は公開していません。

## 必要な環境

| 項目 | 条件 |
| --- | --- |
| ビルドPC | Linux x64 |
| Node.js/npm | Node.js 24で検証済み。Viteを含む依存パッケージの実行条件を満たすこと |
| JDK | 17～25。実行検証は25.0.3 |
| Android SDK | Platform `platforms/android-37.0`、Build Tools `build-tools/36.0.0` |
| Gradle/AGP | 同梱WrapperでGradle 9.4.1、AGP 9.2.1を使用 |
| アプリのABI | `arm64-v8a`と`x86_64`。既定で両方を収録 |
| 端末 | API 37で検証済み。実機はPixel 6、エミュレーターはx86_64・16 KiBページ |

通常のアプリビルドにはNDK、CMake、Muonのソース取得は不要です。MuonのJava・ネイティブ部品は事前ビルドしたAARとしてnpm内に収録しています。GradleとAndroidXなどの初回取得にはネットワーク接続が必要です。Android Studioと接続端末はビルドの必須条件ではありません。

APKの最低インストールAPIは24ですが、古いOSでの動作は未検証です。WebViewは`WEB_MESSAGE_LISTENER`、`WEB_MESSAGE_ARRAY_BUFFER`、`DOCUMENT_START_SCRIPT`を必要とし、不足時は起動画面に理由を表示します。検証したWebViewはPixel 6が153.0.8010.36、エミュレーターが149.0.7827.5です。arm64の16 KiB配置は静的検査済みですが、実行検証はPixel 6の4 KiBとx86_64エミュレーターの16 KiBで行っています。[WebViewの機能検出](https://developer.android.com/reference/androidx/webkit/WebViewFeature)、[16 KiBページへの対応](https://developer.android.com/guide/practices/page-sizes)

JDKと[Android CLI](https://developer.android.com/tools/agents/android-cli)を用意し、SDKのライセンスを確認したうえで、必要なSDKパッケージをインストールします。

```bash
export JAVA_HOME=/path/to/jdk-25
export ANDROID_HOME="$HOME/Android/Sdk"
export PATH="$JAVA_HOME/bin:$ANDROID_HOME/platform-tools:$PATH"
android --sdk="$ANDROID_HOME" sdk install platforms/android-37.0 build-tools/36.0.0 platform-tools
```

SDKとJDKの配置先は環境に合わせてください。`platform-tools`は実機へインストールするときに使います。Muonの`prepare`はSDK/JDKの不足やバージョン不整合を診断し、固定版Gradleを準備します。SDK/JDKのインストールとライセンス受諾は行いません。

## Viteプロジェクトへ追加する

既存のVite・TypeScriptプロジェクトでmuon-uiを開発依存へ追加します。

```bash
npm install -D muon-ui
```

`vite.config.ts`のMuonプラグインを次のように構成します。既存のReactなどのプラグインは残してください。次はsimpleモードの例です。virtual moduleでimport元と許可関数を指定するvalidateモードについては後述します。

```ts
import { defineConfig } from 'vite';
import muon from 'muon-ui/vite';

export default defineConfig({
  plugins: [muon({
    pluginAccess: false,
    build: { targets: ['android'] },
  })],
});
```

プロジェクト直下の`muon.json`にアプリ情報を記述します。`icons/app.png`は自分のPNG画像へ置き換えるか、既定アイコンを使う場合は`icon`を省略してください。

```json
{
  "android": {
    "applicationId": "com.example.notes",
    "label": "Muon Notes",
    "versionCode": 1,
    "versionName": "1.0.0",
    "abis": ["arm64-v8a", "x86_64"],
    "icon": "icons/app.png"
  },
  "config": { "channel": "production" }
}
```

SDK/JDKの診断とGradleの準備を行います。

```bash
npx muon prepare --target android
```

`muon build`は、上のVite設定を使ってWebアセットのビルドからdebug APKの生成まで実行します。

```bash
npx muon build --target android
```

`vite build`でも同じdebug APKを生成できます。`muon build`と`vite build`はどちらか一方で十分です。

```bash
npx vite build
```

APKの出力先は`dist-muon/android/com.example.notes-1-debug.apk`です。同名の`.json`にはvariant、署名状態、ABI、アプリID、バージョンを記録します。CLIの`--json`でも結果を取得できます。`.muon/android`は次のビルドで置き換える生成領域なので、直接編集しないでください。

Androidは`--target android`またはVite設定で明示的に選びます。既定ターゲットと`--all`はdesktop向けです。Viteの開発サーバーからAndroidを自動起動する機能とHMRは今回の範囲に含みません。

MuonのViteプラグインを設定していないプロジェクトでは、`--assets`ですでにビルドしたWebアセットを指定できます。

```bash
npx muon build --target android --assets ./dist
```

## 設定とMuon API

`android`の各項目は、Viteの`build.android`でも指定できます。優先順位はVite/APIの明示指定、`muon.json`、既定値です。CLIの`--app-id`と`--name`は`muon.json`のアプリIDとラベルを上書きします。明示した`build.android.applicationId`と`label`があれば、そちらを優先します。

| 項目 | 意味・既定値 |
| --- | --- |
| `applicationId` | インストールと更新の識別子。省略時はpackage名から`dev.muon.…`を生成 |
| `label` | ランチャーの表示名。既定はpackage名 |
| `versionCode` | 1～2100000000の整数。既定は1。リリースごとに増やす |
| `versionName` | 表示バージョン。既定は`package.json`のversion |
| `abis` | 収録するABI。既定は両対応ABI |
| `icon` | PNG画像のパス |
| `permissions` | Manifestに追加する`android.permission.…`の配列。既定は空 |
| `sdkPath` | SDKの場所。省略時は`ANDROID_HOME`、`ANDROID_SDK_ROOT`、PATH上のadbから検出 |
| `plugins` | 後述の事前ビルド済みプラグイン。既定は空 |
| `signing` | 後述のrelease署名設定 |
| `fcm`、`quickjs` | 未提供。省略またはfalseのみ受け付ける |

`muon.json`内の相対パスの基準は、そのファイルのディレクトリです。Vite/APIから渡すパスはプロジェクトルートを基準にします。`prepare`でSDKを切り替える場合は、環境変数、`muon.json`または`--sdk-path`を指定してください。

Webアセットの既定URLは`https://main.asset.muon.invalid/index.html`です。Viteの`base`がサブパスなら、そのパスを反映します。`browser.startPage`はこのoriginのURLか`asset://main/…`で指定します。外部サイトを起動ページにする指定、ZIPアセット、複数アセットhostは未対応です。

Muon APIはアプリのJavaScriptより先に初期化されます。独自adapterのimportは不要です。simpleモードでTypeScriptから使う例を示します。

```ts
import type {} from 'muon-ui';

const runtime = await window.muon.environments.getRuntimeInfo();
const config = await window.muon.environments.getConfigValues();
await window.muon.fs.writeTextFile('note.txt', 'Saved on Android', 'utf8');
const note = await window.muon.fs.readTextFile('note.txt', 'utf8');
```

simpleモードでは`plugin.plugins`を省略すると現在対応している34個の組込み関数を公開します。明示した場合は、そのリストだけが対象です。空配列、または`internal`を含まないリストでは組込み関数を公開しません。各エントリーの`allow`で関数を限定でき、直接RPCを組み立てた場合もネイティブ側で権限を検査します。

### validateモードでimportする

Vite設定の`pluginAccess: false`を外し、`muon.json`に許可するimport元と関数を指定します。

```json
{
  "plugin": {
    "mode": "validate",
    "plugins": [{
      "name": "internal",
      "imports": [{
        "sources": ["src/**"],
        "allow": ["muon.environments.*", "muon.fs.readTextFile", "muon.fs.writeTextFile"]
      }]
    }]
  }
}
```

```ts
import type {} from 'muon-ui';
import { getRuntimeInfo } from 'muon:environments';
import { readTextFile, writeTextFile } from 'muon:fs';

const runtime = await getRuntimeInfo();
await writeTextFile('note.txt', 'Saved on Android', 'utf8');
const note = await readTextFile('note.txt', 'utf8');
```

`sources`にはプロジェクトルートからのソースパス、`packages`には許可するnpmパッケージ名を指定してください。Viteが直接importを検査し、生成したcapability IDと関数の組合せをネイティブ側が検査します。validateモードでは`window.muon`を公開しません。未対応の関数名や、一致する関数がないパターンはビルド時にエラーになります。AndroidとデスクトップのJSは、それぞれのターゲットを指定して別々にビルドしてください。

`plugin.pages`の省略時は、信頼するアセットoriginのメインフレームからRPCを受理します。指定できる値は`asset://main/**`と`https://main.asset.muon.invalid/**`です。空配列ではブリッジを停止し、パス単位などの未対応条件はビルド時に拒否します。

`sources`・`packages`はビルド時のimport検査です。同じページ内で有効なcapabilityを取得したコードは、その許可範囲でRPCを呼べます。呼出し元JSファイルごとの実行時認証や、同一originのiframeから親ページへのアクセスを含む隔離は保証しません。WebMessageが通知する送信元はoriginとメインフレーム判定であり、JSファイル名や文書の完全URLは含まれません。[WebMessageListenerの仕様](https://developer.android.com/reference/androidx/webkit/WebViewCompat.WebMessageListener)、[Android版の制約](./limitation.md#android-webviewバックエンド)

`muon.fs`の相対パスはアプリ専用のfiles領域を指します。同じアプリIDと署名鍵を使えば、更新後も保存データを利用できますが、アンインストールすると削除される点には注意が必要です。`content://`とファイル選択ダイアログは未提供です。基本のbrowser・environments・fs APIの対応範囲は[Android API対応方針](../../android-api-compatibility.md)を参照してください。同文書のQuickJS関連は試作専用です。

外部通信が必要なら`permissions`に`android.permission.INTERNET`を追加します。Android版はCEF版の`network.allow`などによる宛先制限を適用しません。HTTPの平文通信は無効です。権限名をManifestに追加するだけでは、ユーザーへの実行時権限要求は行われません。ローカルネットワーク権限など、権限ダイアログが必要な機能は公開経路では未提供です。[Android版の制約](./limitation.md#android-webviewバックエンド)、[Androidのローカルネットワーク権限](https://developer.android.com/privacy-and-security/local-network-permission)

## release APKへ署名する

上のVite設定を使う場合、`muon pack`でWebアセットのビルドからrelease APKの生成・署名・検証まで行います。事前に`muon build`や`vite build`を実行する必要はありません。

自分で管理するkeystoreを用意し、WebアセットやViteの`public`ディレクトリの外に保存します。次は新しい鍵を作る例です。`keytool`の対話入力でパスワードと証明書情報を指定してください。

```bash
mkdir -p "$HOME/.local/share/notes-signing"
keytool -genkeypair -keystore "$HOME/.local/share/notes-signing/release.p12" \
  -alias release -keyalg RSA -keysize 2048 -validity 10000
```

`muon.json`の`android.signing`へ、鍵の場所とパスワードを渡す環境変数名を指定します。`keystore`には実際のパスを記述してください。文字列内の`$HOME`は展開しません。

```json
{
  "keystore": "/home/me/.local/share/notes-signing/release.p12",
  "keyAlias": "release",
  "storePasswordEnv": "NOTES_STORE_PASSWORD",
  "keyPasswordEnv": "NOTES_KEY_PASSWORD"
}
```

鍵とkeystoreのパスワードが同じなら`keyPasswordEnv`は省略できます。パスワード自体を設定ファイルに記述しないでください。Bashで入力する例です。

```bash
read -rs -p 'Keystore password: ' NOTES_STORE_PASSWORD
export NOTES_STORE_PASSWORD
npx muon pack --target android --type apk
unset NOTES_STORE_PASSWORD
```

上の例で`NOTES_KEY_PASSWORD`を指定した場合は、その環境変数も設定してください。CIでは同じ環境変数へCIの秘密情報を渡します。

検証済みのrelease APKの出力先は`artifacts/apk/com.example.notes-1-release.apk`です。同名の`.json`に公開証明書のSHA-256を含めます。秘密情報は成果物情報に記録しません。署名設定の不足や既定debug鍵の使用はエラーです。[Androidのアプリ署名](https://developer.android.com/studio/publish/app-signing)

更新版では`applicationId`と署名鍵を維持し、`versionCode`を増やして再度packします。debug APKとrelease APKの署名鍵は通常異なるため、debug版からの切り替えにはアンインストールが必要です。その操作では保存データも削除されます。

## インストールと動作確認

端末のUSBデバッグまたはワイヤレスデバッグを有効にし、adbで接続します。複数端末がある場合は`ANDROID_SERIAL`を指定してください。

```bash
adb devices
export ANDROID_SERIAL=your-device-serial
adb install -r artifacts/apk/com.example.notes-1-release.apk
adb shell am start -W -n com.example.notes/dev.muon.runtime.MuonAppActivity
adb logcat -s MuonActivity AndroidRuntime chromium
```

起動失敗時は画面の診断とlogcatを確認します。ビルドで失敗した場合はCLIの出力と`.muon/android`の生成ファイルを確認してください。APKを作り直すときも公開CLIを使います。

## 事前ビルド済みネイティブプラグイン

独自プラグインは選択したすべてのABIの`.so`を用意し、`android.plugins`へ登録してください。権限とプラグイン固有の設定は、同じ名前の`plugin.plugins`へ記述します。ソースコードからプラグインをビルドする機能はありません。

```json
{
  "android": {
    "plugins": [{
      "name": "calculator",
      "soname": "libcalculator.so",
      "libraries": {
        "arm64-v8a": "plugins/arm64-v8a/libcalculator.so",
        "x86_64": "plugins/x86_64/libcalculator.so"
      },
      "metadata": "plugins/calculator.json"
    }]
  },
  "plugin": {
    "mode": "validate",
    "plugins": [{
      "name": "calculator",
      "imports": [{ "sources": ["src/**"], "allow": ["muon.calculator.*"] }],
      "config": { "precision": "double" }
    }]
  }
}
```

この例では`import { add } from 'muon:calculator'`で呼び出します。関数のTypeScript宣言はプラグインの提供者が用意します。組込みAPIも使う場合は`internal`のエントリーを追加してください。simpleモードでは`imports`の代わりにエントリー直下へ`allow`を指定します。

`metadata`は提供者が配布する次の形式のJSONファイルです。ハッシュの文字列は、stripなどの加工を終えた各ライブラリのSHA-256に置き換えます。

```json
{
  "schemaVersion": 1,
  "functions": ["muon.calculator.add"],
  "sha256": {
    "arm64-v8a": "<64桁の小文字16進数>",
    "x86_64": "<64桁の小文字16進数>"
  }
}
```

validateモードではメタデータが必須です。ビルド時にABI別のハッシュを確認し、完全名とワイルドカードを各プラグインの関数一覧へ照合します。起動時の照合対象は、許可した関数と実際の登録結果です。不一致ならプラグイン名を含むエラーを表示します。simpleモードでは省略できますが、指定した場合は同じ照合を行います。Android用バイナリをホストで実行する必要はありません。

登録名、SONAME、ABI、`muon_init_plugin`、依存ライブラリ、16 KiB整列もビルド前に検査します。プラグイン固有の設定値は文字列です。共通設定の`signature`・`salt`はAndroidでは受理せず、メタデータのABI別SHA-256を使います。バッファ、双方向コールバック、返された関数のプロキシとその解放は共通のプラグインABIに従います。[プラグインの開発](./muon-plugin-develop.md)

## 今後の範囲

FCMの通知配送とバックグラウンド処理は後続作業です。今回のAPKはFirebase設定なしで動作します。

QuickJSの正式提供時は、開発PCでJavaScriptを単一ESMへbundleし、組み込みmoduleを外部参照として残す方針です。コードとハッシュ・必要機能の情報をAPKのassetsへ同梱し、アプリと同時に更新します。永続データはコードから分離します。任意のnpmパッケージ、Node.js全体との互換性、JavaScriptだけの独立更新は含めません。詳しくは[plan6のデプロイメント方針](./plans/plan6.md#8-quickjsのjavascriptデプロイメント方針)を参照してください。

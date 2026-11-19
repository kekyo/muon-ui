# Androidアプリをビルド・配布する

muon-uiのnpmパッケージから、Webアセットを内蔵したAndroid APKを作れます。Viteのビルドと公開CLIを使い、GradleやJNIのソースを編集する必要はありません。配布したAPKの起動に開発サーバーは不要です。

公開CLIでdebug APKと署名済みrelease APKを作成できます。[バックグラウンドサービス](services.md)にはC/C++またはQuickJSで動くJS/TSを使えます。FCMと公開CLIによるAABの生成は未対応です。

## 必要な環境

| 項目 | 条件 |
| --- | --- |
| ビルドPC | Linux x64 |
| Node.js/npm | Node.js 24で検証済み。Viteを含む依存パッケージの実行条件を満たすこと |
| JDK | 17～25。実行検証は21.0.12.1と25.0.3 |
| Android SDK | Platform `platforms/android-37.0`、Build Tools `build-tools/36.0.0` |
| Gradle/AGP | 同梱WrapperでGradle 9.4.1、AGP 9.2.1を使用 |
| アプリのABI | `arm64-v8a`と`x86_64`。既定で両方を収録 |
| 端末 | 実機はPixel 6・API 37。x86_64エミュレーターはAPI 33・35・36・37で検証済み。16 KiBページはAPI 36・37で検証 |

通常のアプリビルドにはNDK、CMake、Muonのソース取得は不要です。MuonのJava・ネイティブ部品は事前ビルドしたAARとしてnpm内に収録しています。GradleとAndroidXなどの初回取得にはネットワーク接続が必要です。Android Studioと接続端末はビルドの必須条件ではありません。

APKの最低インストールAPIは24です。OSのバージョンに加えて、下記のWebView機能が必要です。WebViewは`WEB_MESSAGE_LISTENER`、`WEB_MESSAGE_ARRAY_BUFFER`、`DOCUMENT_START_SCRIPT`を必要とし、不足時は起動画面に理由を表示します。検証したWebViewはPixel 6が153.0.8010.36、エミュレーターが149.0.7827.5です。arm64の16 KiB配置は静的検査済みです。実行検証は、arm64ではPixel 6の4 KiB、x86_64エミュレーターでは4 KiBと16 KiBで行っています。[WebViewの機能検出](https://developer.android.com/reference/androidx/webkit/WebViewFeature)、[16 KiBページへの対応](https://developer.android.com/guide/practices/page-sizes)

JDKと[Android CLI](https://developer.android.com/tools/agents/android-cli)を用意し、SDKのライセンスを確認したうえで、必要なSDKパッケージをインストールします。

```bash
export JAVA_HOME=/path/to/jdk-21
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
| `permissions` | アプリが明示する権限の完全名の配列。既定は空。[ビルド時に追加される権限と実行時の許可](#ビルド時に追加される権限と実行時の許可) |
| `sdkPath` | SDKの場所。省略時は`ANDROID_HOME`、`ANDROID_SDK_ROOT`、PATH上のadbから検出 |
| `plugins` | 後述の事前ビルド済みプラグイン。既定は空 |
| `systemBars` | 起動時のシステムバーとコンテンツ配置 |
| `notifications` | 通知の小さいアイコンと静的チャンネル |
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

simpleモードでは`plugin.plugins`を省略すると対応している組込み関数を公開します。通知関数の公開には、別途`notifications.enabled: true`が必要です。明示した場合は、そのリストだけが対象です。空配列、または`internal`を含まないリストでは組込み関数を公開しません。各エントリーの`allow`で関数を限定でき、直接RPCを組み立てた場合もネイティブ側で権限を検査します。

### ビルド時に追加される権限と実行時の許可

muonはアプリが `android.permissions` に指定した権限と、利用する機能に応じて追加する権限を合わせ、重複を除いてAPKのManifestに宣言します。設定ファイルに表の権限を書くときは、名前の先頭に `android.permission.` を付けてください。

| アプリの機能・設定 | muonがビルド時に追加する権限 | 開発者が指定または要求するもの |
| --- | --- | --- |
| WebViewの外部通信 | WebViewの使用だけでは追加なし | `INTERNET` を宣言します。インストール時に付与されます。 |
| サービスの `io.http.origins` に接続先がある、または `io.server.listen` を指定 | `INTERNET` | 同じ権限の追記は不要です。平文HTTPを使う場合は、許可する接続先を `http://` のoriginで指定します。muonはそのホストに限ってネットワークセキュリティ設定で平文通信を許可します。 |
| Androidサービスを含む | `FOREGROUND_SERVICE`、サービスの種類に対応する `FOREGROUND_SERVICE_*`、`POST_NOTIFICATIONS`。`shortService` には種類固有の権限はありません。 | `services.<id>.android` に用途に合う `foregroundServiceType` と実行中の `notification` を指定します。`specialUse` には用途を示す `specialUseSubtype` も必要です。 |
| nativeまたはQuickJSサービスを一つ以上含む | `RECEIVE_BOOT_COMPLETED` と、端末の起動完了を受け取るレシーバー1件。全サービスの初期値がOFFでも追加します。 | 権限とレシーバーの手動登録は不要です。受信権限はインストール時に付与されます。サービスがないAPKには、muonはどちらも自動追加しません。 |
| ローカル通知を有効化（`notifications.enabled: true`） | `POST_NOTIFICATIONS` | Android 13以降で通常の通知を表示するには、画面から `muon.android.notifications.requestPermission()` で利用者の許可を求めます。フォアグラウンドサービスは、この許可がなくても開始できます。開始時の通知は必要です。 |

端末の再起動後にサービスを自動起動するには、インストール後に一度アプリを開いてください。初回ロック解除時の設定は通常のアプリ起動時と共通です。`services.<id>.autostart` の初期値はOFFで、利用者が変更した場合は保存値を優先します。ONのサービスはAndroidの起動条件を満たせば、画面を開かずに起動します。[再起動後の自動起動](services.md#再起動後の自動起動)・[Androidの受信用権限](https://developer.android.com/reference/android/Manifest.permission#RECEIVE_BOOT_COMPLETED)

例えば、WebViewから外部のHTTPSサイトに接続するには、次のように指定します。

```json
{
  "android": { "permissions": ["android.permission.INTERNET"] }
}
```

既定では `muon.json5`、`muon.jsonc`、`muon.json` の順に探し、最初に見つかったファイルを読みます。Viteの `build.android.permissions` を指定した場合は、その配列が設定ファイルの配列に優先します。次の例ではビルド後の権限一覧に `android.permission.INTERNET` が含まれます。変更するときは元の設定を編集して再度ビルドします。[Android SDKのapkanalyzer](https://developer.android.com/tools/apkanalyzer)

```bash
npx muon build --target android
apkanalyzer manifest permissions dist-muon/android/com.example.notes-1-debug.apk
apkanalyzer manifest print dist-muon/android/com.example.notes-1-debug.apk
```

Manifestへの宣言だけでは実行時の許可ダイアログは開きません。例えば位置情報サービスでは、muonが追加する `FOREGROUND_SERVICE_LOCATION` に加えて、アプリ開発者が `ACCESS_COARSE_LOCATION` または `ACCESS_FINE_LOCATION` を宣言し、利用者の許可を得る必要があります。バックグラウンドからの開始には、条件に応じて `ACCESS_BACKGROUND_LOCATION` の許可も必要です。muonの公開JS APIには任意のAndroid権限の許可を求める機能はありません。[位置情報サービスの要件](https://developer.android.com/develop/background-work/services/fgs/service-types#location)・[Androidの通知権限](https://developer.android.com/develop/ui/compose/notifications/notification-permission)・[ネットワークセキュリティ設定](https://developer.android.com/privacy-and-security/security-config)

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

`muon.fs`の相対パスはアプリ専用のfiles領域を指します。同じアプリIDと署名鍵を使えば、更新後も保存データを利用できますが、アンインストールすると削除される点には注意が必要です。`content://`とファイル選択ダイアログは未提供です。APIの用途は[内蔵プラグインリファレンス](./muon-built-in-plugin-reference.md)を参照してください。Androidで未対応の関数は公開せず、no-opへ置き換えることもありません。

Android版はCEF版の`network.allow`などによる宛先制限を適用しません。平文HTTPは既定で無効です。ローカルネットワーク権限など、個別の許可ダイアログが必要な通信機能は公開経路では未提供です。[ビルド時に追加される権限と実行時の許可](#ビルド時に追加される権限と実行時の許可)・[Android版の制約](./limitation.md#android-webviewバックエンド)・[Androidのローカルネットワーク権限](https://developer.android.com/privacy-and-security/local-network-permission)

## システムバーと安全領域

Androidのステータスバーとナビゲーションバーは、`muon.android.systemBars`で個別に制御できます。起動時の指定は`muon.json`の`android.systemBars`、またはViteの`build.android.systemBars`に記述します。

```json
{
  "android": {
    "systemBars": {
      "statusBar": { "visible": true, "icons": "dark" },
      "navigationBar": { "visible": true },
      "contentLayout": "safe-area"
    }
  }
}
```

既定の配置は`safe-area`です。OSがバーの背後まで描画する環境でも、MuonがWebViewの外側にバーとノッチの余白を確保します。`edge-to-edge`ではWebViewを端まで配置し、ページ側でCSS変数`--muon-safe-area-top`などを使って操作部品を避けます。この変数はWeb側でまだ必要な余白なので、`safe-area`では0になります。ネイティブ側とCSS側で同じ余白を二重に加えないでください。

実行中の`configure()`は指定項目だけを更新します。既存のfullscreen APIとも要求状態を共有し、Activity再生成では実行中の要求を復元します。プロセスを新しく起動した場合はビルド時設定から始めます。上下のバーを非表示にしても、ユーザーはOSのスワイプ操作で一時表示できます。`getState().actual`はOSの報告値であり、この一時表示を必ず検出できるわけではありません。

Androidのバージョンやマルチウインドウなどによって、OSが表示要求を調整する場合があります。要求値と観測値を分けて扱ってください。API 24・25ではナビゲーションバーの濃色アイコンを指定できません。APIとCSS変数の詳細は[内蔵プラグインリファレンス](./muon-built-in-plugin-reference.md#androidのシステムバーと安全領域)、OS側の条件は[Androidのedge-to-edge](https://developer.android.com/develop/ui/views/layout/edge-to-edge)を参照してください。

## ローカル通知とOS設定画面

通知を利用する場合は、共通の`notifications`で有効化し、Android用の小さいアイコンと用途分類を`android.notifications`に指定します。用途分類のIDはAPI 26以降のOSチャンネルIDにもなります。

```json
{
  "notifications": { "enabled": true, "defaultCategory": "orders" },
  "android": {
    "notifications": {
      "smallIcon": "icons/notification.png",
      "channels": [
        {
          "id": "orders",
          "name": "Orders",
          "description": "Order progress and collection",
          "importance": "default"
        }
      ]
    }
  }
}
```

`smallIcon`は24～1024ピクセルの正方形PNGで、透明な背景に白いシルエットを描いた画像が必要です。色付き画像、完全に不透明な画像、ランチャーアイコンの流用は受け付けません。ビルド時に各画面密度用の24 dpアイコンを生成します。通常のアプリアイコンは従来どおり`android.icon`などで指定します。

アプリの起動や通知の投稿で権限ダイアログが自動的に開くことはありません。通知を有効にしたビルドで、ユーザーに用途を説明してから許可を求めてください。通知を無効にしたビルドでは通知APIを公開しません。[ビルド時に追加される権限と実行時の許可](#ビルド時に追加される権限と実行時の許可)

validateモードでは`muon:notifications`と`muon:android.notifications`からimportします。共通の投稿には`muon.notifications.post`、権限要求には`muon.android.notifications.requestPermission`など、実際に使う関数を許可してください。

```ts
import { post } from "muon:notifications";
import { requestPermission } from "muon:android.notifications";

const permission = await requestPermission();
if (permission.state.notificationsEnabled) {
  await post({
    id: "order-42",
    title: "Order ready",
    body: "Your order is ready to collect.",
    data: { orderId: 42 },
  });
}
```

静的チャンネルはアプリ起動時に登録します。動的に増やす場合は、投稿前に`ensureChannel()`を呼びます。既存チャンネルの重要度やユーザー設定は上書きしません。API 24・25にはチャンネル機能がないため、ビルド時に宣言した分類を使用し、`ensureChannel()`と`getChannel()`は非対応になります。`defaultCategory`は静的に宣言したIDを参照する必要があります。[Androidのチャンネル](https://developer.android.com/develop/ui/views/notifications/channels)

OS設定画面を開く場合は`muon.android.settings.open()`を使います。アプリ情報、通知全体、登録済みチャンネルの画面に対応します。復帰後には`getPermissionState()`や`getStatus()`を再取得してください。画面を開いた結果から、ユーザーが設定を変えたかどうかは分かりません。

通知操作はアプリが終了していても受信でき、ページ初期化前に受け取ったイベントも保存します。購読と未受領一覧を併用し、`eventId`で重複を除いた後に受領確認してください。APK更新でOS上の通知が削除される場合も、同じアプリIDとデータを維持する更新なら保存済みの未受領イベントを取得できます。自動再投稿は行いません。[イベントと保存上限](./muon-built-in-plugin-reference.md#通知イベントと受領確認)、[Androidの更新時の通知処理](https://android.googlesource.com/platform/frameworks/base/+/1cdfff555f4a21f71ccc978290e2e212e2f8b168/services/core/java/com/android/server/notification/NotificationManagerService.java#2010)

予約通知、FCM、通知から直接処理を完了するバックグラウンド操作は未提供です。通知ボタンは自アプリを開いて操作IDを渡します。画面の終了後も続ける処理には[サービス](services.md)を使用できます。

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

## サービスと未対応機能

サービスの種類や通信設定によって、APKに必要な権限が変わります。[ビルド時に追加される権限と実行時の許可](#ビルド時に追加される権限と実行時の許可)

FCMの通知配送とバックグラウンド処理は後続作業です。今回のAPKはFirebase設定なしで動作します。

QuickJSサービスは開発PCで単一ESMへbundleし、許可したnative moduleを外部参照として残します。コードをAPKへ同梱し、アプリと同時に更新します。永続データはコードから分離します。任意のnpmパッケージ、Node.js全体との互換性、JavaScriptだけの独立更新は提供しません。設定と実行環境は[サービスのガイド](services.md)を参照してください。

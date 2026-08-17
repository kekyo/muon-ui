## android移植

muonをandroidに対応させることを検討します。
  muonはCEFをバックエンドに使用しているため:
  - CEFのandroid実装を使う/移植する
  - CEFではなくandroid WebViewに接続する
  あるいはその他の方法が考えられます。
  これらについて考察して、いくつか案を示して下さい

---

## 結論

2026年8月時点では、第一候補は「Android WebViewを第2のバックエンドとして追加する」案です。

ただし、単純にCEFをWebViewへ置換するのではなく、

- JavaScript APIとネイティブ実装間のRPC
- プラグイン実行
- Capability判定
- ブラウザ・OS固有操作

をCEFから分離したうえで、デスクトップ用CEFアダプターとAndroid用WebViewアダプターを実装する形が適切です。

一方、「現行と完全に同じネットワーク制御」を必須にする場合、WebViewは不足する可能性があります。この要件をAndroid対応の採否ゲートにすべきです。

## 現在のmuonの構造

現在はCEFが単なる描画部品ではなく、アプリケーション基盤全体に入り込んでいます。

- 起動、サブプロセス、イベントループがCEF依存です。[main.cpp](/home/kouji/Projects/muon-ui/muon-core/src/main.cpp:77)
- アセット配信、プラグイン起動、ブラウザー生成が同じCEF初期化処理内にあります。[muon_app.cpp](/home/kouji/Projects/muon-ui/muon-core/src/app/muon_app.cpp:871)
- プラグインランタイム自体が`CefFrame`や`CefProcessMessage`を公開型に含んでいます。[muon_plugin_runtime.h](/home/kouji/Projects/muon-ui/muon-core/src/plugins/muon_plugin_runtime.h:14)
- ビルドも常にCEFを要求します。[CMakeLists.txt](/home/kouji/Projects/muon-ui/muon-core/CMakeLists.txt:59)
- 配布ターゲットはLinux/WindowsとCEFターゲットが1対1に結び付いています。[targets.ts](/home/kouji/Projects/muon-ui/muon-ui/src/targets.ts:7)

そのため、Android対応は「別のブラウザーコントロールを接続する」よりも「CEF依存部分とmuon本体の境界を作る」作業になります。

一方、Viteが生成する公開APIは最終的に`globalThis.__muon_plugin_call`へ集約されています。[capability.ts](/home/kouji/Projects/muon-ui/muon-ui/src/capability.ts:1339)
ここはAndroid用の別トランスポートを差し込める、かなり有効な既存の境界です。

## 案の比較

| 案 | 初期工数 | Chromium互換性 | エンジン固定 | 現行ネットワーク制御 | 維持費 | 評価 |
|---|---:|---:|---:|---:|---:|---|
| Android WebView | 中 | 高い | できない | 完全再現は困難 | 低〜中 | 第一候補 |
| GeckoView | 中〜大 | 異なる | できる | WebExtension等で設計しやすい | 中 | 有力な代替案 |
| CEF/ChromiumのAndroid移植 | 極大 | 高い | できる | 最大限制御可能 | 極大 | 通常は非推奨 |
| PWA/TWA版muon | 小〜中 | Chrome依存 | できない | ネイティブ機能を大幅制限 | 低 | muon-lite向け |

## 案1: Android WebViewバックエンド

最も現実的です。

構成は次のようになります。

```text
Vite仮想モジュール / muon公開API
                 │
          共通RPCプロトコル
      call / result / event / cancel
          ┌──────┴────────┐
          │               │
   CEF IPCアダプター   WebMessageアダプター
     デスクトップ          Android
          │               │
          └──────┬────────┘
                 │
    Capability・プラグイン共通ランタイム
                 │
       Desktopサービス / Androidサービス
```

Android側では次を使えます。

- アプリ内アセットは`WebViewAssetLoader`で`https://appassets.androidplatform.net/`などから配信する。`file://`を避け、Same-Origin Policyを維持できます。[WebViewAssetLoader公式資料](https://developer.android.com/reference/androidx/webkit/WebViewAssetLoader)
- JavaScriptブリッジは`WebViewCompat.addWebMessageListener`を使用する。
- 許可オリジンを厳密に指定し、`sourceOrigin`と`isMainFrame`も確認する。
- `addJavascriptInterface`は全フレームに公開され、呼び出し元オリジンを安全に特定できないため使用しない。[AndroidのJavaScriptブリッジ公式資料](https://developer.android.com/develop/ui/views/layout/webapps/native-api-access-jsbridge)
- C/C++側の共通ランタイムとはJNIで接続する。
- ネイティブプラグインはABI別にビルドし、APK/AABへビルド時に同梱する。[Android NDKのABI公式資料](https://developer.android.com/ndk/guides/abis)

注意点として、WebViewの実装・バージョンは端末上のプロバイダーに依存します。Android 7以降は複数パッケージから選択される場合もあります。[WebViewバージョン管理](https://developer.android.com/develop/ui/views/layout/webapps/managing-webview)

### 最大の問題: ネットワーク制御

現在のmuonは、

- URL全体に対するglob判定
- トップレベル遷移かどうか
- リクエスト発生元オリジン

を使って許可判定しています。[muon_network_policy.cpp](/home/kouji/Projects/muon-ui/muon-core/src/network/muon_network_policy.cpp:48)

WebViewの`shouldInterceptRequest`だけでは同等になりません。公式仕様上、

- リダイレクト後のURLでは呼び出されない
- `javascript:`と`blob:`は対象外
- `shouldOverrideUrlLoading`はPOSTなど全遷移を捕捉しない

という制約があります。[WebViewClient公式資料](https://developer.android.com/reference/android/webkit/WebViewClient)

WebView案では、次のいずれかを仕様として選ぶ必要があります。

1. Androidではオリジン単位のallowlistへ簡略化する。
2. WebViewからの外部ネットワークを禁止し、許可されたネイティブHTTPプラグイン経由に限定する。
3. `WebViewClient`、Service Worker用ハンドラー、ローカルプロキシを組み合わせて近似する。ただし現行との完全同等は保証しない。
4. 完全同等が必須ならWebView案を採用しない。

私は2が最も安全で明瞭、1が最も使いやすいと考えます。

## 案2: GeckoViewバックエンド

GeckoViewはアプリにエンジンを同梱できるため、テストしたバージョンと実際に動くバージョンを一致させられます。[GeckoView公式サイト](https://mozilla.github.io/geckoview/)

組み込みWebExtensionとNative MessagingによってJavaScriptブリッジを構築でき、WebExtension側でネットワーク要求を扱う設計も可能です。[GeckoViewのWebコンテンツ連携](https://firefox-source-docs.mozilla.org/mobile/android/geckoview/consumer/web-extensions.html)

利点は次の通りです。

- Webエンジンのバージョンを固定できる。
- ブラウザー用途を意識したAPIがWebViewより豊富。
- 組み込み拡張によるブリッジ、要求制御、スクリプト注入を設計しやすい。

欠点は次の通りです。

- CEFと異なるGeckoエンジンなので、Webアプリの挙動差が生じる。
- APKが大きくなる。
- CEFコードはほとんど再利用できず、別のバックエンド実装が必要。
- Chromium固有機能との互換性は失われる。

「Chromium互換性よりも、実行エンジンの再現性と制御性を重視する」場合の有力候補です。

## 案3: CEFまたはChromiumをAndroidへ移植する

現在のCEF公式バイナリはWindows、macOS、Linux向けで、Android向け配布はありません。[CEF公式ドキュメント](https://chromiumembedded.github.io/cef/general_usage.html)
現在の公式ビルド設定もLinux、Windows、macOSのみをサポートし、それ以外を`Unsupported platform`としています。[CEFのgn_args.py](https://github.com/chromiumembedded/cef/blob/master/tools/gn_args.py)

したがって、「CEFのAndroid実装を使う」は現状選べず、実質的には新規移植になります。

必要になるのはCEFの薄いAndroid対応だけではありません。

- AndroidのActivity/Viewライフサイクル
- Chromiumマルチプロセス起動
- サンドボックス
- GPU描画
- タッチ、IME、アクセシビリティ
- 権限要求
- APK/AABパッケージング
- Chromium更新への継続的追従

直接Chromiumの`android_webview`をアプリへ組み込む案もありますが、Chromium自身が「個別APKで利用するためのものではない」と明記しています。[Chromiumソース構造の公式説明](https://www.chromium.org/developers/how-tos/getting-around-the-chrome-source-code/)

これはmuonのAndroid対応というより、独自ブラウザーエンジン配布プロジェクトです。専任チームを置き、継続的にChromiumへ追従すること自体を目的にできる場合に限る案です。

## 案4: PWA/TWAによるmuon-lite

Android版ではネイティブプラグイン互換を求めず、

- Web UI
- Service Worker
- Custom TabsまたはTWA
- 最小限のAndroid連携

だけを提供する案です。

開発・保守コストは低い一方、ファイルシステム、任意ネイティブプラグイン、ウィンドウ操作など、muonの主要機能は提供できません。「既存muonアプリをそのままAndroidへ」という目的には合いませんが、モバイル向け閲覧クライアントとしては成立します。

## 推奨する進め方

最初からAndroid全機能を実装せず、以下を順に検証するのがよいです。

1. CEF非依存RPCを抽出する
   完了条件は、デスクトップCEF版の挙動と全テストが変更前と同じであることです。

2. WebViewによる最小試作を作る
   アセット表示、`getConfigValues`相当のPromise往復、エラー、キャンセル、バイナリ転送、Activity再生成を通すことを完了条件とします。

3. ネットワーク制御を検証する
   メインフレーム、iframe、fetch、XHR、WebSocket、リダイレクト、Service Worker、`blob:`、ローカルネットワークをdeny-by-defaultで検証します。現行契約を満たせなければ、Android仕様を変更するかGeckoViewへ進みます。

4. Android API対応表を定義する
   reload、fullscreen、zoomなどは実装候補です。一方、minimize、maximize、title bar、window bounds、system tray、launcher updater、Node sidecar、executorはAndroidでは非対応または別APIにすべきです。[ブラウザー組み込み機能一覧](/home/kouji/Projects/muon-ui/muon-core/src/browser/muon_builtin_browser.h:17)

5. ネイティブプラグインをNDK対応する
   まず`arm64-v8a`とエミュレーター用`x86_64`を対象にし、プラグインはビルド時同梱とします。

総合すると、「WebViewを採用し、RPCとプラグインランタイムをCEFから分離する」が推奨案です。ただし現行ネットワークポリシーの完全互換を必須とするなら、先にセキュリティ試作を行い、不成立ならGeckoViewを選ぶのが妥当です。CEFのAndroid移植は初手には勧められません。
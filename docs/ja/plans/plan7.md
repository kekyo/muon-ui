# Androidプラグインシステムの確立

## 初回調査と実装順序の案

現在は、Androidで外部ネイティブプラグインを動かす基盤とsimpleモードまで実装済みです。主な残作業は、通常のプラグイン設定・validateモード・Vite連携をAndroidの公開ビルド経路へ接続することです。

現在のコードを追うと、次の範囲まで実現しています。

| 項目 | 現状 |
|---|---|
| プラグインの同梱 | `android.plugins`で事前ビルド済み`.so`を登録。arm64-v8a・x86_64に対応 |
| ビルド時検査 | ABI、SONAME、`muon_init_plugin`、依存ライブラリ、16 KiB整列を検査 |
| ネイティブ実行 | デスクトップと共通の`MuonPluginRuntime`を使用。設定値の受け渡しと非同期呼出しを実装 |
| JSとの相互呼出し | プリミティブ型、文字列、バッファ、JSコールバック、ネストした関数型、ネイティブ関数プロキシに対応 |
| 寿命管理 | プロキシ解放、context解放、Activity再生成、非同期停止後のライブラリ解放を実装 |
| 組込みAPI | `browser`の8関数、`environments`の4関数、`fs`の22関数を公開 |
| Node.js／QuickJSとの分離 | 通常の配布アプリはQuickJSなしでプラグインを利用する構成 |

根拠は[プラグイン入力検査](/home/kouji/Projects/muon-ui/muon-android/src/plugins.ts:159)、[共通ランタイムの接続](/home/kouji/Projects/muon-ui/muon-android/runtime/src/main/cpp/muon_android_process_runtime.cpp:332)、[相互呼出しの端末テスト](/home/kouji/Projects/muon-ui/muon-android-prototype/android/app/src/androidTest/java/dev/muon/runtime/MuonActivityTest.java:573)、[組込みAPI一覧](/home/kouji/Projects/muon-ui/muon-android/src/renderer/android-api.ts:8)です。

未接続の箇所は、主に次の4点です。

1. 公開ビルドがsimpleモード限定です。`validate`と通常の`plugin.plugins`設定を拒否し、レンダラーへ渡すモードも`simple`に固定しています。[ビルド側の制限](/home/kouji/Projects/muon-ui/muon-ui/src/android.ts:188)

2. 権限制御の基盤はありますが、設定との接続が不十分です。外部プラグインの`allow`はネイティブ側でも検査します。一方、組込みAPIの許可は固定で、`sources`・`packages`から生成するcapabilityや`plugin.pages`によるページ選別には未接続です。現在のRPC境界は、固定のアセットoriginとメインフレームで制限しています。[RPC初期化](/home/kouji/Projects/muon-ui/muon-android/runtime/src/main/cpp/muon_android_rpc_jni.cpp:1376)

3. Vite側にAndroid向けの処理が必要です。関数一覧はデスクトップ用を含み、生成する呼出しコードもAndroidの引数・戻り値変換と統合されていません。また、外部プラグインのワイルドカード展開はホスト上で実行するinspectorに依存しています。Android用`.so`からビルド時メタデータを得る方法を決める必要があります。[モジュール生成](/home/kouji/Projects/muon-ui/muon-ui/src/capability.ts:1266)、[inspector呼出し](/home/kouji/Projects/muon-ui/muon-ui/src/plugin-inspector.ts:206)

4. 配布経路の継続検証に不足があります。外部プラグイン入りの独立利用アプリを検証する`--plugins`は存在しますが、現在のCIでは指定していません。[CI設定](/home/kouji/Projects/muon-ui/.github/workflows/ci.yml:151)

Android固有操作では、既存のfullscreen APIがステータスバーとナビゲーションバーをまとめて表示・非表示にします。ただし、現在のテストが確認しているのは内部のfullscreenフラグです。実際のバー表示や復帰動作の確認を、今回の計画に含める必要があります。バーの色や個別状態を変更する専用APIは、現行コードでは見つかりませんでした。[実装](/home/kouji/Projects/muon-ui/muon-android/runtime/src/main/java/dev/muon/runtime/MuonAndroidPlatformService.java:208)、[既存テスト](/home/kouji/Projects/muon-ui/muon-android-prototype/android/app/src/androidTest/java/dev/muon/runtime/MuonActivityTest.java:330)

なお、`launcher`、`executor`、`fs.dialogs`、デスクトップ固有のウィンドウ操作は現在公開されていません。これらのAndroid向け新規設計と、今回のプラグインシステムの確立は、計画上で区別します。

この調査を踏まえた実装順序の案は、次のとおりです。

| 段階 | 成果物と完了条件 |
|---|---|
| 1. 設定とsimpleモードを接続 | 組込み・外部プラグインの公開範囲を設定で制御でき、許可・拒否をAPK上で確認できる |
| 2. 組込みAPIのvalidate対応 | 現在の34関数をvirtual moduleから利用でき、未許可のimportと未対応APIをビルド時に拒否できる |
| 3. 外部プラグインのvalidate対応 | メタデータ取得方法を確定し、外部関数のimport、ワイルドカード、コールバック、解放を配布APKで確認できる |
| 4. 配布・端末検証を完成 | QuickJSなしで両ABIを検証。既存fullscreen操作は実表示と遷移を確認し、必要な動画検証、CI、全体テストまで完了する |

各段階で再現テストのRED/GREENを確認し、コミットする計画にします。新しいAndroid固有機能は追加対象に含めません。

今回実行した関連Vitestは86件すべて成功しました。端末テストの再実行はまだ行っておらず、コード変更もありません。

## 追加調査：JSからのプラグイン参照を制限できる範囲

2026年10月8日、`b2d54af2f5618eef7018d47cfef0e97fbf411e02`のコードを基準に調査した。前節は初回の分析をそのまま保存したものである。この追記では、初回の分析で残していたフィルタの意味とAndroidでの保証範囲を具体化する。実装はまだ開始していない。

調査の結果、Androidでのvalidateモードを妨げる一律のWebView制約は確認できなかった。`sources`・`packages`によるビルド時のimport検査と、capabilityによる実行時の関数権限検査はAndroidでも実装できる。未接続の設定、メタデータ、レンダラーを接続する作業として扱う。

一方、現在使用しているWebMessage APIでは、プラグイン呼出しを行ったJSファイルやnpmパッケージをネイティブ側で認証できない。同じページ内で実行するJSをファイルごとに隔離する保証は、現在のCEF版にもない。CEFとの差として確認できたのは、WebMessage APIが送信元文書の完全URLを通知せず、注入条件にもURLのパスを指定できないことである。ページURLの選別と、JSファイルの識別は区別する必要がある。

### CEF版の「細かなフィルタ」は何を検査するか

| 層 | 現在の処理 | 保証の範囲 |
| --- | --- | --- |
| Viteによるimport検査 | virtual moduleのimport元を`plugin.imports`の`sources`または`packages`と照合する | 許可していないソースやパッケージからの直接importをビルド時に拒否する |
| ページ単位の公開制限 | `CefFrame::IsMain()`と`GetURL()`を使い、ページURLのポリシーを検査する | 条件を満たすメインフレームにだけAPIを公開し、ブラウザ側でも呼出し時に再検査する |
| 関数単位の権限検査 | ランダムなcapability IDに許可関数を対応付け、要求の関数パスと実際の呼出し先を照合する | IDの欠落、不明なID、許可外の関数、関数パスと実体の不一致を拒否する |

import元の照合とIDの生成は[capability.ts](../../../muon-ui/src/capability.ts)の`matchesCapabilityRuleImporter`、`createMuonCapabilityModuleResolver`、`createCapabilityId`で行う。これはViteが把握しているビルド時のソース情報を使う処理であり、CEF固有の機能ではない。許可されたモジュールから関数を再公開したり、別のコードへ渡したりする利用まで追跡する仕組みではない。

ページの検査は[muon_app.cpp](../../../muon-core/src/app/muon_app.cpp)の`ShouldExposeMuonApi`と[muon_client.cpp](../../../muon-core/src/browser/muon_client.cpp)の`IsPluginPageAllowed`にある。APIの意味は[CEFのCefFrameリファレンス](https://cef-builds.spotifycdn.com/docs/147.0/classCefFrame.html)と、使用中のCEF 147.0.14の`include/cef_frame.h`のコメントでも確認した。

validateモードの呼出し入口は、ページのglobalに置く`__muon_plugin_call`である。[muon_v8_handler.cpp](../../../muon-core/src/plugins/muon_v8_handler.cpp)は、JSから渡されたIDと関数パスを受け取り、共通の[muon_rpc_host.cpp](../../../muon-core/src/rpc/muon_rpc_host.cpp)の`ValidateCapability`が権限を検査する。実行中のJSファイル名やnpmパッケージ名との照合は行っていない。

したがって、同じページ内のコードが別のモジュールの有効なIDを取得すれば、そのIDに許可された関数を呼び出せる。IDを得ても、そのIDに許可していない関数を呼べるわけではない。この限界は既に[virtual moduleインポートの制約](../limitation.md#virtual-moduleインポートのフィルタ機能)にも記載されている。Android対応によって初めて生じる制約ではない。

### Androidで既にある検査と未接続の部分

[MuonWebViewHost.java](../../../muon-android/runtime/src/main/java/dev/muon/runtime/MuonWebViewHost.java)は、`addWebMessageListener`と`addDocumentStartJavaScript`に`https://main.asset.muon.invalid`の完全一致originを渡している。[MuonRpcBridge.java](../../../muon-android/runtime/src/main/java/dev/muon/runtime/MuonRpcBridge.java)の`onPostMessage`でも、WebViewが渡した`sourceOrigin`と`isMainFrame`を検査する。JSがメッセージ本文へ書いたoriginを信用する構成ではない。

[muon_android_rpc_jni.cpp](../../../muon-android/runtime/src/main/cpp/muon_android_rpc_jni.cpp)は、共通RPCホストを内部的には`MuonRpcHostMode::Validate`で作成している。外部プラグインの`allow`に加え、組込みAPIについても固定IDと固定の許可関数で検査する。公開設定がsimple限定であることと、ネイティブ側で関数権限を検査していないことは同義ではない。

残っているのは、Viteで生成するIDと許可関数をAndroidの設定、レンダラーメタデータ、共通RPCホストへ一貫して渡す処理である。現行の組込み用固定IDを残したままvalidateモードを追加すると、意図したimport制限を通らない呼出し経路が残るため、この接続と切替を同じ段階で検証する。

また、CEFでは`__muon_plugin_call`を読取り専用かつ削除不可で公開する一方、Androidの[webview-rpc.ts](../../../muon-android/src/renderer/webview-rpc.ts)では書換え・再定義が可能である。この差は実装上の検討事項であり、WebViewのorigin制限とは別である。入口の属性を揃えても、同じページ内のJSファイルを認証する保証にはならない。

### WebMessage APIの仕様から確定できる制約

現行依存は[runtime/build.gradle.kts](../../../muon-android/runtime/build.gradle.kts)にある`androidx.webkit:webkit:1.17.0`である。以下は公式APIリファレンスに加え、Google Mavenが配布する[1.17.0のsources.jar](https://dl.google.com/dl/android/maven2/androidx/webkit/webkit/1.17.0/webkit-1.17.0-sources.jar)内の`WebViewCompat.java`、`JavaScriptExecutionWorld.java`、`JavaScriptReplyProxy.java`のAPIコメントでも確認した。

| 確認事項 | APIの仕様 | Muonへの影響 |
| --- | --- | --- |
| メッセージの送信元 | `onPostMessage`はフレームのoriginとメインフレームかどうかを通知する | 外部originとサブフレームからの直接RPCを拒否できる |
| JSファイルの識別 | 呼出し元スクリプトのURL、Viteのimport元、npmパッケージ名は通知されない | これらを実行時のネイティブ認証条件にはできない |
| 注入条件 | `allowedOriginRules`はscheme、host、portを対象とする | 同一origin内の`/allowed.html`と`/denied.html`を注入規則だけでは区別できない |
| 送信元文書のURL | `sourceOrigin`にpath、query、fragmentは含まれない | CEFのページURLポリシーを、そのままWebMessageの引数検査へ移植できない |
| サブフレームへの注入 | 許可originに一致する各フレームへオブジェクトを注入する | 同一originのiframeにオブジェクトが存在しないとは保証できない。RPC受理時の`isMainFrame`検査が必要 |

送信元情報の根拠は[WebMessageListener](https://developer.android.com/reference/androidx/webkit/WebViewCompat.WebMessageListener)、注入条件の根拠は[WebViewCompat](https://developer.android.com/reference/androidx/webkit/WebViewCompat)を参照する。

同じ信頼済みページで動く外部scriptは、そのページのJSとしてRPCを呼べる。たとえば、アセットのメインページが外部の`widget.js`を読み込んでも、RPCの`sourceOrigin`が`widget.js`の配信元になるわけではない。simpleモードでは公開API、validateモードでは取得できた有効なcapabilityの範囲で呼出しが可能になる。

このAPIの情報だけでは、メインフレーム自身の操作と、メインフレームが外部iframeの要求を受けて代理した操作も区別できない。同一originのiframeから親ページへアクセスする経路についても、iframe内の直接RPCを拒否しただけで隔離が完成するとは扱わない。[過去の検証で確定した公開境界](../../../filter-limitation.md#muonプラグインの公開境界)は、この意味で引き継ぐ。

### ページURLのフィルタを「不可能」と断定する範囲

確定できるのは、現在の注入規則とWebMessageの送信元情報だけでは、CEFと同じ完全URLのフィルタを直接構成できないことである。Android上でページ別制御を実現するあらゆる設計が不可能だとする根拠にはならない。

JSから自己申告したURLを送るだけでは、呼出し側が値を変更できる。また、別のタイミングで取得したWebViewのURLを、そのメッセージを送った文書のURLとみなすには、遷移や文書の寿命との対応を検証する必要がある。

現在のAndroidXには、返信先フレームと実行worldでJSを実行する[JavaScriptReplyProxy.executeJavaScript](https://developer.android.com/reference/androidx/webkit/JavaScriptReplyProxy)もある。しかし、これは非同期のJS実行であり、メッセージに送信時の完全URLが付くAPIではない。文書単位の認証や遷移状態を組み合わせた別方式は追加設計と検証の対象になる。本計画では、その方式によるCEFの`plugin.pages`との完全互換を完了条件に含めない。

現行の[android-config.ts](../../../muon-android/src/renderer/android-config.ts)は`plugin.pages`の明示指定を警告し、公開originを変更しない。これは現在の実装上の扱いである。今回の設定接続では、未対応の狭いページ条件を無視して広いorigin全体へ許可してしまう挙動を避け、対応範囲外の設定はビルド時のエラーとして利用者へ返す方針とする。受理する設定値と省略時の扱いは、段階1で共通設定との整合を確認して記録する。

### isolated worldは現行バージョンにもある

[JavaScriptExecutionWorld](https://developer.android.com/reference/androidx/webkit/JavaScriptExecutionWorld)はAndroidX WebKit 1.16.0で追加されており、現在採用している1.17.0にも存在する。`getExecutionWorld`、worldを指定する`addWebMessageListener`、`addJavaScriptOnEvent`を使う公開APIがある。したがって、「現在のAndroidXには独立したJS実行環境がない」とは記載しない。

利用できるかどうかは、端末のWebView providerに対して`WebViewFeature.isFeatureSupported(JS_INJECTION_IN_FRAME_AND_WORLD)`で確認する必要がある。依存ライブラリのバージョンだけでは対応を保証できない。APIコメントでは、注入したJSから呼べるメッセージリスナーは同じworldに登録したものに限られる。[WebViewCompatの公式仕様](https://developer.android.com/reference/androidx/webkit/WebViewCompat)

現在のMuonは通常のページworldへAPIとbootstrapを注入している。別worldへブリッジを移すと、通常のViteアプリからの呼出し方法も変える必要がある。ページ側へ汎用RPCの中継口を戻せば、そこで動く各JSファイルを識別できない問題は残る。isolated worldを採用するには、信頼するコードの配置とworld間のインターフェイスを別途設計する必要がある。本計画のvalidate対応の前提にはしない。

### ネットワークフィルタとの区別

過去の[検証後の確定方針](../../../filter-limitation.md#android-webview検証後の確定方針)で再現できないと判断したのは、CEF相当の通常ネットワーク全体に対する選択的な制御である。プラグインのimport検査や関数権限検査まで不可能と判断したものではない。

`WebResourceRequest.getUrl()`は取得先URLであり、呼出し元JSや要求元originを表す情報ではない。[WebResourceRequestの公式仕様](https://developer.android.com/reference/android/webkit/WebResourceRequest)

`shouldInterceptRequest`もリダイレクト後の各URLでは呼ばれず、`blob:`など捕捉対象外の経路がある。これらの通信フックを、プラグイン呼出し元の認証手段として代用できるとは扱わない。Androidで`network.allow`などを通常通信の包括的な制限として提供しない既存方針を維持する。[WebViewClientの公式仕様](https://developer.android.com/reference/android/webkit/WebViewClient)

## 本計画で採用する公開範囲と実装段階

Androidのプラグインは、信頼するアセットoriginのメインフレームからのRPCだけを受理する。現行の対象は`https://main.asset.muon.invalid`である。関数の許可はネイティブ側でも検査し、validateモードではViteが生成したcapabilityと対応させる。`sources`・`packages`はビルド時の直接importを制限する設定として提供する。

保証しない範囲は、同じページ内のJSファイルごとの実行時認証、capabilityを取得した同じページ内のコードからの利用防止、CEFのページURL globとの完全互換、通常ネットワーク全体の選択的制御である。サブフレームについては「直接RPCを拒否する」と表現し、同一originのサブフレームからオブジェクトが一切見えないとは記載しない。既存の[制約文書](../limitation.md#android-webviewバックエンド)にある「公開しません」の表現も、実装段階でこの意味に揃える。

この範囲は、同一ページを共有する現行構成と現在採用しているAPIに対する仕様である。将来のWebView APIや、別world・別originへコードを分離する構成まで不可能と断定しない。外部scriptの読込み制限にはCSPを併用し、信頼済みページに無制限のRPC中継処理を置かない前提を利用者へ説明する。

初回の4段階を維持し、追加調査による条件を次のように補う。各段階で動作を確認できるアプリを残し、問題を再現するテストのRED確認、実装、GREEN確認を行ってからコミットする。

| 段階 | 実装内容 | 観測できる成果物・完了条件 |
| --- | --- | --- |
| 1. 設定とsimpleモード | 共通の組込み・外部プラグイン設定をAndroidの登録と許可関数へ接続する。`android.plugins`のABI別ライブラリ指定との役割を整理する。`plugin.pages`の対応値、省略値、未対応値の診断を確定する | simpleモードのAPKで許可した関数だけが動く。直接RPCを組み立てても許可外の関数を実行できない。適用できないページ条件を指定するとビルドが失敗する |
| 2. 組込みAPIのvalidate対応 | Android用の関数一覧と引数・戻り値変換をvirtual moduleへ接続する。生成したcapabilityをAPKとネイティブ側へ渡す。simple用の固定IDや入口をvalidateの迂回経路として残さない | 現在の34関数を対象とするvalidateモードのアプリが動く。許可外importと未対応APIをビルド時に拒否し、不明ID・許可外関数の直接呼出しも拒否する |
| 3. 外部プラグインのvalidate対応 | Android用`.so`のメタデータをビルド時に得る方法を確定する。ホスト用inspectorでAndroid用バイナリを実行できる前提を置かない。完全名とワイルドカードの関数解決、型変換、コールバックとプロキシ解放を接続する | 配布APKで独自プラグインをvirtual moduleから呼べる。許可・拒否、双方向コールバック、バッファ、解放を確認できる。メタデータと同梱ライブラリが食い違う場合の診断も確認する |
| 4. 配布・端末検証 | 独立したnpm利用アプリのプラグイン検証をCIへ接続する。文書と公開型を仕様に揃え、既存Android操作を端末で検証する | QuickJSなしの配布APKをarm64-v8aとx86_64で確認する。fullscreenによる実際のシステムバー表示・復帰と時間軸の挙動を確認する。関連する端末・配布テストと全体テストが成功する |

段階1で設定形式、段階3でメタデータ取得方式を確定した際には、採用理由と具体的な設定・検証方法を本計画へ追記する。これらは残っている設計判断であり、今回確認したWebMessageの仕様上の制約とは区別する。

Node.js、QuickJSの製品化、新しいAndroid固有API、独立したナビゲーションバー色・状態変更APIは対象に含めない。既存のfullscreen実装が行うステータスバー・ナビゲーションバー操作は検証対象とする。未実装の`launcher`、`executor`、`fs.dialogs`を追加することも、本計画の完了条件にしない。

### 権限制御の完了判定に使うケース

| ケース | 期待する結果 | 主な確認段階 |
| --- | --- | --- |
| 許可・未許可のソースおよびnpmパッケージから直接import | 許可したものだけビルド成功 | 2、3 |
| 許可capabilityと許可関数の組合せ | ネイティブ関数を実行して結果が返る | 2、3 |
| ID欠落、不明ID、許可外関数、関数パスと実体の不一致 | ネイティブ関数を実行せず拒否 | 1〜3 |
| validateモードでsimple用の固定capabilityを使う | 許可を迂回できない | 2、3 |
| 許可アセットoriginのメインフレーム | 設定された関数のRPCを受理 | 1〜4 |
| 外部originのメインフレーム、外部iframe、同一originのiframe内のブリッジから直接送信 | RPCを受理せず、対象のネイティブ関数が実行されない | 1、4 |
| 同じ信頼済みページに読み込んだ別scriptへ有効なcapabilityを渡す | そのIDの許可範囲では実行できる。JSファイル隔離を保証していないことを確認 | 2、4 |
| 適用できないページURL条件を設定 | 意図した制限を広げず、ビルド時に診断 | 1 |
| reload、ページ遷移、Activity再生成、終了後に古いcallbackやproxyを利用 | 古いcontextの資源を再利用できず、現行contextの処理が壊れない | 2〜4 |

iframe拒否の検証では、応答が来ないことだけを根拠にせず、ネイティブ側の実行回数や状態も観測する。同じページ内の別scriptへ権限を渡すケースは、既知の有効IDをテストから渡して制約を確認する。ランダムIDの推測に依存したテストにはしない。外部scriptのCSP違反やネットワーク失敗を、プラグイン権限による拒否と取り違えない。

## 今回の調査の完了確認

今回の成果物は、初回分析の保存、現行コードと公式APIの照合、保証範囲の確定、段階ごとの実装・検証条件の記録である。Androidプラグイン対応そのものの完了とは区別する。

- 初回分析を本書の先頭へそのまま保存した。
- CEFのimport検査、ページ検査、capability検査と、Androidの現在の接続をコードで確認した。
- AndroidX WebKit 1.17.0の公式配布ソースとAPIリファレンスで、通知される送信元情報、origin規則、実行worldのAPIを確認した。
- 初回調査では関連Vitest 86件が成功した。追加調査では`muon-ui/test/vite.test.ts`からimport元・packageの許可と拒否、`muon.json`との接続、許可関数生成、capability ID共有の6件を実行し、すべて成功した。この追加実行では他の77件を選択対象外とした。
- 接続中のPixel 6はWebView provider `com.google.android.webview`の153.0.8010.36を使用していた。providerのバージョン確認だけを行い、isolated worldのfeature flagや新しいAPKの端末動作は今回検証していない。
- yomiyasuの文書検査を行い、追記の表現を調整した。文書内のローカルリンク26件は参照先ファイルの存在を確認した。
- 変更は本計画書だけである。文書変更に伴うビルド、全体テスト、端末テストは行っていない。これらを実装完了時に実行する条件は前節に残した。

## 実施記録

2026年10月8日、`feature/plan7`で実装を開始した。実機検証にはPixel 6を使用する。

### 段階1で確定した設定

`plugin.plugins`を権限とプラグイン固有設定の指定箇所とし、`android.plugins`は同名プラグインのSONAMEとABI別ライブラリの指定に使う。Android側にライブラリだけを指定してプラグインを公開する方式は終了する。ライブラリと共通設定が対応しない場合や、登録名が重複する場合はビルド時にエラーにする。

simpleモードでは、`plugin.plugins`を省略すると現在実装している34個の組込み関数を公開する。明示した場合は、そのリストだけを使用し、`internal`を省略すれば組込み関数を公開しない。組込み関数のワイルドカードはAndroidで実装している関数へ展開し、未対応の完全名や一致しないパターンはエラーにする。

`plugin.pages`の省略時は、信頼するアセットoriginのメインフレームへ公開する。明示値は`asset://main/**`と`https://main.asset.muon.invalid/**`だけを受理する。空配列ではブリッジの注入とRPC受理を停止する。他のhost、パス単位の条件、任意originを表す`*`などはエラーにする。設定した制限を警告だけで無視する処理は残さない。

組込み関数の許可リストはAPKの設定へ収録し、ネイティブ側のポリシーとレンダラーに渡す公開関数一覧へ反映する。JS側で関数を隠すだけでなく、直接組み立てたRPCにも同じ許可判定を適用する。関数一覧はDOM型に依存しないモジュールへ分離し、Node.jsで動くビルド処理とレンダラーで共有する。

段階1では共通設定の拒否、未許可関数の公開、適用できないページ条件の受理をテストで再現してから修正した。`muon-ui`のAndroid・共通設定テスト27件、`muon-android`の全47件、試作の全23件が成功した。Androidランタイムと試作は両ABIでビルドできた。

Pixel 6では、新しくpackしたnpm配布物を独立した利用アプリへ導入し、`test-packaged-application.mjs --plugins`を実行した。共通設定からの外部プラグイン登録と設定値の受渡し、組込み関数の非公開化、許可外の`fs.unlink`を直接RPCで呼んだ場合の拒否を確認した。debug APKと署名付きrelease APKで操作・再読込み・再起動が成功し、releaseの更新後も保存データが維持された。段階1の完了条件を満たした。

### 段階2の接続方式

Androidを対象とするViteビルドでは、組込み関数の一覧を34関数に限定する。virtual moduleは公開APIの引数でブリッジを呼び、ブリッジが既存のAndroid用変換処理を使う。環境情報のJSON、ファイル操作のオプション、バイナリ、watchの変換をsimpleモードと共有する。未実装の完全名と、一致する関数がないワイルドカードはビルド時に拒否する。

CLIは、実際にJSを生成したViteプラグインからcapabilityを受け取り、そのIDと許可関数をAPKへ収録する。設定を読み直してIDを作り直す処理にはしない。CLIのAndroid指定はViteの関数選択にも反映する。Androidとデスクトップでは呼出し変換が異なるため、両者のJSバンドルは別々にビルドする。[Viteのbuild API](https://vite.dev/guide/api-javascript.html#build)と[configResolvedフック](https://vite.dev/guide/api-plugin.html#configresolved)を使用し、導入済みViteの型定義コメントも確認した。

ネイティブ側はvalidateモードで生成済みcapabilityだけを登録する。simple用の固定IDと外部プラグイン名をcapabilityとして受理しない。組込みAPIのルートも共通設定で許可した関数に限定する。公開型が不足していた`muon:fs`と環境情報のvirtual module宣言は、この段階で補った。

関連するVite・Androidビルドの112件とレンダラーの15件が成功した。実機用の観測テストでは、アプリがエラーを表示した場合に、その内容を即座に失敗理由として返すようにした。

Pixel 6の独立したnpm利用アプリで、組込みAPIのvalidateモードがdebug・署名付きreleaseの両方で動作した。ID欠落、不明ID、simple用の固定ID、許可されたIDによる別名前空間の呼出しは拒否された。同じページ内の別scriptへ有効なIDを渡すと、その許可範囲内の呼出しは成功した。再読込み・再起動・release更新後の保存データ維持も確認し、段階2の完了条件を満たした。

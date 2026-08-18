# Android WebView版ネットワークフィルタの制約

## 結論

Android WebView版では、CEF版の選択的なネットワーク許可規則を再現しない。WebViewを外部ネットワークへ接続しないレンダラーとして扱い、アプリ内アセットと画像用`data:` URLだけを許可する。

この仕様変更を受け入れる場合、`plan.md`のステップ3は完了と判断できる。CEF版と同じ`network.allow`、`network.authorizedOrigin`、`network.localAccess`の動作が必須の場合、WebView案は採用条件を満たさないため、GeckoViewなど別のバックエンドを検討する必要がある。

## 確定したAndroid仕様

許可するURLは次の2種類に固定する。

| URL | 条件 | 用途 |
|---|---|---|
| `https://appassets.androidplatform.net/assets/`配下 | ホスト名とスキームは大文字小文字を区別せず、ポートは省略または443、パスは`/assets/`で始まること | `WebViewAssetLoader`から配信するアプリ内アセット |
| `data:image/`で始まるURL | URL文字列が小文字の`data:image/`で始まること | インライン画像 |

これ以外の`http:`, `https:`, `ws:`, `wss:`, `file:`, `content:`, `blob:`、画像以外の`data:`は、外部通信または遷移の許可対象にしない。Android版では設定ファイルの許可規則によってこの固定リストを拡張できない。

リリースビルドには`INTERNET`と`ACCESS_LOCAL_NETWORK`の権限を宣言しない。`android:usesCleartextTraffic`も`false`のままとする。計装テストはフィルタ自身の効果をOSの権限拒否と区別する必要があるため、デバッグビルドに限って両権限と平文HTTPを有効にする。

実装は[MuonWebViewNetworkFilter.java](muon-android-prototype/android/app/src/main/java/dev/muon/prototype/MuonWebViewNetworkFilter.java)に集約した。

## 遮断に使う層

単独のWebViewコールバックには全通信を捕捉できないため、次の層を同時に適用する。

| 層 | 設定または動作 |
|---|---|
| リリースManifest | `INTERNET`と`ACCESS_LOCAL_NETWORK`を付与しない。平文通信を禁止する |
| `WebSettings` | `blockNetworkLoads=true`、ファイルアクセスと`content:`アクセスを無効化し、mixed contentを禁止する |
| ナビゲーション | 固定許可リスト外を`shouldOverrideUrlLoading`で中止する |
| リソース要求 | アプリ内アセットを`WebViewAssetLoader`から返し、それ以外をHTTP 403相当の`WebResourceResponse`で拒否する |
| Service Worker | ファイル、`content:`、ネットワークアクセスを無効化し、専用コールバックでも全要求をHTTP 403相当で拒否する |
| CSP | `default-src 'none'`を基準に、`connect-src`, `frame-src`, `worker-src`, `object-src`, `form-action`などを`'none'`にする。スクリプトとスタイルは`'self'`、画像は`'self' data:`だけを許可する |

必要な`androidx.webkit.WebViewFeature`が1つでも利用できないWebViewプロバイダーでは、安全性を弱めて起動せず、`IllegalStateException`でfail-closedとする。このため、古いWebViewプロバイダーではAndroid版が起動できない可能性がある。

## 実測結果

2026年8月18日に次の環境で確認した。

- Pixel 6 AVD
- Android 17、API level 37、x86_64
- `com.google.android.webview` 149.0.7827.5
- アプリのtarget SDK 37

テストではアプリへ`INTERNET`と`ACCESS_LOCAL_NETWORK`を付与し、WebViewと同じアプリプロセス内で実TCPサーバーを起動した。JavaScript側の成功・失敗だけではなく、サーバーが要求を受信したかどうかを最終判定に使用した。

リリースAPKも別途生成し、Manifestの実体に`INTERNET`と`ACCESS_LOCAL_NETWORK`が含まれず、`android:usesCleartextTraffic=false`であり、デバッグ専用の観測Activityが含まれないことを確認した。

| 経路 | deny-by-default時の結果 | 追加の実測事項 |
|---|---|---|
| メインフレーム | 外部URLはサーバーへ到達しない | アプリから直接`loadUrl()`した場合も遮断できた |
| iframe | 外部URLはサーバーへ到達しない | 遮断時にもiframeの`load`イベントが発生し得るため、DOMイベントは許可判定の根拠にできない |
| `fetch` | Promiseが拒否され、サーバーへ到達しない | CSPとネットワーク遮断設定が有効だった |
| XHR | `error`となり、サーバーへ到達しない | CSPとネットワーク遮断設定が有効だった |
| WebSocket | 接続エラーとなり、ハンドシェイクはサーバーへ到達しない | 通常の`shouldInterceptRequest`には要求自体が現れない |
| リダイレクト | 初回URLもリダイレクト先もサーバーへ到達しない | フィルタを外した観測では初回URLだけが通常コールバックに現れ、リダイレクト先は現れなかった |
| Service Worker | 登録をCSPで拒否し、Workerのネットワークも無効化する | フィルタを外した観測ではWorkerからの`fetch`はService Worker専用コールバックにだけ現れた |
| `blob:` | iframe内の`blob:`文書を利用できず、その内部リソースもサーバーへ到達しない | フィルタを外すと`blob:`文書は読み込めるが、通常の`shouldInterceptRequest`には`blob:` URLが現れなかった |
| ローカルネットワーク | エミュレーターのプライベートIPv4アドレスへの`fetch`はサーバーへ到達しない | テストでは`ACCESS_LOCAL_NETWORK`を明示的に許可しており、OS権限ではなくフィルタが遮断した |
| `data:image/` | 1ピクセルGIFを正常に読み込める | 固定許可リストの例外として維持する |

この結果を固定する計装テストは[MuonNetworkFilterTest.java](muon-android-prototype/android/app/src/androidTest/java/dev/muon/prototype/MuonNetworkFilterTest.java)にある。コールバック範囲の観測には、リリースAPKへ入らないデバッグ専用の[MuonNetworkCapabilityProbeActivity.java](muon-android-prototype/android/app/src/debug/java/dev/muon/prototype/MuonNetworkCapabilityProbeActivity.java)を使用する。

## CEF版との相違

| 項目 | CEF版 | Android WebView版 |
|---|---|---|
| URL許可 | `network.allow`のURL全体globで追加可能 | 固定したアプリ内アセットと`data:image/`だけ。外部URLの追加は不可 |
| 許可オリジン | `network.authorizedOrigin`により、トップレベル遷移の宛先またはサブリソースのrequest initiatorを判定 | 未対応。外部オリジンは常に拒否 |
| request initiator | CEFコールバックから独立した値を取得して判定 | `WebResourceRequest`には相当するプロパティがない。`Referer`や`Origin`ヘッダーは全経路で得られる保証がなく、信頼できる代替にはしない |
| メインフレーム判定 | ナビゲーションとフレーム情報から判定 | `isForMainFrame()`は利用できるが、他の不足を補えない |
| リダイレクト | 各要求をネットワークポリシーで評価可能 | `shouldInterceptRequest`は最初のURLだけを通知し、リダイレクト先を再評価できない |
| WebSocketと`blob:` | 通常のネットワーク要求は`CefResourceRequestHandler`へ集約してポリシーを適用する | WebSocket要求と`blob:` URLは通常の`shouldInterceptRequest`に現れないため、このコールバックによる選択的許可には使えない |
| Service Worker | ブラウザー単位のCEF要求処理と同じポリシーへ接続できる | 別のService Worker用コールバックが必要で、その設定はプロセス全体に作用する |
| ローカルネットワーク | `network.localAccess.loopbackOrigins`と`localNetworkOrigins`を要求元オリジン単位で判定 | Android 17の`ACCESS_LOCAL_NETWORK`はホストアプリの実行時権限であり、WebViewもその状態を継承する。オリジン単位の許可にはできない |
| 拒否時の見え方 | HTTP 403応答とmuonの診断メッセージを返す | 経路によりHTTP 403、ナビゲーション中止、CSP違反、一般的なネットワークエラーのいずれかになる。CEF版と同じエラー文字列は保証しない |
| エンジンの固定 | 配布物に含めたCEFバージョンで固定 | 端末で選択されたWebViewプロバイダーと更新状態に依存する |

Androidの`WebViewClient`仕様でも、`shouldInterceptRequest`はリダイレクト後のURL、`javascript:`、`blob:`を通知せず、`shouldOverrideUrlLoading`はアプリ自身の`loadUrl()`とPOSTを含む全遷移を通知しないと明記されている。[WebViewClient API](https://developer.android.com/reference/android/webkit/WebViewClient)

Service Workerには別のコントローラーと設定が必要である。[ServiceWorkerControllerCompat API](https://developer.android.com/reference/androidx/webkit/ServiceWorkerControllerCompat)、[ServiceWorkerWebSettingsCompat API](https://developer.android.com/reference/androidx/webkit/ServiceWorkerWebSettingsCompat)

Android 17では`ACCESS_LOCAL_NETWORK`がtarget SDK 37以上のアプリに対する実行時権限となり、WebViewのローカル通信もホストアプリの権限状態を継承する。[Local network permission](https://developer.android.com/privacy-and-security/local-network-permission)

## 運用上の制約

- Android WebView内から外部HTTP、WebSocket、Service Worker通信を利用するアプリは動作しない。
- Android版では`network.allow`、`network.authorizedOrigin`、`network.localAccess`を設定しても外部通信を有効化しない。
- 外部通信が必要になった場合は、URL、リダイレクト、DNS解決後のアドレス、ローカルネットワーク、応答サイズをネイティブ側で一元検証できるHTTPプラグインを別ステップで設計する。WebView設定だけを部分的に緩和しない。
- デバッグManifestのネットワーク権限と平文通信許可は計装テスト専用である。リリースManifestへ移してはならない。
- WebView更新でコールバック範囲やCSP動作が変わり得るため、サポートするAndroidバージョンとWebViewプロバイダーごとに計装テストを再実行する。
- CEF版とのネットワーク互換が製品要件へ戻った場合は、このフィルタを拡張して近似せず、WebView採用判断そのものを再度行う。

## 完了条件との比較

`plan.md`のステップ3が挙げたメインフレーム、iframe、`fetch`、XHR、WebSocket、リダイレクト、Service Worker、`blob:`、ローカルネットワークはすべて実WebViewと実TCPサーバーで検証した。deny-by-default時は全経路でサーバー到達がなく、固定許可対象のアプリ内アセット、RPC、`data:image/`は引き続き動作した。

一方、CEF版の選択的ネットワーク契約は満たしていない。したがって、ステップ3の最終判定は「Android仕様を外部ネットワーク禁止へ変更する条件で完了」である。

----

# PoC実装後の分析結果

整理すると、その方針は「WebViewが発行するすべての通信を完全に再現するプロキシ」ではなく、「WebViewから直接通信させず、公開APIで代理可能なHTTP(S)要求だけをmuonが処理する」という仕様なら成立します。

ただし、任意の既存Webページを無変更で動かせるほど全面的なプロキシにはなりません。

## 改訂後の構成

```text
WebViewのHTTP(S)要求
        │
        ▼
shouldInterceptRequest
        │
        ├─ 対応できない要求 → fail-closed
        │
        ▼
network.allowで宛先を検査
        │
        ▼
muonネイティブHTTPクライアント
        │
        ├─ 各リダイレクト先を再検査
        │
        ▼
Manifest・Android実行時権限
        │
        ▼
外部ネットワーク
        │
        ▼
WebResourceResponseとしてWebViewへ返却
```

WebViewとService Workerには`blockNetworkLoads=true`を設定し、WebView自身によるネットワークアクセスは常に禁止します。[WebSettings API](https://developer.android.com/reference/android/webkit/WebSettings#setBlockNetworkLoads(boolean))

## Android版のネットワーク契約

| 設定 | Android版での扱い |
|---|---|
| `network.allow` | 対応可能なHTTP(S)要求に対する、アプリ全体の宛先allowlistとして機能する |
| `network.authorizedOrigin` | 非対応。設定してもアクセス許可を拡張しない |
| `network.localAccess.loopbackOrigins` | 非対応。要求元origin単位では判定しない |
| `network.localAccess.localNetworkOrigins` | 非対応。要求元origin単位では判定しない |
| メイン／サブフレーム依存の許可 | 非対応として扱う |
| Manifestの権限 | muonポリシーより外側にある、アプリ全体の上限制約として扱う |

`network.allow`に一致するURLは、どのページやフレームから要求されたかに関係なく許可対象になります。CEF版のように、信頼されたoriginから発生した場合だけ追加の宛先を許可することはできません。

設定で誤解を生まないよう、Androidで`network.authorizedOrigin`やorigin単位の`network.localAccess`が指定された場合は、無言で無視するより、ビルド時または起動時に「Androidでは非対応」と警告するのが適切です。

## フレーム判定についての正確な制約

「フレーム判定ができない」という表現は少し広すぎます。`WebResourceRequest.isForMainFrame()`により、最初の要求がメインフレーム文書用かどうかは取得できます。[WebResourceRequest API](https://developer.android.com/reference/android/webkit/WebResourceRequest)

取得できないのは次の情報です。

- 要求を発生させたフレームのorigin
- 個々のサブフレームの識別子
- サブフレームの親子関係
- WorkerやService Workerを含むrequest initiator

したがって、文書上は次の表現が正確です。

> Android版ではメインフレーム文書か否かを観測できる場合があるが、要求元フレームのoriginや識別情報を取得できないため、フレームまたはoriginに基づくネットワーク許可判定には使用しない。

## `network.allow`で可能になること

ネイティブHTTP層で次を実施できます。

- 最初のURLをglob判定
- URLスキームとポートの検査
- リダイレクトの自動追跡を無効化
- 各`Location`を解決して再度glob判定
- リダイレクト回数の制限
- DNS解決後のIPアドレス検査
- 応答サイズ制限
- 許可された応答だけを`WebResourceResponse`として返す

これにより、`network.allow`についてはWebViewの不完全なリダイレクト通知に依存せず、muon側で各ホップを検査できます。

## 全面プロキシにできない要求

公開されている`WebResourceRequest`にはURL、メソッド、ヘッダーはありますが、リクエストボディーがありません。このAPI構造から、POSTやPUTの内容をそのままネイティブHTTPへ転送することはできません。[WebResourceRequest API](https://developer.android.com/reference/android/webkit/WebResourceRequest)

したがって、少なくとも次の制約が残ります。

| 要求 | 扱い |
|---|---|
| GET／HEADのページ・画像・スクリプト等 | プロキシ可能 |
| GETのfetch／XHR | プロキシ可能。ただしCookie、CORS、キャッシュなどに互換性差が生じ得る |
| POST／PUT／PATCH／ファイルアップロード | ボディーを取得できないため、透過的なプロキシ不可 |
| HTMLフォームのPOST | 透過的なプロキシ不可 |
| WebSocket | `shouldInterceptRequest`に現れないため、プロキシ不可 |
| Service Worker | 別のプロセス共通コールバックが必要。初期仕様では無効化するのが安全 |
| `blob:` | URL自体はコールバックに現れない |
| HTTPリダイレクト | ネイティブ側で追跡・検査できるが、WebViewへ3xxを返すことはできない |

`WebResourceResponse`は3xxによるリダイレクトをサポートしていません。[WebResourceResponse API](https://developer.android.com/reference/android/webkit/WebResourceResponse)
そのため、ネイティブ側で最終応答まで取得して返すと、単純な実装ではWebViewの表示URL、履歴、最終ページのoriginが最初のURLのままになります。外部ページのログインフローなどでは大きな互換性問題になります。

## Manifestとの関係

有効なアクセスは、次の条件の積集合になります。

```text
プロキシが対応する要求
  ∩ network.allow
  ∩ Manifest権限
  ∩ Android実行時権限
  ∩ cleartext通信ポリシー
```

したがって、

- Manifestに`INTERNET`がなければ、`network.allow`で許可しても通信できない
- `INTERNET`があっても、`network.allow`に一致しなければmuonが拒否する
- Android 17でローカルネットワーク権限がなければ、ローカル宛てURLを許可してもOSが拒否する
- ローカルネットワーク権限があっても、`network.allow`に一致しなければmuonが拒否する

という関係になります。Androidのローカルネットワーク権限はネイティブHTTPクライアントを含むアプリ全体に作用します。[Android local network permission](https://developer.android.com/privacy-and-security/local-network-permission)

origin判定がないため、ローカルURLが`network.allow`に含まれ、Manifest権限も許可されている場合、そのURLは要求元ページを問わずアクセス可能になる点を明記する必要があります。

## 最終的な位置付け

この条件でのAndroid版は、次の仕様になります。

- `network.allow`は、代理可能なGET／HEAD中心のHTTP(S)要求に対して機能する
- originおよび要求元フレームによる許可拡張は機能しない
- Manifest権限はmuon設定より外側にあるOSレベルの上限制約
- 対応できない要求は直接通信へフォールバックせず拒否する
- 一般的な外部ページの表示は一部可能
- POST、WebSocket、認証リダイレクトなどを使う一般的なWebアプリとの互換性は保証しない

したがって、「単純な外部ページやGETリソースを許可するAndroid限定サブセット」としては成立します。一方、「既存の任意のWebアプリを変更せず、CEF版と同様に動かす」ことが条件なら、この方式でもWebViewの採用条件を満たしません。

----

リクエストボディを転送できないのが致命的と感じました。
視点を変えて、WebViewからのアクセスを全面的に認める（つまりWebViewの通常の使用方法におけるネットワークアクセスをmuonがインターセプトしない）と割り切った場合、CEF muonで実現できてい
るフィルタ機能の何を諦めることになりますか？

----

結論として、WebViewの通常ネットワークアクセスを全面的に認める場合、CEF版muonのネットワークポリシーはほぼすべて諦めることになります。その代わり、POSTボディー、Cookie、リダイレクト、WebSocket、Service WorkerなどはWebView本来の仕組みで動作します。

現在のCEF側の判定は[muon_network_policy.cpp](/home/kouji/Projects/muon-ui/muon-core/src/network/muon_network_policy.cpp)に集約されていますが、Android版ではこの判定層自体が存在しない形になります。

## 諦める機能

| CEF版の機能 | WebView全面許可時のAndroid版 |
|---|---|
| deny-by-default | 失われる。ManifestとOSが許すHTTP(S)通信は原則としてWebViewが実行する |
| `network.allow` | 機能しない。URL、パス、クエリーを含むglob制限は適用されない |
| `network.authorizedOrigin` | 機能しない。信頼したoriginだけに追加通信を許可する仕組みはない |
| メインフレームかどうかによる許可判定 | ネットワーク許可には使用しない |
| request initiatorによる許可判定 | 機能しない |
| リダイレクト先ごとの再検査 | 行わない。WebViewが通常どおり追跡する |
| `localAccess.loopbackOrigins` | 機能しない |
| `localAccess.localNetworkOrigins` | 機能しない |
| URLスキームごとのmuon allowlist | 機能しない。WebView設定とAndroidの処理に依存する |
| 拒否時の統一403応答 | 失われる |
| ブロック理由のmuon診断メッセージ | 失われる |
| 通信判断の一元的な監査 | 失われる |
| デスクトップとAndroidで同じ設定による保護 | 失われる |

特に重要なのは、CEF版では安全な次の設定が、Androidでは同じ意味を持たなくなることです。

```json
{
  "network": {
    "allow": ["https://api.example.com/**"]
  }
}
```

CEF版では指定先以外が拒否されますが、全面許可のAndroid版では、この設定に関係なく他のHTTP(S) URLにも通信できます。

## 代わりに得られる互換性

`blockNetworkLoads=false`にして通常のWebView通信へ戻すため、次の機能はWebView自身が処理します。[WebSettings API](https://developer.android.com/reference/android/webkit/WebSettings#setBlockNetworkLoads(boolean))

- 外部URLからのページロード
- GET、POST、PUTなどのリクエストボディー
- HTMLフォーム送信
- fetchとXHR
- Cookie、認証、キャッシュ
- HTTPリダイレクト
- WebSocket
- Service Worker
- ストリーミングやRange要求
- WebViewが対応するブラウザー標準のCORSやTLS検証

つまり、Web互換性を優先する代わりに、muon独自のネットワーク境界を取り除く選択になります。

## 引き続き残せる保護

ネットワークフィルタを廃止しても、次の保護は独立して維持できます。

- `WebViewAssetLoader`によるアプリ内アセット配信
- `file:`アクセス禁止
- `content:`アクセス禁止
- mixed content禁止
- Android Network Security Configによるcleartext通信制限
- WebViewのTLS証明書検証
- Safe Browsing
- ページ自身が設定するCSP
- ブラウザー標準のSame-Origin PolicyとCORS
- WebMessage RPCの`sourceOrigin`と`isMainFrame`検査
- muon capabilityによるネイティブAPI制限

RPCのorigin検査はネットワークフィルタとは別なので、現在の「信頼したアプリ内ページのメインフレームだけがネイティブAPIを呼べる」という制約は維持できます。

ただし、CORSはレスポンスをJavaScriptから読めるかを制限する仕組みであり、すべての通信送信を止めるallowlistではありません。画像、フォーム、ナビゲーションなどの通信に対する`network.allow`の代替にはなりません。

## Manifestの位置付け

この構成では、有効なネットワーク境界は次のようになります。

```text
Android Manifest・実行時権限
        ∩
Android Network Security Config
        ∩
WebView標準のセキュリティ
        ∩
ページ自身のCSP・CORS
```

ここにmuonの`network.allow`は含まれません。

- `INTERNET`がなければ外部通信全体ができない
- `INTERNET`があれば、WebViewは原則として任意のインターネット宛てへ通信できる
- Android 17で`ACCESS_LOCAL_NETWORK`が未許可ならLAN通信はOSが拒否する
- `ACCESS_LOCAL_NETWORK`が許可されると、muonによる要求元origin単位の制限はなく、残るのはWebViewとWeb標準側の制約だけ

ローカルネットワーク権限はWebViewを含むアプリ全体へ適用されます。[Android local network permission](https://developer.android.com/privacy-and-security/local-network-permission)

## 文書上の推奨表現

Android版の契約は、次のように明示するのが適切です。

> Android WebView版はWebViewの通常のネットワーク処理を使用し、muonによる通信のインターセプトを行わない。`network.allow`、`network.authorizedOrigin`、`network.localAccess.loopbackOrigins`、`network.localAccess.localNetworkOrigins`はAndroidでは適用されない。ネットワークアクセスはAndroid Manifest、実行時権限、Network Security Config、WebViewおよびページ自身のセキュリティポリシーに従う。

また、設定が無言で無効になると危険なので、Androidビルドでこれらの項目が指定された場合は「Androidでは適用されない」という警告を出すのが望ましいです。

この選択は、Web互換性を最大化できる一方、デスクトップ版とAndroid版でセキュリティ境界が明確に異なる設計になります。

----

- アプリ内アセット配信は asset: のようなスキームは使用できますか？
- CEF muonのように柔軟性が高くなくても良いので、特定のJSからのみmuonプラグインオブジェクトにアクセス可能にするような、muonプラグインオブジェクトへのアクセスを制限する方法はありますか？

----

結論は次のとおりです。

- `asset:`のような独自スキームは技術的には使用できますが、アプリ本体の文書originにはHTTPS形式を推奨します。
- muonプラグインへのアクセスは、originとメインフレーム単位なら制限できます。
- 同じページ内で実行される「特定のJSファイルだけ」を識別して許可することはできません。
- より強い制限が必要なら、プラグインブリッジと信頼済みコードをisolated worldへ配置できます。

## アプリ内アセットの独自スキーム

`shouldInterceptRequest`で`asset:`や`asset://...`を検出し、APK内のデータを`WebResourceResponse`として返すことは可能です。

ただし、CEFのように独自スキームを「標準かつsecureなスキーム」としてブラウザーへ登録するAPIはWebViewにありません。そのため、次の挙動をHTTPSと同等には保証できません。

- Same-Origin Policy
- CookieとWeb Storage
- fetchとXHR
- Service Worker
- secure context限定API
- 相対URLの解決
- CSPのorigin判定

Androidも、アプリ内コンテンツにはHTTP(S) URLと`WebViewAssetLoader`を推奨しています。[Load in-app content](https://developer.android.com/develop/ui/views/layout/webapps/load-local-content)、[WebViewAssetLoader API](https://developer.android.com/reference/androidx/webkit/WebViewAssetLoader)

したがって、内部表現は次のように分けるのが適切です。

```text
CEF:
asset://main/index.html

Android WebView:
https://appassets.androidplatform.net/assets/index.html
```

開発者向けAPI上で`asset://`を維持したい場合は、Androidバックエンド内部でHTTPS形式へ変換できます。WebView自身にはHTTPS URLだけを見せます。

独自スキームをプラグイン許可originに指定することもできますが、`addWebMessageListener`のカスタムスキーム規則はスキーム単位です。ホスト、ポート、パスを限定できません。例えば`asset://main`だけではなく、`asset://`全体が一致します。HTTPSならスキーム、ホスト、ポートを完全一致で指定できます。[WebViewCompat API](https://developer.android.com/reference/androidx/webkit/WebViewCompat)

## プラグインオブジェクトのorigin・フレーム制限

これは可能で、現在の試作でもすでに実施しています。

[MuonActivity.java](/home/kouji/Projects/muon-ui/muon-android-prototype/android/app/src/main/java/dev/muon/prototype/MuonActivity.java:90)では、ブリッジを次のoriginだけへ注入しています。

```text
https://appassets.androidplatform.net
```

さらに[MuonRpcBridge.java](/home/kouji/Projects/muon-ui/muon-android-prototype/android/app/src/main/java/dev/muon/prototype/MuonRpcBridge.java:61)で、受信時にも次を検証しています。

- `sourceOrigin`がアプリ内アセットoriginと一致
- `isMainFrame=true`

したがって、WebViewのネットワークアクセスを全面許可しても、次のページにはmuonプラグインオブジェクトが公開されません。

- 外部URLへ遷移したメインページ
- 外部originのiframe
- アプリ内originを使わないポップアップ
- 同じアプリ内originでもサブフレームからの呼び出し

`sourceOrigin`と`isMainFrame`はWebViewが通知する値なので、JavaScriptの自己申告には依存しません。[WebMessageListener API](https://developer.android.com/reference/androidx/webkit/WebViewCompat.WebMessageListener)

## 特定のJSファイルだけに制限できるか

同じページのJavaScript実行環境内ではできません。

例えば次の外部スクリプトは、取得元が外部URLでも、実行時には埋め込み先ページのoriginで動作します。

```html
<script src="https://cdn.example.com/library.js"></script>
```

アプリ内ページから読み込まれた場合、このスクリプトの実行originも`https://appassets.androidplatform.net`です。そのため、WebView側からはアプリ本体のJavaScriptと区別できず、両方ともプラグインオブジェクトへアクセスできます。

次の値を使った判定もセキュリティ境界にはできません。

- スクリプトファイルのURL
- `document.currentScript`
- JavaScriptスタックトレース
- 呼び出し元関数名
- JavaScriptが渡した署名やorigin文字列

したがって、通常のpage worldで実現できる制限単位は「特定のJS」ではなく「特定の文書originとフレーム」です。

## 現実的な推奨構成

通常は次の組み合わせが適切です。

1. アプリ本体を`https://appassets.androidplatform.net`から配信する
2. WebMessageブリッジをそのoriginだけへ注入する
3. ネイティブ側でも`sourceOrigin`と`isMainFrame`を再検査する
4. アプリ内アセットのCSPでは`script-src 'self'`を維持する
5. 外部ページと外部iframeにはブリッジを公開しない
6. プラグイン側では既存のcapability判定も行う

これにより、ネットワーク通信自体はWebViewへ任せながら、プラグインを利用できるのは「パッケージに同梱されたアプリ本体のメインフレーム」に限定できます。

ただし、アプリ内ページが外部JavaScriptを読み込む場合、その外部JavaScriptも同じ権限を持ちます。CSPやSubresource Integrityは、どのコードを実行させるかを制限するために使用します。

## isolated worldを使用する強い方式

現在使用している`androidx.webkit:webkit:1.17.0`には、page worldとは別のJavaScript execution worldへコードとWebMessageブリッジを注入するAPIがあります。

構成は次のようになります。

```text
page world
  └─ 通常のアプリ・外部JavaScript
       muonブリッジは見えない

muon isolated world
  ├─ muonが注入した信頼済みbootstrap
  └─ WebMessage RPC
```

この方式なら、page worldのJavaScriptから生のプラグインブリッジを参照できません。[JavaScriptExecutionWorld API](https://developer.android.com/reference/androidx/webkit/JavaScriptExecutionWorld)、[WebKit 1.16以降のisolated world](https://developer.android.com/jetpack/androidx/releases/webkit)

ただし、通常の開発者コードもpage worldで動くため、そのままではプラグインを呼べなくなります。page worldへ汎用的な呼び出し口を公開すると、結局すべての同一ページJavaScriptが利用できるため、分離の効果が失われます。

したがって、今回の要件にはまず次の境界が最も現実的です。

> muonプラグインは、HTTPS形式で配信されたアプリ内アセットoriginのメインフレームにだけ公開する。同一文書内で実行される個々のJavaScriptファイル間ではアクセス権を区別しない。

これはWebViewの通常ネットワークアクセスと両立し、現在の試作もすでにこのorigin・フレーム境界を採用しています。

----

"アプリ内アセットoriginのメインフレームにだけ公開する" でよさそうです。それを可能にするアセットのhttps url化ですが、CEF版と統一したいです。

- URLは任意のベースでも良いのですか？ 例えば `https://assets.muon.local/main/`
- 既にCEF版は公開しているので、CEF版では当面 `asset://`と両方を受け付けるようにして、androidでは初めからhttpsのみでハンドルします

----

はい、この構成は実現可能です。ただし、URLは技術的には変更可能でも、セキュリティ上は任意設定にせず、Muon専用の固定originにするべきです。

### ベースURL

Androidでは`setDomain()`と`addPathHandler()`を組み合わせられるため、次の形式は問題ありません。

```text
https://<専用ホスト>/main/index.html
```

`/main/`も任意の固定プレフィックスにできます。[WebViewAssetLoader.Builder](https://developer.android.com/reference/androidx/webkit/WebViewAssetLoader.Builder)

ただし、`https://assets.muon.local/main/`は推奨しません。`.local`はmDNS用の特別な名前空間なので、インターセプト漏れが発生した場合にローカルネットワークへ名前解決が流れる可能性があります。[RFC 6762](https://www.rfc-editor.org/rfc/rfc6762.html)

候補の優先順位は次のようになります。

1. Muonプロジェクトが恒久的に管理する実ドメイン配下
   - 例: `https://assets.<owned-domain>/main/`
2. 実ドメインを用意しない場合の候補
   - `https://assets.muon.invalid/main/`

`.invalid`は実在しない名前として予約されているため、誤って通常通信へフォールバックしても外部サイトには接続しません。[RFC 2606](https://www.rfc-editor.org/rfc/rfc2606.html)
ただしAndroid公式は、組織が所有するドメインか、既定の`appassets.androidplatform.net`を推奨しています。そのため`.invalid`採用時は、WebViewとCEFの実機テストを経てから確定するのが妥当です。[WebViewAssetLoader](https://developer.android.com/reference/androidx/webkit/WebViewAssetLoader)

### `/main/`はセキュリティ境界ではない

originは次の部分だけで決まり、パスは含まれません。

```text
https://assets.muon.invalid
```

したがって、以下は同一originです。

```text
https://assets.muon.invalid/main/index.html
https://assets.muon.invalid/other/page.html
```

`allowedOriginRules`もscheme、host、portだけを検査します。[WebViewCompat](https://developer.android.com/reference/androidx/webkit/WebViewCompat)

そのため設計上は、ホスト全体をアプリ内アセット専用にし、`/main/`以外や存在しないアセットについても通常ネットワークへフォールバックさせず、ローカルで404または403を返す必要があります。`/main/`はストレージ名前空間であって、プラグインアクセスの境界ではありません。

また、AndroidのWebMessageオブジェクトは一致するoriginのフレームに注入されます。現在の実装は[MuonRpcBridge.java](/home/kouji/Projects/muon-ui/muon-android-prototype/android/app/src/main/java/dev/muon/prototype/MuonRpcBridge.java:72)で`isMainFrame`を検査しているため、正確な保証は次の表現になります。

> アプリ内アセットoriginのメインフレームから送信されたRPCだけを受理する

同一originのiframeにオブジェクトが見えないことまでは保証せず、そこからのRPCを拒否します。

さらに、信頼されたメインフレーム内で実行されるJavaScriptは、ファイルの出所に関係なく同じ権限を持ちます。外部CDNのスクリプトにプラグインを使わせたくない場合は、通常ネットワークを許可しても`script-src 'self'`相当のCSPは維持する必要があります。

### CEFでの併用

CEFで次の両方を同じアセットストレージへ割り当てることは可能です。

```text
asset://main/<path>
https://<専用ホスト>/main/<path>
```

CEFはHTTP/HTTPSの組み込みschemeにも、ドメインを限定したscheme handlerを登録できます。[CEF Scheme Handler](https://chromiumembedded.github.io/cef/general_usage.html)

現在は[muon_app.cpp](/home/kouji/Projects/muon-ui/muon-core/src/app/muon_app.cpp:901)で`asset://main`だけを登録し、[muon_app_scheme.cpp](/home/kouji/Projects/muon-ui/muon-core/src/app/muon_app_scheme.cpp:177)も`asset`以外を拒否しています。実装時には以下が必要です。

- `asset://main/<path>`とHTTPSの`/main/<path>`を同じストレージキーへ変換する
- HTTPS専用ホストでは、未知のパスもhandler内で404にする
- CEFの既定`network.allow`へHTTPS URLを加える
- `plugin.pages`にも旧URLとHTTPS URLの両方を加える
- 明示指定されたallowlistは上書きせず、既存の設定意味を維持する

注意点として、両方で同じファイルを返してもWeb originは別です。そのためCookie、localStorage、IndexedDB、Cache Storageなどは共有されません。CEFの既定start pageをHTTPSへ切り替えると、既存アプリからは保存状態がリセットされたように見えます。

したがって移行順序としては、次が安全です。

1. CEFにHTTPSの別名を追加するが、既定start pageは当面`asset://main/index.html`のままにする
2. Androidは最初からHTTPSを使用する
3. originストレージ移行の扱いを決めた後、CEFの既定もHTTPSへ変更する
4. `asset://`は互換用エイリアスとして一定期間残す

なお、タイトルバーやトレイアイコンなどのネイティブ設定で使う`asset://main/...`は、ブラウザ文書URLとは別のリソース指定として残せます。今回統一すべき対象は、WebView/CEFにロードする文書originです。

----

では以下のようにします:

- android WebViewでは、muonのdeny allは技術的制約で担保されないこととします。但し、androidにはmanifestによる制約と、muonプラグインオブジェクトがアセット外originから参照できない事をdocに盛り込む。
- `https://main.asset.muon/` のように、アセット分岐をサブドメイン名として使用します
- ベースアセット名を構成可能にします。例えば、 `https://{asset_name}.asset.muon/` をデフォルトとして定義し、`{asset_name}` プレースホルダでアセット名を置換します。定義はmuon.jsonで行いますが、デフォルト定義で良ければ再定義は不要とします。
- CEF muonに限り、`asset://`を引き続き使用可能にしますが、docで移行を推奨します。デフォルト値で使用している`asset://`をプレフィックスとして持つ値は、`https://main.asset.muon/` で置き換えます。
- docには、このURLの引当に失敗した場合は、外部ネットワークにアクセスする可能性があることと、開発者が適切なベースホスト名を定義すべきであること、androidの場合は推奨名の掲示を行います

----

そのままでは完全には満たされません。方向性は妥当ですが、「アセットoriginの信頼」と「アセット取得失敗時の外部フォールバック」が衝突しています。以下を追加定義すれば、整理された仕様になります。

### 1. アセットoriginだけは必ずfail-closedにする必要がある

WebView全体の通信をMuonが制限しない方針と、Muonのアセットホストをローカルで必ず処理することは両立します。

```text
通常のHTTPS origin
    → WebViewの通常通信

構成されたアセットorigin
    → 必ずMuonが処理
    → アセットがなければローカル404
    → 外部ネットワークへフォールバックしない
```

`WebViewAssetLoader`はアセットが見つからない場合に`null`を返し、WebViewは通常ネットワークへフォールバックします。[Android公式ドキュメント](https://developer.android.com/develop/ui/views/layout/webapps/load-local-content)
CEFもHTTPS handlerが`nullptr`を返すと組み込みのネットワーク処理へフォールバックします。[CEF API](https://cef-builds.spotifycdn.com/docs/115.2/cef__scheme_8h.html)

外部から取得されたページでもURLが`https://main.asset.muon/`なら、WebViewから見れば信頼したアセットoriginです。そのページにもプラグインブリッジが注入されるため、単なる注意書きでは防げません。

したがって、ドキュメントにはフォールバックの一般的な危険性を記載してよいですが、Muonの仕様としては次が必要です。

- 構成されたアセットホスト全体をMuonが引き受ける
- 未登録パスや存在しないアセットは404
- handlerの初期化・登録に失敗した場合は起動を失敗させる
- アセットホストでは通常ネットワークへフォールバックしない

### 2. サブドメインによる分岐は適切だが、`.muon`は安全な既定値ではない

```text
https://main.asset.muon/
https://images.asset.muon/
```

という構成は、アセット名ごとにoriginを分離できるため適切です。ただし`.muon`は現在IANAルートに存在しませんが、予約されたTLDでもありません。将来の委任や企業内DNSとの衝突を排除できません。[IANA Root Zone Database](https://www.iana.org/domains/root/db)

安全側の既定値候補は次です。

```text
https://{asset_name}.asset.muon.invalid/
```

`.invalid`は実在しない名前として予約されています。[RFC 2606](https://www.rfc-editor.org/rfc/rfc2606.html)

一方、Android公式の推奨に最も従う本番設定は、開発者が所有するドメインです。

```text
https://{asset_name}.asset.myapp.example.com/
```

Android公式も、ローカルアセットには組織が所有するドメインか、既定の`appassets.androidplatform.net`を使うよう求めています。[WebViewAssetLoader](https://developer.android.com/reference/androidx/webkit/WebViewAssetLoader)

なお、サブドメイン方式を維持するなら、公式既定の`appassets.androidplatform.net`そのものとはURL構造が一致しません。Android向け推奨値は「アプリ開発者が所有する専用ドメイン」とするのが自然です。

### 3. URLテンプレートには検証規則が必要

テンプレートは少なくとも以下を満たす必要があります。

- schemeは`https`固定
- `{asset_name}`をホスト部分にちょうど1個含む
- userinfo、query、fragmentを禁止
- 末尾は`/`
- `asset_name`は単一DNSラベルに制限する
- ドット、スラッシュ、ポート記号、パーセントエンコードを許可しない
- 正規化後の完全一致originだけをプラグイン許可対象にする
- `https://*.asset.muon/`のようなワイルドカードでプラグインを公開しない

また、テンプレートを変更した場合は以下の省略時値も動的に生成する必要があります。

- `browser.startPage`
- CEFの`network.allow`
- `plugin.pages`
- AndroidのWebMessage許可origin

単に既定文字列を`https://main.asset.muon/`へ置換するだけでは、テンプレートを変更した際に設定が不整合になります。

### 4. CEFの`asset://`互換性には追加ルールが必要

現在のCEFでは、空の`network.allow`が`asset://`も拒否する設計です。そのため既定allowlistをHTTPSだけに置き換えると、handlerを残しても既存の`asset://main/...`設定が遮断される場合があります。

次のどちらかを決める必要があります。

- CEF移行期間中は、`network.allow`と`plugin.pages`にHTTPSと`asset://`の両方を既定登録する
- CEF内部で両URLを同じ論理アセットの別名として扱い、ポリシー評価時に元URLと変換後URLの両方を検査する

後者なら、公開上の既定値をHTTPSだけにしつつ、既存の`asset://`設定も維持できます。

### 5. Androidドキュメントの保証表現

「アセット外originから参照できない」だけでは少し強すぎるため、次のように記載するのが正確です。

> Android版ではMuonによる一般ネットワーク通信のdeny-by-defaultを提供しない。通信可否はAndroid Manifest、実行時権限、Network Security Config、WebViewおよびWeb標準に従う。MuonプラグインRPCは、構成されたアセットoriginのメインフレームから送信された場合だけ受理する。

さらに以下も必要です。

- 一致するoriginのサブフレームにはブリッジオブジェクトが見える可能性があるが、`isMainFrame`検査でRPCを拒否する
- 外部URLから取得したJavaScriptでも、アセット文書内で実行されればアセットoriginの権限を持つ
- アセットページが外部iframeからの要求を代理する場合、Muonはそれを区別できない
- パッケージ外スクリプトへプラグインを公開したくない場合は、CSPの`script-src`などを開発者が適切に設定する

`addWebMessageListener`は一致するoriginの各フレームへオブジェクトを注入し、コールバックの`isMainFrame`で送信元を判定できます。[WebViewCompat](https://developer.android.com/reference/androidx/webkit/WebViewCompat)

結論として、次の2点を修正すれば問題整理は満たされます。

- 一般ネットワークはWebViewへ任せるが、信頼するアセットホストだけは外部へフォールバックさせない
- CEFの`asset://`に対するポリシー互換性を別途定義する

また、現在の[filter-limitation.md](/home/kouji/Projects/muon-ui/filter-limitation.md:5)には、冒頭のdeny-by-default仕様と後半の全面許可案が併存しています。最終仕様化するときは、今回の決定に合わせて旧結論を置き換える必要があります。

----

分析はほぼOKです:
- "テンプレートを変更した場合は以下の省略時値も動的に生成する必要があります": これは開発者の責任とします（ベース名の変更に自動追従させる必要はなく、ベース名を変更した場合はこれらも適切に再設定するのは開発者の責任、但しdocに変更が必要なキーのガイド文書を記述する必要はある）
- デフォルトを `https://{asset_name}.asset.muon.invalid/` とします
  
----

了解しました。この2点を確定すると、仕様は整合します。

既定値は次のようになります。

```text
アセットURLテンプレート:
https://{asset_name}.asset.muon.invalid/

mainアセットorigin:
https://main.asset.muon.invalid

既定start page:
https://main.asset.muon.invalid/index.html
```

テンプレートを変更しても、他の省略時値は追従させません。開発者が少なくとも以下を合わせて再設定する責任を持ちます。

- `browser.startPage`
- `network.allow`（CEFのみ有効）
- `plugin.pages`
- アセットoriginを含む`browser.allowUnsafeJavaScriptParentAccess`
- アセットoriginを明示しているその他のorigin・URLパターン

ドキュメントには、変更手順、必要なキー、設定不整合時にはページ読み込みやプラグイン公開が失敗することを記載します。Muonが暗黙に設定を補正したり、テンプレートから他の値を再生成したりはしません。

CEFの`asset://`については、今回の方針から次の位置付けになります。

- handlerは引き続き`asset://`を処理する
- 新しい既定値はHTTPSだけを使用する
- `asset://`を継続利用する開発者は、`network.allow`や`plugin.pages`にも旧URLパターンを明示する
- ドキュメントではHTTPSへの移行を推奨する

つまり「既存の`muon.json`が無変更で動く」という互換性ではなく、「CEFでは明示設定すれば引き続き利用できる」という互換性です。

また、前述した安全条件は維持します。WebView全般の通信はdeny-allにしませんが、構成済みアセットホストへの要求だけはMuonが必ず処理し、不明なアセットはローカル404として、通常ネットワークへフォールバックさせない設計とします。外部アクセスの可能性に関する説明は、誤設定やプラットフォーム上の制約に対する注意事項として記載するのが適切です。

---

## Android WebView検証後の確定方針

### この追記の位置づけ

ステップ1ではCEF非依存RPCを抽出し、ステップ2ではAndroid WebViewから共通RPCホストへ接続する最小試作を実装した。ステップ3では、メインフレーム、iframe、`fetch`、XHR、WebSocket、リダイレクト、Service Worker、`blob:`、ローカルネットワークについて、WebViewでdeny-by-defaultを構成できるかを実機相当の環境で検証した。

検証の結果、複数のWebView APIとAndroid Manifestの制約を重ねれば外部通信を禁止できる一方、CEF版の要求元origin、要求元フレーム、リダイレクト先を使った選択的なネットワーク許可を、通常のWebアプリを変更せずに再現することはできないと判断した。また、ネイティブHTTP層で全面的に代理取得する方式には、任意のWebViewリクエストのボディーを取得できない、WebSocketやService Workerを透過的に代理できない、リダイレクト後の表示URLとoriginを維持できない、という互換性上の問題がある。

このため、上記「推奨する進め方」のステップ3に記載したdeny-by-defaultは採否を判断するための検証条件として完了したものとし、Android製品仕様には採用しない。この追記は、先行するネットワーク制御案と矛盾する箇所について、検証後の最終判断として優先する。

詳細な検証結果と制約は[filter-limitation.md](/home/kouji/Projects/muon-ui/filter-limitation.md)に記録している。実装時には、同文書に残っているdeny-by-default案、ネイティブHTTP代理案、通常通信採用案を、今回確定した仕様が明瞭になるよう整理する。

### Android版のネットワーク契約

Android版では、WebViewが行う通常のネットワーク通信をMuonが包括的にインターセプトしない。POSTボディー、Cookie、HTTPリダイレクト、WebSocket、Service WorkerなどはWebView本来の処理へ委ねる。

この判断により、CEF版が提供する次のネットワークポリシーと同等の保証はAndroid版では提供しない。

- `network.allow`による宛先URLのdeny-by-default
- `network.authorizedOrigin`による要求元origin単位の許可
- `network.localAccess.loopbackOrigins`によるloopback通信の要求元制限
- `network.localAccess.localNetworkOrigins`によるLAN通信の要求元制限
- Muonによる各リダイレクト先の再検査
- MuonによるWebSocket、Service Worker、`blob:`などを含む全通信経路の捕捉

Android版の通信可否は、次の外側の制約に従う。

- アプリ開発者が定義するAndroid Manifestの`INTERNET`
- 対象Androidバージョンで必要となる`ACCESS_LOCAL_NETWORK`と実行時権限
- Network Security Configと`usesCleartextTraffic`
- WebViewによるTLS検証、CORS、mixed contentなどのWeb標準
- ページ自身が定義するContent Security Policy

Manifestに必要な権限がなければWebViewは通信できない。権限があれば、アセットoriginを除く通信にMuon独自のallowlistは適用されない。Manifestと実行時権限はアプリ全体を制約する大きな境界であり、CEF版のような要求元origin単位の制約ではないことを利用者向け文書へ記載する。

### アセットoriginだけに適用するfail-closed規則

通常のネットワーク通信をWebViewへ委ねても、Muonプラグインを信頼して公開するアセットoriginだけは通常ネットワークへフォールバックさせない。originはコンテンツの取得元を証明せず、同じHTTPS URLを外部サーバーから取得した場合もWebView上では同一originになるためである。

構成済みのアセットホストに対しては次を必須とする。

- ホスト全体をMuonのアセットハンドラーが引き受ける。
- APK、パッケージ、またはMuonアセットストレージに存在する内容だけを返す。
- 未登録パス、存在しないアセット、許可しないHTTPメソッドにはローカルのエラー応答を返す。
- アセットが見つからない場合もハンドラーから`null`または`nullptr`を返して通常ネットワークへ委譲しない。
- アセットハンドラーの初期化または登録に失敗した場合は、安全性を弱めて続行せず起動を失敗させる。

これはWebView全体のdeny-by-defaultではなく、プラグイン公開originの真正性を維持するための限定的なfail-closed規則である。

### Muonプラグインの公開境界

Android版のMuonプラグインRPCは、構成されたアプリ内アセットoriginのメインフレームから送信された場合だけ受理する。

- WebMessageの許可規則には解決済みの完全一致originを列挙し、ホストのワイルドカードを使用しない。
- コールバックで`sourceOrigin`が期待するoriginと一致することを再検査する。
- コールバックで`isMainFrame`が真であることを検査する。
- 外部originのメインフレーム、iframe、ポップアップからの直接RPCを拒否する。
- 同一アセットoriginのサブフレームにはWebMessageオブジェクトが見える可能性があるが、そこから送信されたRPCを`isMainFrame`で拒否する。

この境界は個々のJavaScriptファイルを識別するものではない。外部URLから取得したスクリプトでも、信頼されたアセット文書内で実行されれば、その文書のoriginを持ち、Muonプラグインへアクセスできる。また、信頼されたメインフレーム自身が外部iframeの要求を代理した場合、ネイティブ側はそれをメインフレーム自身のRPCと区別できない。

したがって、パッケージ外のJavaScriptへプラグインを公開したくないアプリでは、開発者が`script-src 'self'`などのCSPを設定し、信頼されたページ自身にRPCを無条件で中継する実装を置かない必要がある。文書では「特定のJavaScriptだけに公開する」ではなく、「構成されたアセットoriginのメインフレームからのRPCだけを受理する」と表現する。

### HTTPSアセットURL

WebViewとCEFで共通に扱う正規アセットURLはHTTPS形式とし、アセット分岐をパスではなくサブドメインで表現する。

既定のURLテンプレートは次とする。

```text
https://{asset_name}.asset.muon.invalid/
```

既定の`main`アセットは次のURLへ解決される。

```text
origin:     https://main.asset.muon.invalid
start page: https://main.asset.muon.invalid/index.html
URL glob:   https://main.asset.muon.invalid/**
```

サブドメイン方式では、`main`と別名のアセットは異なるoriginになる。localStorage、IndexedDB、Cookie、Cache Storage、Service Worker、同一originアクセスなどもアセット名ごとに分離される。異なるアセットorigin間で通信する場合は、通常のCORSとSame-Origin Policyに従う。

`asset_name`はホスト名の単一ラベルとして扱う。実装時には、少なくとも次の検証を行う。

- 空文字列を許可しない。
- ASCII英小文字、数字、ハイフンだけを許可する。
- 先頭と末尾のハイフンを許可しない。
- ドット、スラッシュ、コロン、パーセントエンコードを許可しない。
- DNSラベルの長さ上限を超える値を許可しない。
- 比較と置換の前に大文字小文字の扱いを正規化する。

`.invalid`は外部DNS上で実在するサイトと衝突しない既定値として使用する。一方、Android向けの利用者文書では、製品アプリが所有し続ける専用ドメイン配下を使用する構成も推奨する。

```text
https://{asset_name}.asset.myapp.example.com/
```

実在するドメインを使用する場合は、アセットハンドラーが要求を処理できなかったときに外部ネットワークへ到達する危険が特に大きい。アセット専用ホストを通常のWebサイトと共用しないこと、ドメインの所有権を維持すること、Muon側のfail-closed規則を無効化しないことを文書へ記載する。

### `muon.json`によるURLテンプレートの構成

URLテンプレートは`muon.json`のアセット設定で変更可能にする。設定キーの最終的な名前とJSON構造は、実装前に既存の`asset`設定との整合性を確認して確定する。

テンプレート値は次の条件を満たさなければならない。

- schemeは`https`である。
- `{asset_name}`プレースホルダーをホスト部分にちょうど1個含む。
- userinfo、query、fragmentを含まない。
- アセットパスをルートへ連結できるよう、URLは`/`で終わる。
- プレースホルダー置換後のURLが有効なHTTPS URLとoriginになる。

URLテンプレートを変更しても、他の省略時値を新しいテンプレートへ自動追従させない。関連設定を一括して変更する責任はアプリ開発者が負う。設定の不整合をMuonが暗黙に補正したり、明示値を書き換えたりしない。

テンプレートを変更する場合のガイドには、少なくとも次の設定の確認と変更が必要であることを記載する。

- `browser.startPage`
- CEF版の`network.allow`
- `plugin.pages`
- `browser.allowUnsafeJavaScriptParentAccess`にアセットURLを指定している場合はそのパターン
- `network.authorizedOrigin`や`network.localAccess`など、アセットoriginを明示しているその他の設定
- アプリソース、CSP、テストコードなどに絶対アセットURLを記述している場合はそのURL

既定テンプレートを使用する場合は追加設定を不要とし、従来`asset://`をプレフィックスとしていた省略時値を次のHTTPS値へ変更する。

- `browser.startPage`は`https://main.asset.muon.invalid/index.html`
- `plugin.pages`は`https://main.asset.muon.invalid/**`
- CEF版の既定`network.allow`におけるアセット許可は`https://main.asset.muon.invalid/**`
- `data:image/**`など、アセットURLと無関係な既定値は維持する。

### CEF版の`asset://`移行互換性

CEF版は移行期間中に限り、次の両方を同じMuonアセットストレージへ割り当てる。

```text
https://main.asset.muon.invalid/index.html
asset://main/index.html
```

HTTPSを正規形式とし、新しい省略時値と利用者向け例はHTTPSを使用する。`asset://`はCEF版だけの非推奨な互換形式として文書化し、Android版では受け付けない。

この互換性は、CEFが`asset://`を解釈してアセットを返せることを意味する。HTTPSへ変更した新しい`network.allow`や`plugin.pages`へ、旧URLを暗黙に別名展開することまでは行わない。`asset://`を継続利用する開発者は、`browser.startPage`だけでなく、必要な`network.allow`と`plugin.pages`にも旧URLパターンを明示する。

HTTPSと`asset://`は同じバイト列を返しても異なるWeb originである。Cookie、localStorage、IndexedDB、Cache Storage、Service Workerなどの状態は共有されない。CEFアプリのstart pageをHTTPSへ移行すると、originストレージが新規状態に見えることを移行ガイドへ記載する。

CEFのHTTPSハンドラーは構成済みアセットホスト全体を処理し、不明なパスにもローカル404を返す。組み込みHTTPSハンドラーへ処理を戻さない。既存の`asset://`ハンドラーも同様に、許可するメソッド、ホスト、アセット名、パスを検証する。

### 文書化する最終的な制約

利用者向け文書は少なくとも次を明示する。

- Android版はWebViewの通常ネットワーク処理を使用し、MuonのCEF版ネットワークポリシーを再現しない。
- Android Manifest、実行時権限、Network Security ConfigがMuonより外側の制約として適用される。
- `INTERNET`がなければ外部通信できず、付与されていればMuonの宛先allowlistなしでWebViewが通信できる。
- Androidのローカルネットワーク権限はアプリ全体へ適用され、要求元origin単位の許可にはならない。
- MuonプラグインRPCは、構成されたアセットoriginのメインフレームからだけ受理される。
- 同じ信頼済み文書内で実行されるJavaScriptを、ファイルの取得元ごとに区別できない。
- 既定HTTPSアセットURLと、開発者所有ドメインを使うAndroid向け構成例を掲載する。
- URLテンプレート変更時に連動して変更すべき`muon.json`キーを一覧化する。
- 構成済みアセットoriginはMuonがfail-closedで処理するが、誤ったホスト設定、専用でない実在ホストの使用、またはプラットフォーム上の想定外の処理によって外部ネットワークへアクセスする危険がある。
- CEF版の`asset://`は移行用であり、新規アプリにはHTTPSを推奨する。
- CEFでHTTPSへ移行するとWeb originストレージが旧`asset://`と共有されない。

公開文書は日本語版と英語版で同じ契約を示す。`filter-limitation.md`は検証記録として残す場合も、相互に矛盾する案を最終仕様のように並べず、採用しなかった案と確定仕様を明確に分離する。

### 今後の実装手順

1. 共通のアセットURLモデルを定義する。
   `muon.json`のURLテンプレート設定、テンプレート検証、`asset_name`検証、HTTPS URLとCEF用`asset://` URLの解析結果を、CEFやAndroidへ依存しない値として扱えるようにする。テストを先に追加し、既定値、正常な置換、不正なテンプレート、不正なアセット名、テンプレート変更時に他の設定へ自動追従しないことを検証する。

2. CEFへHTTPSアセットハンドラーを追加する。
   HTTPSの構成済みホストを既存ストレージへ割り当て、GETとHEAD、MIME型、エラー応答を既存`asset://`と揃える。存在しないアセットが外部ネットワークへ到達しないテストを追加する。`asset://`ハンドラーは残し、両URLが同じアセット内容を返すことを検証する。

3. CEFの既定値とポリシーをHTTPSへ移行する。
   `browser.startPage`、`plugin.pages`、`network.allow`のアセット既定値を`https://main.asset.muon.invalid/`へ変更する。明示設定を暗黙に書き換えないこと、明示的に許可したCEFの`asset://`が引き続き動作することを検証する。

4. Android WebViewを通常ネットワーク動作へ変更する。
   deny-all用の包括的フィルター、`blockNetworkLoads=true`、Service Workerの全面遮断を最終仕様から外す。構成済みアセットホストだけをローカル配信し、未登録アセットをローカル404にする。Manifestで許可された環境では通常のGET、POST、リダイレクト、WebSocket、Service WorkerがWebView本来の経路で動作することを計装テストで確認する。

5. AndroidのRPC公開originを新しいHTTPS originへ変更する。
   完全一致originとメインフレーム検査を維持し、外部originのメインフレーム、外部iframe、同一originサブフレームからRPCを実行できないことを計装テストで確認する。アセットメインフレームからの文字列、バイナリ、エラー、キャンセル、Activity再生成も引き続き動作することを確認する。

6. 制約と移行方法を文書化する。
   Androidのネットワーク契約、Manifestと実行時権限、プラグイン公開境界、アセットURLテンプレート、変更が必要な設定キー、CEFの`asset://`移行、originストレージの非共有を利用者向け文書へ反映する。

7. 全体テストと手動確認を行う。
   個別テストだけで完了とせず、リポジトリ全体のテストを実行する。Android計装テストは実サーバーへの到達有無も使って通常通信とアセットoriginのfail-closedを区別する。最後にCEF版とAndroid版の両方で既定start page、プラグインRPC、通常ネットワーク、存在しないアセット、CEFの旧`asset://`を手動確認する。

### 完了条件

この方針の実装は、次をすべて満たした場合に完了とする。

- 共通の既定アセットURLが`https://main.asset.muon.invalid/`になっている。
- 有効な`muon.json`設定でアセットURLテンプレートと`asset_name`を構成でき、不正値を拒否できる。
- テンプレート変更時に関連する明示値や省略時値を暗黙に変更せず、その責任と必要なキーを文書化している。
- Androidでは通常ネットワークをMuonが包括的に遮断または選択許可せず、WebViewとAndroid権限へ委ねている。
- AndroidとCEFの構成済みアセットホストは、存在しないアセットを外部ネットワークへフォールバックさせない。
- Androidでは構成済みアセットoriginのメインフレームだけがMuonプラグインRPCを利用できる。
- CEFではHTTPSアセットURLが既定で動作し、明示設定された`asset://`も移行期間中は動作する。
- HTTPSと`asset://`のoriginストレージが共有されないことを移行文書に記載している。
- Androidで再現しないCEFネットワーク機能、Manifestによる制約、CSPを含む開発者責任を日本語・英語の利用者向け文書に記載している。
- 新規テストを含むリポジトリ全体のテストが成功し、CEF版とAndroid版の手動確認結果を記録している。

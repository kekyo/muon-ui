# CEF版のHTTP・WebSocketサーバーとローカルMCP連携

## 目的と採用方針

CEF版のmuonアプリからHTTP・WebSocketサーバーを公開し、アプリ内のブラウザーと、同じPCで動作する外部プログラムから利用できるようにする。主な用途は実行中のmuonアプリをCodexやClaude CodeからMCPで操作することである。MCP以外のHTTP APIやWebSocket通信にも使える汎用機能として提供する。Androidは対象に含めない。

当初は議論で案2として選択したCEFの`CefServer`を使い、段階2までを実装した。段階3の調査で、送信の欠落や未送信データの破棄を公開APIから防げないと分かったため、通信部分をcardioと非ブロッキングTCPへ差し替える。理由と移行手順は「送信制約と通信部分の差替え」に記載する。muonの内蔵機能として受信・応答・接続管理を公開し、MCPのメソッドやアプリ固有の操作はその上のJavaScript側で実装する。CEF版だけを対象とし、Node.jsを必須依存にはしない。

初版のサーバーは作成元ページの実行コンテキストに生存期間を合わせる。ただし、ネイティブのサーバー、ページのハンドラー登録、処理中の要求・接続を最初から分離する。将来アプリ全体へ生存期間を広げる際に、通信と認証の実装を再利用できる構成にする。

公開APIはサーバーの作成、ハンドラー登録、待受開始を分ける案2を基本とし、HTTPハンドラーには案1のFetch形式を組み合わせる。APIの案番号は前段のサーバー提供方式の案番号とは別である。一括起動の`serve()`は便利関数の候補として残し、IDとイベントを直接扱う案3は主に内部の通信に使う。

標準設定はloopbackだけでの待受、HostとOriginの検証、任意のBearer認証を組み合わせる。Bearerを設定しない場合は、同じPCのローカルプロセスを信頼する。設定した場合は、認証対象の要求でトークンを必須にする。不許可のHost／Originを拒否する処理はBearerの有無にかかわらず有効にする。

作成日と今回の更新日は2026年10月21日、muon-uiの基準コミットは`8eb1c1f899a4b88e9b4369f2f9c111b0c4e67cb3`。参照CEFは`147.0.14+g76d2442+chromium-147.0.7727.138`である。本書に議論と調査結果、実施記録を保存する。機能は実装・検証中である。初版の生存期間とAPIの基本形式は採用方針として記録し、未確定事項と実装時に決めた事項を区別する。

## 用語と利用形態

MCPは、LLMクライアントがアプリの操作や情報を利用するためのプロトコルである。Streamable HTTPはMCPのHTTP通信方式で、HTTPの応答にJSONやServer-Sent Events（SSE）を使う。SSEはサーバーからクライアントへイベントを順次送るHTTP応答である。WebSocketは別の双方向通信方式であり、一般のMCPクライアントが共通して対応する通信方式とはみなさない。

loopbackは同じネットワーク環境内の自分自身へ接続するためのアドレスで、IPv4の`127.0.0.1`とIPv6の`::1`が該当する。bindはサーバーのソケットが待ち受けるアドレスとポートを指定する操作である。Bearer認証では、クライアントが`Authorization: Bearer <token>`を送信し、サーバーがトークンを検証する。

| 利用形態 | 想定する使い方 | 接続に必要な性質 |
| --- | --- | --- |
| CEFブラウザー内からだけ利用 | ブラウザー対応のNPMパッケージをMCPクライアントとして使い、自分のアプリへ接続する。 | 外部プロセスがTCP接続できても、アプリ用の資格情報がなければ操作できないようにする。 |
| CEFブラウザー外からも利用 | 同じPCのCodexやClaude Codeから、実行中のmuonアプリを操作する。 | 通常のHTTP URLを登録して接続できるようにする。標準ではトークンの設定を要求しない。 |
| 内外の両方から利用 | アプリ内クライアントと外部クライアントが同じ機能を使う。 | 同じ要求処理を共有し、ブラウザー向けのOriginとCORSの設定を追加する。 |

CEF外からの利用と、別のPCからの利用は区別する。本計画の中心は同じPC内の通信である。WSL、コンテナー、リモート開発環境では、同じPCでもloopbackの接続先が異なることがある。クラウド上のクライアントに指定した`localhost`も、ユーザーのPCを指すとは限らない。

## 提供方式の比較

| 方式 | 利点 | 制約と判断 |
| --- | --- | --- |
| 既存のNode sidecarでサーバーを実装 | ExpressやNode向けMCP SDKなどを使える。現在のコードにもHTTPサーバーの実行例がある。 | Node runtimeとプロジェクトの配布が必要。ページとsidecarの寿命、アプリ操作の受け渡しも扱う必要がある。既存の選択肢として残す。 |
| 案2：`CefServer`を内蔵機能として公開 | CEFに含まれるHTTP・WebSocket機能を使い、Nodeを追加せずにアプリへ受信機能を提供できる。 | 当初採用し、段階2まで実装した。送信制約が用途を満たさないため、段階3で通信部分を差し替える。 |
| 独自のネイティブサーバーや別プロセス | 必要なHTTP・WebSocket仕様やTLSを選びやすい。 | 依存関係、配布、終了処理、プラットフォームごとの検証が増える。代替実装として、同じbrowser process内でcardioと非ブロッキングTCPを使う方式を採用する。別プロセスやTLSは追加しない。 |
| CEF内のリクエストを処理する仮想HTTPサーバー | TCPポートを開かずに、ブラウザーからのHTTP要求を処理できる。 | 外部クライアントから直接接続できない。WebSocketはHTTPリソース処理だけでは同じ形で提供できない。内部専用の別案として比較した。 |

Node sidecarの実例は[Expressのfixture](../../../core/cef-test/fixtures/node-express-project/server.mjs)と[そのE2Eテスト](../../../core/cef-test/plugin-e2e/node-plugin-express.ts)にある。fixtureは`127.0.0.1`の動的ポートで待ち受ける。この例のCORS設定を、そのまま新しい内蔵サーバーの既定値にはしない。

### CEF内だけに公開する二つの方法

| 比較項目 | loopbackと認証を使う方法 | TCPポートを開かない方法 |
| --- | --- | --- |
| 外部プロセスからのTCP接続 | 接続は可能。認証で利用を拒否する。 | サーバーのTCPポートが存在しない。 |
| HTTPクライアントとの互換性 | 通常のHTTP URLを使いやすい。 | `CefResourceHandler`などで要求を処理する。URLのscheme、Origin、リソース処理の条件を合わせる必要がある。 |
| WebSocket | 実際の待受を使える。通信部分を差し替え、textとbinaryに対応する。 | 通常のWebSocketをリソースハンドラーだけで置き換えることはできず、別の通信アダプターが必要になる。 |
| 外部公開への切替 | 同じ受信機能を使い、認証と公開設定を変更できる。 | 外部向けに別の実サーバーが必要になる。 |
| 運用上の負担 | ポートの競合、資格情報の配布、Host／Origin検証を扱う。 | ポートの競合はないが、内部専用経路と外部経路を別に検証する。 |

議論では外部プロセスからTCP接続されること自体を禁止せず、認証で利用を制約する前者を選択した。その後、最も多い用途を同じPCの外部CLIからの操作と整理し、標準設定ではBearerを任意にする方針になった。

この変更によって、認証なしの標準設定を「CEF内だけに公開」と呼ぶことはできない。内部専用の利用では外部へ配布しない資格情報を使う。HTTPではアプリが生成・保持するBearerをサーバー作成時に設定し、クライアントのfetchへAuthorizationヘッダーを指定する。ブラウザーのWebSocketでは同じ方法を使えないため、接続方法を段階4で定める。同じOSユーザーの任意コードからも秘密を守れる、という保証はしない。

仮想HTTP方式の参照APIは[CEFのResourceHandler](https://cef-builds.spotifycdn.com/docs/147.0/classCefResourceHandler.html)と[SchemeHandlerFactory](https://cef-builds.spotifycdn.com/docs/147.0/classCefSchemeHandlerFactory.html)である。この方式を今回の実装範囲には含めない。

## 現在のコードと拡張箇所

| 対象 | 現在の構成 | 計画上の扱い |
| --- | --- | --- |
| [内蔵プラグイン登録](../../../core/cef/src/plugins/builtin/muon_builtin.cpp) | fs、environments、executor、launcherを登録する。 | サーバー操作を内蔵機能として追加する。CEFに依存するサービスはbrowser process側へ置く。 |
| [デスクトップのプラグイン実行処理](../../../core/cef/src/plugins/muon_plugin_runtime_desktop.cpp) | プラグインの初期化・終了と、ページの実行コンテキスト解放を扱う。 | サーバーと購読の所有元、終了処理を接続する。 |
| [CEF側のAPI公開](../../../core/cef/src/app/muon_app.cpp)、[公開型](../../../ui/muon.d.ts)、[仮想モジュール型](../../../ui/muon-virtual-modules.d.ts)、[capability処理](../../../ui/common/capability.ts) | simpleとvalidateの両モードで関数を公開する。 | 関数の許可設定、型、引数・戻り値の変換を両モードへ反映する。 |
| [共通プラグインランタイム](../../../core/common/src/plugins/muon_plugin_runtime.cpp)と[SDKの関数参照API](../../../sdk/include/muon_plugin_api.h) | ネイティブ側でJavaScript関数参照を保持できる。実行コンテキストの破棄時には無効化する。 | 通常の呼出し後も続く要求・イベント配信に利用できるか検証する。所有元を失ったコールバックは実行しない。 |
| [通知のRPC連携](../../../core/cef/src/notifications/muon_notification_rpc.cpp) | browser、frame、contextに対応する所有元を扱う。 | サーバーの所有元確認とイベント配信を設計する際の先例にする。 |
| [ネットワークポリシー](../../../core/cef/src/network/muon_network_policy.cpp)と[権限処理](../../../core/cef/src/browser/muon_client.cpp) | `network.allow`と`network.localAccess`でCEFからの外向き通信を制御する。 | 新しい受信側ポリシーと区別する。既存設定だけでサーバーを保護できるとは扱わない。 |
| [asset scheme](../../../core/cef/src/app/muon_app_scheme.h) | アプリのリソースに標準・secure・CORS・fetch対応のschemeを登録する。 | アプリのOriginとloopback接続を検証する。CORSの許可は受信側でも設定する。 |

Node sidecarへ渡したコールバックの寿命と、ネイティブSDKの関数参照の寿命は同一ではない。ネイティブSDKには`retain_plugin_function_pointer`があるため、「呼出し完了後にJavaScriptの関数を保持できない」という制約を、今回の内蔵機能へそのまま適用しない。ただし、保持した参照も元の実行コンテキストを越えて使えるわけではない。

## CEFの機能と制約

公式の[CefServer API](https://cef-builds.spotifycdn.com/docs/147.0/classCefServer.html)、[CefServerHandler API](https://cef-builds.spotifycdn.com/docs/147.0/classCefServerHandler.html)、参照バージョンの[ヘッダーとAPIコメント](https://github.com/chromiumembedded/cef/blob/76d2442/include/cef_server.h)を確認した。

| 項目 | 確認した内容 | 実装時の扱い |
| --- | --- | --- |
| 対象用途 | 少数の同時接続、localhostでのアプリ間通信を想定している。 | 多数の利用者向けのWebサーバーとしての性能は保証しない。 |
| 実行場所 | browser processから利用する。サーバーごとにCEFが専用スレッドを作る。 | rendererのJavaScriptを専用スレッドから直接呼ばず、既存の非同期配送へ接続する。muon独自の待機用ワーカースレッドやポーリングは追加しない。 |
| 待受 | `CreateServer`に数値のIPv4またはIPv6アドレスを渡す。 | `localhost`の文字列をそのまま渡さず、明示したloopbackアドレスへbindする。IPv4とIPv6を同時に扱う構成は実測する。 |
| ポート | 公開コメントは予約範囲外のポートを指定する説明になっている。 | Nodeの例からポート`0`の対応を推定しない。動的ポートの可否と取得方法を確認してから公開APIへ含める。 |
| HTTP | URL、メソッド、ヘッダー、POSTデータを取得し、非同期に応答できる。 | JSONとバイナリーの要求・応答、エラー、接続終了を提供する。 |
| SSE | `SendHttpResponse`で長さを未指定にし、`SendRawData`で順次送信できる。 | 実際のMCPクライアントで逐次受信を検証する。書込み完了や送信バッファー解放を通知する公開APIはないため、送信Promiseの意味を明示する。 |
| WebSocket | 接続の許可・拒否、接続後のメッセージ送受信、切断がある。 | 公開APIにtext／binaryの種別、subprotocol選択、close codeを指定する引数がない。必要な互換性を先に確認し、対応していない操作を成功扱いにしない。 |
| TLS | 公開APIに証明書やTLSの設定がない。 | 初期対象をloopbackのHTTP／WSとする。HTTPS／WSSを内蔵機能として提供するとは約束しない。 |
| 終了 | `Shutdown`の後に`OnServerDestroyed`が呼ばれる。切断後のconnection IDは使用できない。 | 停止要求と停止完了を分け、遅れた応答や二重終了を処理する。 |

WebSocketやSSEの要求を満たせない場合は、公開範囲と代替実装をこの計画へ反映してから進める。外部CEFやベンダーコードの変更、Chromium内部APIの呼出しで不足を補う方針は採らない。

### 送信制約と通信部分の差替え

段階3の調査で、参照バージョンには次の制約があることを確認した。

- Chromiumの送信キューは接続ごとに既定1 MiBで、上限を超える追加は失敗する。`SendRaw()`はその失敗を呼出元へ返さず、CEFの公開APIにはキュー残量や送信完了の通知もない。CEFは上限を変更していない。[送信キュー](https://github.com/chromium/chromium/blob/147.0.7727.138/net/server/http_connection.h#L69-L105)、[追加処理](https://github.com/chromium/chromium/blob/147.0.7727.138/net/server/http_connection.cc#L106-L122)、[SendRaw](https://github.com/chromium/chromium/blob/147.0.7727.138/net/server/http_server.cc#L86-L96)、[CEFの送信処理](https://github.com/chromiumembedded/cef/blob/76d2442/libcef/browser/server_impl.cc#L393-L443)
- `CloseConnection()`は未送信のキューを排出せず、接続を管理対象から除外して破棄する。送信APIを呼んだ直後に閉じると、通常の有限HTTP応答でも末尾が失われる可能性がある。[CEFの切断](https://github.com/chromiumembedded/cef/blob/76d2442/libcef/browser/server_impl.cc#L309-L319)、[Chromiumの切断](https://github.com/chromium/chromium/blob/147.0.7727.138/net/server/http_server.cc#L130-L145)、[書込み完了処理](https://github.com/chromium/chromium/blob/147.0.7727.138/net/server/http_server.cc#L320-L338)
- WebSocketの送信はtext固定で、binaryの受信はエラーになる。[送信種別](https://github.com/chromium/chromium/blob/147.0.7727.138/net/server/http_server.cc#L75-L85)、[受信処理](https://github.com/chromium/chromium/blob/147.0.7727.138/net/server/web_socket_encoder.cc#L69-L83)

送信を小さく分割するだけでは、読み取りが遅いクライアントに対するキューの増加を防げない。固定時間の待機や独自ACKを既存クライアントへ要求する方式も採らない。サーバー、ハンドラー登録、要求の管理を分けた設計を維持し、通信部分を差し替える。

HTTPの解析には、分割入力とUpgrade後の未消費データを扱える[llhttp](https://llhttp.org/)を使う。公式配布のCコードを変更せずに組み込み、厳密な解析を維持する。ヘッダーと本文の容量制限、重複ヘッダーの検証、応答生成、接続の生存期間はmuonで管理する。[公開APIのコメント](https://github.com/nodejs/llhttp/blob/main/src/native/api.h)

ソケットの読書きは非ブロッキングで行い、読み書きできる状態になるまでcardioで待つ。部分書込みの残りを保持して送信を直列化し、待ち行列を無制限に増やさない。送信Promiseは対象データをOSが受け付けた時点で完了し、相手の受信・処理完了は保証しない。通常の応答終了は送信処理を完了してから閉じる。ページ破棄や明示停止による打切りは別に扱い、保留中の待機とRPCも完了させる。受信側のEOFだけで応答の送信まで中断しない。[Winsockのsend](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-send)、[正常終了と打切り](https://learn.microsoft.com/en-us/windows/win32/winsock/graceful-shutdown-linger-options-and-socket-closure-2)

Linuxは`cardio::from_fd()`を使う。Windowsは`WSAEventSelect`と`cardio::from_win32_handle()`を使い、イベント取得・解除を`WSAEnumNetworkEvents()`で行う。現在のcardioには通常62個のHANDLE待機上限があるため、ソケットごとに待機枠を増やさず、共有イベントへの集約と取消の競合を検証する。独自のワーカースレッド、定期ポーリング、待機中のfd／HANDLEの再利用は導入しない。[cardioの待機API](../../../deps/cardio/README.md)、[WSAEventSelect](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-wsaeventselect)、[WSAEnumNetworkEvents](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-wsaenumnetworkevents)

## MCPクライアントの調査結果

以下は2026年10月21日時点の調査結果である。接続先の制約と、クライアントの組織管理設定は区別する。CodexとClaude Codeは、一般的なHTTP MCP接続をloopbackだけに限定していない。ローカルHTTPとリモートHTTPSの接続が案内されている。組織のポリシーや実行環境によって、利用可能なサーバーが追加で制限される場合がある。

| クライアント | HTTP接続と認証 | 今回の用途への影響 |
| --- | --- | --- |
| Codex CLI | Streamable HTTPのURL、Bearerを読む環境変数、固定・環境変数由来のHTTPヘッダー、OAuthを設定できる。調査環境のCLIは`0.163.0`。 | localhostのURLだけでも登録できる。Bearerを採用してもOAuthサーバーの実装は必須ではない。 |
| Claude Code | HTTP URL、追加ヘッダー、環境変数の展開、ヘッダー取得用helper、OAuthに対応する。 | 手動設定のBearerを利用できる。独自の環境変数名を使い、保護対象の環境変数の扱いと混同しない。 |
| VS CodeのMCPクライアント | stdioとHTTPなどを使い、HTTPの`headers`は任意設定である。 | サーバーの認証既定値をVS Codeが統一するわけではない。 |

根拠は[CodexのMCP説明](https://learn.chatgpt.com/docs/extend/mcp?surface=cli)、[Codex設定リファレンス](https://learn.chatgpt.com/docs/config-file/config-reference)、[Claude CodeのMCP説明](https://code.claude.com/docs/en/mcp)、[Claude Codeの管理ポリシー](https://code.claude.com/docs/en/managed-mcp)、[VS CodeのMCP設定仕様](https://code.visualstudio.com/docs/agents/reference/mcp-configuration)である。

Claude CodeのOAuthで使うエンドポイントやlocalhostの例外に関する条件を、MCPサーバーの接続先全般の制約と混同しない。WebSocket接続についても、Claude Codeの個別対応やIDE専用連携が、そのままCodexやすべてのMCPクライアントで利用できることを意味しない。共通の接続例はStreamable HTTPを基本とする。

調査時のClaude CodeはJSON設定の`type: "ws"`でWebSocketサーバーを登録できるが、この接続ではヘッダー認証を使い、OAuthには対応しない。CLIの`--transport ws`も使わない。Codexの公式な接続方式はstdioとStreamable HTTPである。[Claude CodeのWebSocket設定](https://code.claude.com/docs/en/mcp)、[Codexの対応方式](https://learn.chatgpt.com/docs/extend/mcp?surface=cli)

### 接続設定の例

ポート`8765`とパス`/mcp`は説明用の値であり、muon全体の予約ポートや必須パスとして採用したものではない。

Codexの認証なしの例は次のとおりである。Bearerを有効にする場合は、同じ設定へ`bearer_token_env_var = "MUON_MCP_TOKEN"`を追加する。

```toml
[mcp_servers.muon_app]
url = "http://127.0.0.1:8765/mcp"
```

Claude Codeは次のコマンドでHTTPサーバーを登録できる。

```sh
claude mcp add --transport http muon-app http://127.0.0.1:8765/mcp
```

Bearerを使うClaude Codeの設定例では、ヘッダーに独自の環境変数を指定する。サーバー側にも同じトークンを設定する。

```json
{
  "mcpServers": {
    "muon-app": {
      "type": "http",
      "url": "http://127.0.0.1:8765/mcp",
      "headers": {
        "Authorization": "Bearer ${MUON_MCP_TOKEN}"
      }
    }
  }
}
```

これらは公式の設定方式に基づく例であり、未実装のmuonサーバーへの接続試験を済ませたものではない。

## 他のローカルMCPサーバーの既定値

| 実装 | 待受と有効化 | 認証 | 追加の制約と調査範囲 |
| --- | --- | --- | --- |
| [DebugMCP](https://github.com/microsoft/DebugMCP#security-model) | 既定で`127.0.0.1`と`::1`、ポート3001。 | 公開MCP HTTPエンドポイントは認証なし。 | Hostをloopback名と待受ポートで検証する。Originがある場合もloopback名で検証する。公式説明とコードを確認した。 |
| [FigmaデスクトップMCP](https://developers.figma.com/docs/figma-mcp-server/local-server-installation/) | アプリ内で有効化し、`127.0.0.1:3845/mcp`で接続する。 | 公式導入手順では接続用トークンの設定が不要。 | Host／Origin検証の内部仕様は公開資料から確認できていない。Figmaアプリ側のアカウント条件とは別である。 |
| [Playwright MCP](https://playwright.dev/mcp/configuration/options) | 通常の起動例はstdio。HTTPモードは`--port`で起動し、既定のhostはlocalhost。 | 公式HTTP接続例はトークン指定なし。 | 全インターフェイスの待受には`--host 0.0.0.0`を明示する。サーバー向けのallowed hosts設定がある。 |
| [Claude Code拡張のIDE MCP](https://code.claude.com/docs/en/vs-code#the-built-in-ide-mcp-server) | `127.0.0.1`の10000〜65535から選んだランダムポートでWSを待ち受ける。 | 拡張の起動ごとに生成するランダムトークンが必須。 | CLIがローカルのlock fileから取得し、`X-Claude-Code-Ide-Authorization`を送る。利用者による手入力を不要にしている。 |

DebugMCPはMicrosoftのリポジトリにあるVS Code拡張であり、VS Code本体の既定設定ではない。調査したコードはコミット`0c4cae41769e8ce80f7eca25ed83bef1c649ebb4`、バージョン`2.4.3`である。[bindの既定値](https://github.com/microsoft/DebugMCP/blob/0c4cae41769e8ce80f7eca25ed83bef1c649ebb4/package.json#L103)と[Host／Origin検証](https://github.com/microsoft/DebugMCP/blob/0c4cae41769e8ce80f7eca25ed83bef1c649ebb4/src/debugMCPServer.ts#L411)を確認した。OriginのないCLI要求は許可する。複数ウインドウ間の内部制御には別のトークンがあり、公開MCP入口の認証とは区別する。

Claude CodeのIDE連携は、`~/.claude/ide/<port>.lock`を`0600`、ディレクトリを`0700`としてトークンを保管する例である。専用CLIによる自動取得で利便性を維持している。ただし、muonが同じようなファイルを作っても、汎用クライアントが自動で読むわけではない。自動登録や設定生成は、必要になった段階で別途設計する。

Playwrightの`network.allowedOrigins`は、操作対象のブラウザーがアクセスする宛先の設定である。サーバーへ入る要求のOrigin検証として扱わない。

MCPのTypeScript SDKでも、調査時のmainにある[Express用の生成処理](https://github.com/modelcontextprotocol/typescript-sdk/blob/c05225a636c679870b65f8ca4fe83a76ea1cf113/packages/middleware/express/src/express.ts)は、localhost用のHost／Origin検証を組み込んでいる。Originがない要求は許可する。ただし、この関数自体はソケットのlistenを行わないため、アプリ側でも適切なアドレスへbindする必要がある。これは当該コミットの実装であり、すべての配布済みSDKバージョンの動作を示すものではない。

## 受信側の標準設定

### 待受と認証

| 項目 | 採用する標準案 |
| --- | --- |
| 起動 | アプリがサーバー機能を明示的に開始する。機能を使っていないアプリで待受を作らない。 |
| アドレス | `127.0.0.1`を基本とする。IPv6を提供する場合は`::1`も明示的に扱い、`0.0.0.0`や`::`へ自動的に広げない。 |
| Bearer未設定 | 認証なしで利用できる。同じPCのローカルプロセスを信頼する。 |
| Bearer設定済み | 認証対象のHTTP要求とWebSocketの接続要求で検証し、未提示・不一致を拒否する。「ヘッダーがある場合だけ検証する」動作にはしない。 |
| Host | 想定したloopbackホスト名と待受ポートを検証する。外部から渡された任意のホスト名を許可しない。 |
| Origin | CLIとの互換性のため、ヘッダーがない要求も許可する。ある場合は明示的な許可リストで照合し、不許可の値や`null`を拒否する。 |
| WebSocket | Upgradeを受け入れる前に同じ受信ポリシーを適用する。 |
| LAN公開 | ローカル専用設定から自動的には移行しない。別の明示設定とし、認証と暗号化の方針を別途検討する。初期実装の必須範囲には含めない。 |

接続元の判断には実際のソケットの情報を使う。Hostは接続先の名前を示すヘッダーであり、接続元の証明にはならない。転送を信用する構成を設けていない状態で、`X-Forwarded-For`などを接続元として扱わない。

Bearerなしでは、CodexやClaude Codeに加え、通常は別のOSユーザーのプロセスも接続できる。Host／Origin検証も任意のヘッダーを送れるローカルプログラムを認証する機能ではない。ユーザーやクライアントを区別したいアプリではBearerを有効にする。トークンはURL、ログ、エラーメッセージへ含めない。

### ブラウザーからの接続

Originは、ブラウザーが要求を発行したページの出所を示す。Host／Origin検証は外部WebサイトからユーザーのPC内のサーバーを操作する経路を制限するために行う。DNS rebindingは攻撃者のホスト名の解決先をloopbackなどへ変えてブラウザーから接続させる攻撃である。

アプリ内クライアントを許可する場合は、実際のアプリOriginを登録する。`asset://main`などの製品アプリと、開発用HTTPサーバーのOriginを区別し、開発環境の広い許可を製品へ持ち込まない。`localhost`由来のページをポートに関係なくすべて信頼することも既定にはしない。

CEF内から接続するには、受信側の許可に加え、既存の`network.allow`へ宛先URLを登録し、`network.localAccess.loopbackOrigins`へ接続元ページのOriginを完全一致で登録する。後者は宛先の一覧ではなく、未指定の権限要求は拒否される。既存の`network.authorizedOrigin`も送信側の設定であり、受信側のOrigin許可と混同しない。[現在の外部ネットワーク設定](../external-network.md)

CORSは、ブラウザーが別Originの応答を読み取るための許可である。受信拒否やクライアント認証の代わりにはならない。許可したOriginに必要な応答ヘッダーを返し、認証やMCPのヘッダーを伴う要求に対するOPTIONSの事前確認も扱う。事前確認には通常Bearerが付かないため、許可したOrigin・メソッド・ヘッダーを確認して応答し、アプリの操作は実行しない。本体の要求では認証を省略しない。

OriginがないことはCLI由来である証明にはならない。ブラウザーからの画像取得やナビゲーションなどのGET／HEAD要求にも、Originが付かない場合がある。汎用HTTPの実装では、GET／HEADへ状態変更を割り当てず、用途に応じた追加の検証が必要か確認する。Host／Originだけで外部Webサイトからのあらゆる要求を拒否できるとは説明しない。[Fetch StandardのOrigin付加規則](https://fetch.spec.whatwg.org/#append-a-request-origin-header)

ブラウザー標準のWebSocket APIは任意のAuthorizationヘッダーを指定できない。Bearerを有効にした内部WebSocket接続では、所有元と宛先を限定したネイティブ側の付加や別の認証手順が必要になる。この接続方法は未確定であり、ブラウザーからのfetchと同じ設定だけで動くとは扱わない。[WebSocketの公開インターフェイス](https://websockets.spec.whatwg.org/#the-websocket-interface)

### MCP仕様との関係

[MCPのStreamable HTTP仕様](https://modelcontextprotocol.io/specification/2026-07-28/basic/transports/streamable-http#security--endpoint)では、Origin検証が必須で、不正なOriginを伴う要求は403で拒否する。ローカル実行時のlocalhost限定bindと、すべての接続に対する適切な認証は推奨されている。

Bearerなしを標準にする方針は、認証省略を仕様が推奨しているという意味ではない。同じPC上のプロセスを信頼する用途で、接続設定の負担を減らすための選択である。認証を有効にしても、OriginやHostの検証は継続する。

## 生存期間と所有権

### 初版のページ連動

初版はサーバーを作成したページの実行コンテキストが解放されたときに、ハンドラー登録を解除し、そのサーバーを停止する。通常のページ遷移、リロード、frameの破棄、rendererの終了を対象にする。JavaScriptの操作用オブジェクトを保持することや、ネイティブ側で関数参照をretainすることによって、元のJavaScript実行環境が存続するとは扱わない。

MPAは画面遷移で別のHTML文書を読み込み、SPAは同じ文書の中で画面を切り替える構成を指す。初版で通信を継続する条件は、ハンドラーを登録したページの実行コンテキストを維持することである。アプリ全体をSPAにする制約は課さない。SPAでもリロードするとサーバーは停止する。

ネイティブの待受を残すことと、処理中のJavaScriptを継続することは分ける。現在の[ページ終了処理](../../../core/cef/src/app/muon_app.cpp)は未完了Promiseと関数参照を解放し、[共通ランタイム](../../../core/common/src/plugins/muon_plugin_runtime.cpp)もその所有元のコールバックを無効化する。この動作を変更して古いページを延命する方式にはしない。

### サーバーとハンドラー登録の分離

| 管理対象 | 所有する情報 | ページとの関係 |
| --- | --- | --- |
| ネイティブのサーバー | 待受ポート、認証、Host／Origin設定、接続の管理情報。 | browser process側のサービスが管理する。初版では作成元ページの終了を契機に停止する。 |
| ハンドラー登録 | サーバーとJavaScript関数の対応、登録元の所有者、登録の世代。 | 登録元ページに属する。サーバーの寿命とは別に解除できる。 |
| 要求・接続の処理 | 応答の状態、SSEの送信、WebSocketメッセージの配送先など。 | 受け付けた時点のハンドラー登録に対応付ける。新しい登録へ自動で移さない。 |

この分離は将来のアプリ連動を実装する前から採用する。JavaScriptのサーバーオブジェクトは、ネイティブの管理対象を操作するハンドルにする。ページ連動かアプリ連動かの違いを、所有元が消えたときの停止規則へ限定できるようにする。

現行の[通知サービス](../../../core/cef/src/notifications/muon_notification_service.cpp)には、アプリ全体のサービスとページ単位の購読を分ける構成がある。`ReleaseOwner`は該当ページの購読と待機を終了し、`Stop`はサービス全体を停止する。[内蔵通知プラグイン](../../../core/cef/src/plugins/builtin/muon_builtin_notifications.cpp)も両方を別のフックとして登録している。サーバーでもこの所有期間の分離を参考にする。通知のディスク永続化をサーバーへ導入する必要はない。

所有元にはbrowser側で確認したbrowser・frame・contextの情報を使う。rendererの数値のcontext IDだけではプロセスをまたいで一意にならないため、rendererが渡した値だけで所有者を決めない。ハンドラー登録には世代を持たせ、解除後や再登録後に届く古い応答を拒否する。サーバーの名前やIDを知っているだけで、別ページが停止やハンドラーの交換をできる設計にはしない。

### ハンドラー不在と処理中の要求

サーバーを停止する操作と、ハンドラー登録だけを解除する操作を分ける。待受が存続していても、有効なハンドラーがない間の新しいHTTP要求にはネイティブ側で503を返せる構成にする。WebSocketの新しい接続要求もアプリ側の処理先がない状態では受け入れない。再登録を待つ要求を無制限に蓄積しない。

ページが失われたときは、未完了のHTTP要求に対して応答開始前ならエラーを返し、応答開始後のHTTP・SSEと接続済みWebSocketは終了する。新しいページへ同じ要求を自動再配送しない。保存などの副作用が完了した後で応答だけが失われた場合、再配送すると操作が重複するためである。明示的な登録解除時に処理の完了を待つか打ち切るか、その詳細は実装時に定義する。

停止処理は接続・購読・コールバックの解放まで非同期に追跡する。通信部分の差替え後は、既存のcardio dispatcher上でソケットとRPCの実行を管理し、終了を同期的に待つことで相互に停止する構成を避ける。コールバック内から停止した場合も完了できるようにする。

### 将来のアプリ連動と変更コスト

将来のアプリ連動では、ページの終了時にハンドラー登録だけを解除し、サーバーを未登録状態へ戻す。新しいページは既存サーバーを取得してハンドラーを登録する。待受ポート、認証、Host／Origin設定を維持する部分では、初版の通信処理を再利用する。

| 将来追加する動作 | 分離設計を採用した場合の相対的な変更コスト | 主な追加内容 |
| --- | --- | --- |
| ページ遷移中も同じポートを維持 | 小 | ページ終了時にサーバーを停止しない所有規則。 |
| ハンドラー不在中のHTTP 503 | 小 | 未登録状態のネイティブ応答。明示的な登録解除にも使う。 |
| 新ページから既存サーバーへ再登録 | 中 | 検索、登録権限、重複登録と置換の規則。 |
| 処理中のHTTP・SSE・WebSocketを継続 | 大 | JavaScriptの実行場所や状態の保持方法の変更。 |
| 全ウインドウを閉じた後も常駐 | 別途、中〜大 | アプリの終了条件、常駐と明示終了の設計。 |

この評価は実装日数の見積もりではなく、変更する範囲の比較である。ページ用のオブジェクトへ待受、ハンドラー、要求状態をまとめる実装では、後からそれらを分解する変更も必要になる。初版から分離すれば、将来の変更を管理と登録の追加へ寄せられる。ただし、処理中のJavaScriptの継続まで小変更で実現できるとはしない。

MPAの画面遷移中も処理を継続する場合は、画面遷移しない制御用のJavaScript実行環境を設け、ハンドラーとMCPの実行状態をそこへ置く構成を将来候補とする。表示用のページとは独立させる。待受だけを存続させる段階と、この実行環境を追加する段階は別の拡張として扱う。

アプリ連動は、まずアプリのプロセスが動作している間の生存期間を意味する。現在の[終了判定](../../../core/cef/src/browser/muon_client.cpp)は、最後のbrowserが閉じられるとプラグインを停止してCEFを終了する。全ウインドウを閉じた後の常駐や、プロセスの再起動を越えた継続は、サーバーの所有期間を変えるだけでは実現しない。

## 内蔵プラグインの公開API

### 比較と採用結果

| 案 | APIの形式 | 利点 | 制約と採否 |
| --- | --- | --- | --- |
| 案1 | `serve(options)`へ設定とハンドラーを渡し、一括起動する。 | 小規模なサーバーを短く記述できる。 | サーバーとハンドラーの寿命が見えにくい。案2を呼び出す便利関数の候補として残す。初版の必須APIにはしない。 |
| 案2 | `createServer()`、`attach()`、`listen()`を分ける。 | 登録解除と停止を区別し、再登録やアプリ連動へ拡張しやすい。 | 操作と状態遷移が増える。基本APIとして採用し、HTTPにはFetch形式のハンドラーを組み合わせる。 |
| 案3 | サーバーIDと要求IDを使い、`watchRequests()`、`respond()`、`write()`などで操作する。 | ネイティブの管理やプロセス間通信へ対応付けやすい。 | 二重応答や応答忘れなどの管理を利用者へ要求する。内部の通信に使い、一般向けの低水準APIは必要性を確認してから公開する。 |

サーバーはmuon内蔵プラグインの一つとして実装する。計画上の名前空間は`muon.server`、validateモードの仮想モジュールは`muon:server`とする。simpleモードでは`window.muon.server`から対応する操作を使う。既存の許可設定と関数カタログへ組み込み、Androidには未対応の関数を公開しない。

### 作成・登録・待受・停止

```ts
import { createServer } from "muon:server";

const server = await createServer({
  name: "automation",
  port: 8765,
});

const binding = await server.attach({
  fetch: async (request) => {
    return new Response("ok");
  },
});

await server.listen();

// Release this page's handlers.
await binding.detach();

// Stop the server itself.
await server.close();
```

これは採用したAPIの使用例である。`automation`とポート`8765`は例示である。省略した認証や待受アドレスには、本書の標準設定を適用する。実装・検証済みの範囲は実施記録に記載する。

| 操作 | 役割 |
| --- | --- |
| `createServer(options)` | ネイティブ側に管理対象を作り、操作用ハンドルを返す。この段階では待ち受けない。 |
| `server.attach(handlers)` | 現在のページのハンドラーを登録し、その登録を解除するハンドルを返す。 |
| `server.listen()` | ハンドラーを登録した後に待受を開始する。開始の失敗を呼出し側へ返す。 |
| `binding.detach()` | ハンドラー登録を解除する。サーバー全体の停止とは区別する。 |
| `server.close()` | サーバー自体を停止し、接続と関連リソースを解放する。 |

初版では作成元ページが終了すると、明示的に`detach()`や`close()`を呼べなくてもネイティブ側で登録とサーバーを解放する。ハンドルはinterfaceとファクトリー関数で実装する。JavaScriptのオブジェクトとネイティブのリソースは別に管理し、将来のアプリ連動でページ側のハンドルが失われたことだけを理由にサーバーを停止しない構成にする。

### HTTPとWebSocket

HTTPハンドラーはWeb標準の`Request`を受け取り、`Response`またはそのPromiseを返す形式を採用する。SSEはストリームを本文に持つ応答として扱う。メソッド、URL、ヘッダー、本文、ステータスを保持し、バイナリーを文字列化しない。[FetchのRequest／Response仕様](https://fetch.spec.whatwg.org/#response-class)

`Request`、`Response`、`ReadableStream`をそのままRPCへ渡すことは前提にしない。renderer側のアダプターがメタデータ、バイナリーの本文、ストリームの操作へ変換する。本文の受信方法、キャンセル、送信Promiseの完了条件は、CEFの公開APIで実現できる範囲を検証して定める。

WebSocketは同じ`attach()`で接続要求、メッセージ、切断のハンドラーを登録する形とする。接続要求を許可した後は接続ハンドルで送受信する。HTTP応答とWebSocket接続の型は分け、text／binary、subprotocol、close codeなどの対応範囲をCEFの検証前に保証しない。WebSocketの具体的なメンバー名と型は未確定である。

MCP SDKのサーバー処理を載せるアダプターと、汎用HTTPサーバー機能は分ける。Fetch形式を採用しても、Nodeの`http.Server`やExpressとの直接互換性は保証しない。ブラウザーで使えるNPMパッケージとNode専用パッケージを区別し、利用するMCP SDKとの接続を検証する。

### 将来の既存サーバー取得

アプリ連動のサーバーを提供する段階で、次のように新しいページから取得・登録できるAPIを追加する。

```ts
import { getServer } from "muon:server";

const server = await getServer("automation");
const binding = await server.attach({
  fetch: async (request) => {
    return new Response("ok");
  },
});
```

`getServer()`とアプリ連動の生存期間指定は将来の追加であり、初版では未対応の関数や選択肢を公開しない。既存サーバーを取得することと、新しく作ることを区別する。同名の作成や重複登録の規則は初版で定義する。別ページから取得した後の登録の置換規則と、取得・再登録・停止に必要な権限は、アプリ連動を実装する段階で追加する。

## 非同期配信とリソース上限

CEF側でHost、Origin、認証を検証してから、許可した要求をJavaScriptへ渡す。要求処理中の例外、応答の二重完了、クライアントの切断、停止との競合も結果を定義する。

イベントの配送方法は保持したコールバックを使う方法と、[通知APIの待機型RPC](../../../core/common/renderer/notifications.ts)を使う方法を比較し、段階1で待機型RPCを採用した。待機型RPCはイベントが届くまで要求を保留する方式で、定期ポーリングではない。SSEなど一つの長時間処理が別のHTTP要求の受付を止めないよう、イベントの配送と各要求の処理期間を分ける。受信したデータは、非同期配送の前にmuonが所有するバッファーへ保持する。

同時接続数、要求本文、未処理要求、muonが保持する送信データには上限を設ける。段階1で採用した数値は実施記録に記載し、SSEとWebSocketの上限は後続段階で定める。差替え後は受信中のデータと送信待ちのデータも制限する。SSEやWebSocketの送信完了を、相手側で受信・処理が完了した保証として公開しない。

## 実装段階と完了条件

計画書の保存後に実施の指示を受け、以下の段階に沿って実装する。各段階は単体の変更と実行可能なドライバーをそろえ、挙動を観測できる単位で進める。機能テストで未実装の状態を確認してから実装し、適切な粒度でコミットする。

### 段階1 ローカルHTTPサーバー

CEF内蔵サービスにサーバー、ハンドラー登録、要求・接続の管理を分けて実装する。`createServer()`、`attach()`、`listen()`、`detach()`、`close()`を接続し、ドライバーアプリからFetch形式のHTTPハンドラーを登録する。同じPCのHTTPクライアントからJSONの取得と更新を行えるようにする。loopback bind、Host／Origin検証、所有元の解放をこの段階に含める。認証なしでも、不許可のOriginを受け入れる途中状態は完成扱いにしない。

完了条件は外部クライアントから要求と応答を確認でき、非loopbackの待受を作らず、不正なHost／Originがアプリのハンドラーへ届かないことである。登録解除とサーバー停止を別々に観測し、登録解除後は待受を維持しながらHTTP 503を返すこと、古い登録からの応答を拒否することを確認する。作成元ページの破棄では登録と待受の両方を解放し、ポートの競合を呼出し側へ報告する。同名の作成や重複登録などの規則も、この段階で定義・検証する。

### 段階2 BearerとCEF内クライアント

任意のBearer認証、Origin許可設定、CORSを追加する。ドライバー内のfetchクライアントと外部CLIを使い、内部専用、外部利用、内外両方の利用を切り替えて確認する。simpleとvalidateの許可設定、バイナリーの要求・応答も検証する。

完了条件は認証なしの接続、正しいBearerによる接続、未提示・不一致の拒否を区別できることである。許可したブラウザーから事前確認と本体の要求が成功し、不許可のOriginや資格情報を持たない外部クライアントでは内部専用機能を操作できないことを確認する。

### 段階3 SSEとMCPの接続

送信制約への対応として、次の単位で実装・検証する。

1. 非同期TCPの待受・送受信・取消を実装する。動作するドライバーで、低速受信、部分書込み、途中切断、停止時の解放を確認する。
2. HTTPの解析と応答を新しい通信部分へ移す。段階1・2のドライバーとテストを通し、公開APIと認証の挙動を維持する。有限応答が最後まで届き、エラーや停止で待機が残らないことを確認してからコミットする。
3. Fetch形式のストリーム応答、切断時の中断、アプリ側のエラー処理を実装する。小さなキューで読み手に合わせて送信し、総送信量だけを理由に長時間のSSEを終了させない。逐次受信と通常の要求の並行処理を確認する。
4. 最小のMCPアダプターをドライバーへ追加し、状態取得と状態変更をMCPの操作として公開する。実際のCodexとブラウザー対応のMCPクライアントから接続する。

完了条件は通常のJSON応答を処理でき、SSEイベントはHTTP応答全体の終了を待たずに逐次受信できることである。接続を切った後に要求や送信データが残り続けず、長時間のSSEが別の要求の受付を止めないことを確認する。ページ破棄後に古い要求を自動再実行しないことも確認する。Codexとブラウザークライアントからアプリの状態を読み書きでき、認証あり・なしの両方を確認する。使用したMCP SDK、プロトコル、クライアントのバージョンを記録する。Claude Codeの実接続検証は2026年10月21日のユーザー指示により省略し、公式の接続設定例を利用文書へ残す。

### 段階4 WebSocket

新しい通信部分にWebSocketを実装し、ドライバーで接続、text／binaryの送受信、切断を観測する。`attach()`へのWebSocketハンドラー登録と接続ハンドルを実装する。HTTPと同じHost／Origin／認証ポリシーを適用し、ブラウザー内から認証付きで接続する手段も検証する。フレーム解析のライブラリを採用する場合は、保守状況、公開API、Linux／Windowsのビルドを確認し、公式コードを変更せずに使う。

完了条件はCEF内と外部のクライアントで対応範囲のメッセージを往復でき、不許可の接続をUpgrade前に拒否することである。終了、切断、処理中の例外でリソースを解放し、提供できないsubprotocolやclose code等を利用文書で区別する。採用した通信部分で必要な用途を満たせない場合は、代替案を計画へ反映してから続ける。

### 段階5 対象環境と利用文書

LinuxとWindowsの配布対象でビルドと動作を確認し、汎用HTTP、WebSocket、MCPの利用例を日本語で整える。既存の英語文書へ反映する場合は日本語を原本とする。MCP専用の利用者にも、認証の既定値とローカルプロセスへの信頼が伝わるようにする。

完了条件は対象環境の必要なテストと全体の`npm test`が成功し、新たな警告を分析・対処していることである。WindowsはWineで確認できない製品上の挙動を実機で検証する。ドライバーと製品ビルドの両方で、終了後にサーバーが残らないことを確認する。

## 検証方法

| 対象 | 検証する挙動 |
| --- | --- |
| 待受 | IPv4、対応する場合はIPv6、ポート競合、開始失敗、停止後の再起動。待受アドレスがloopbackに限定されること。 |
| APIの状態遷移 | 作成時は未待受、ハンドラー登録後の待受開始、登録だけの解除、未登録時の503、再登録、全体停止。同名の作成や重複登録などが定義どおりに処理されること。 |
| 認証 | Bearer未設定、正しい値、未提示、誤った値。内部専用の資格情報を持たないクライアントの拒否。 |
| Host／Origin | 正常な値、不正なホストとポート、不正なOrigin、`null`、Originなし。拒否した要求でアプリの状態が変化しないこと。 |
| CEF内のHTTP | 製品のasset Origin、開発Origin、OPTIONS、許可したヘッダー、外向きのnetwork設定との組合せ。 |
| HTTPとSSE | Request／Responseの変換、JSON、バイナリー、ヘッダー、エラー応答、逐次受信、途中切断、上限超過。長時間のSSE中も別の要求を処理できること。 |
| WebSocket | 許可・拒否、メッセージ種別、双方向送信、切断、認証付きのブラウザー接続、未対応操作。 |
| 所有期間 | 明示停止、コールバック内の停止、MPA遷移、リロード、frame破棄、ウインドウ終了、アプリ終了、別の所有元からの操作拒否。古い登録からの応答拒否と、旧要求を自動再配送しないこと。 |
| MCP | 初期化、操作一覧、状態取得と変更、SSE、クライアントからの切断。Codexとブラウザー対応クライアントで確認する。Claude Codeの実接続検証はユーザー指示により省略する。 |

テストはソースに特定の文字列が存在することではなく、公開APIと実際の通信結果を確認する。ブラウザーやmuonの試行・E2EではPlaywright MCPを利用する。受信完了や切断通知などを待ち、固定時間のsleepで成否を決めない。停止時に滞留する場合は、待ち時間を増やす前に所有期間や完了通知の関係を調べる。

## 全体の完了条件

1. CEF版の内蔵プラグインとしてHTTPとWebSocketのサーバー機能があり、MCP以外の用途にも使える。作成・登録・待受・登録解除・停止を分け、HTTPはFetch形式で扱う。
2. CEF内だけ、外部CLI、内外両方の利用形態を、認証とOriginの条件を含めて実行例で確認できる。
3. 既定の待受がloopbackに限定され、Host／Origin検証と任意のBearer認証が機能する。
4. Codexとブラウザー対応クライアントから実行中のmuonアプリをStreamable HTTPで操作できる。Claude Codeは公式の接続設定例を示し、実接続検証の省略を明記する。
5. ネイティブのサーバー、ページのハンドラー登録、要求・接続の所有元を分ける。初版では作成元ページの破棄時に登録とサーバーを終了し、古い応答を適用せず、要求を自動再実行しない。アプリ連動やMPA遷移中の通信継続は、この初版の完了条件に含めない。
6. 採用した通信部分のSSE・WebSocket・TLSの対応範囲と、対応するクライアント・プロトコルの範囲を、根拠付きで説明できる。
7. simpleとvalidateの両モード、対象のLinux・Windows環境で検証し、全体テストが成功する。Androidのサーバー対応は追加しない。

## 未確定事項

- 後続段階で追加するAPIの詳細な引数・戻り値、WebSocketのメンバー名と型。名前の衝突と重複登録、明示的な登録解除で処理中の要求を打ち切る規則は、段階1の実施記録に記載する。
- 動的ポートの提供方法と、IPv4とIPv6を同じサーバーとして扱う範囲。段階1はIPv4と明示指定のポートを扱う。
- 将来のアプリ連動を追加する時期と生存期間の指定方法、既存サーバー取得・再登録の権限、常駐するJavaScript実行環境の提供方法。初版のページ連動と管理対象の分離は採用済みである。
- 内部専用のWebSocket接続で、ブラウザーから資格情報を付加する方法。HTTPは段階2の実施記録に記載したBearer設定を使う。
- SSEとWebSocketのリソース上限、要求のタイムアウト。送信Promiseの完了は、対象データをOSが受け付けた時点とする。
- 使用するMCP SDKと対応プロトコルのバージョン、CEFで提供するWebSocketの機能範囲。

アプリ連動の生存期間と`getServer()`、画面遷移から独立したJavaScript実行環境、全ウインドウを閉じた後の常駐、便利関数`serve()`、一般向けの低水準APIは初版の必須範囲に含めない。LAN公開、TLS、OAuth認可サーバー、クライアントへの自動登録、TCPを使わない内部専用経路も後続の検討対象とする。必要になった場合に、追加する用途と完了条件をこの計画へ記録する。

## 実施記録

2026年10月21日に段階1を実装した。CEF内蔵プラグイン、ページ所有元の検証、Fetch形式のハンドラー、simple／validateの公開処理を追加した。既存の配布runtimeに対し、Playwright MCP経由でサーバーAPIが存在しないことによるテストの失敗を確認してから実装した。Linux Debugビルド、ネイティブテスト58件、関連するTypeScriptテスト155件が成功した。CEFの実通信テストでも、JSONの取得・更新、バイナリーの往復、Host／Originの拒否、登録解除後の503、再登録、古い応答の破棄、ポート競合後の再試行、ページ遷移時の停止を確認した。

段階1では次の詳細を採用する。

- 待受は`127.0.0.1`、ポートは`1025..65535`の明示指定とする。動的ポートとIPv6は、対応を検証するまで公開しない。
- 名前はページ内で一意にし、同名の作成とハンドラーの重複登録を拒否する。ポート競合で待受に失敗したハンドルは再試行できる。
- `detach()`は処理中の要求を打ち切り、応答前のHTTP要求へ503を返す。新しい登録へ旧要求を移さず、古い応答を拒否する。`close()`で閉じたハンドルは再利用せず、新しいサーバーを作成する。
- 要求本文とバッファーに保持する応答本文は各1 MiB、サーバーごとの接続と処理中要求は各32、アプリ内のサーバーは32を上限とする。CEFが受信中に保持するメモリーまで、この制限で保証するものではない。
- ネイティブの応答バッファーはSDKの`allocate_shared_buffer`で確保した領域をruntimeへ渡す。非同期RPCの完了まで本文を保持する。

段階2では`bearerToken`と`allowedOrigins`を作成時の設定へ追加した。Bearerを指定したサーバーが認証なしで201を返すテストの失敗を確認してから、ネイティブ側の認証を実装した。HTTP由来のページと外部クライアントを使う実通信テストはsimple／validateの両モードで成功した。製品の`asset://main`ページでも、両モードでBearer付きのバイナリー往復、MCPと独自の応答ヘッダーの読み取り、認証エラーの401を確認した。ネイティブテスト58件、設定の受け渡しと公開型などのTypeScriptテスト22件も成功した。

段階2では次の詳細を採用する。

- Bearerはアプリが生成・保持して設定する。省略時だけ認証なしとし、空文字列、形式不正、4096文字を超える値は拒否する。文字の形式は[RFC 6750のb64token](https://www.rfc-editor.org/rfc/rfc6750#section-2.1)に従う。値をURL、ログ、エラーメッセージへ含めない。
- Originは`http`、`https`、`asset`の完全一致で許可する。ワイルドカード、`null`、パス、クエリー、fragmentを含む値は受け付けず、省略時は空の許可一覧とする。一覧は64件、各文字列は2048バイトまでとする。Originがない要求もBearerの対象になる。
- 許可したOriginのCORS事前確認は、メソッドとヘッダー名を検証して204を返し、アプリのハンドラーを呼ばない。本体の要求でBearerが未提示・不一致なら401、不正なHost／OriginはBearerが正しくても403にする。通常のOPTIONS要求は認証を省略しない。
- CORSのヘッダーはネイティブ側で管理し、許可したOriginからはアプリの応答ヘッダーと認証エラーを読み取れるようにする。ブラウザーのfetchは既定のcredentials設定、または`omit`を使う。Cookieを送る`include`には対応しない。[FetchのCORSとcredentials](https://fetch.spec.whatwg.org/#cors-protocol-and-credentials)
- 重複したAuthorization／Origin、不正な設定値や事前確認を拒否すること、拒否時にハンドラーが実行されないこと、認証なしで内外から共有できることを実通信で確認した。

段階3の最初の単位として、通信部分をcardioの非同期TCPとllhttp 9.4.3へ差し替えた。公開API、ページ所有元、Bearer、Host／Origin、CORSの規則は維持した。llhttpは公式の生成済みCコードをSHA-256で固定して取得し、変更せずに組み込んだ。通常の応答は全バイトをOSへ渡してから接続を閉じる。待機中の接続の停止ではI/Oの取消を回収してからソケットを解放する。

HTTPパーサーのテストでは、未実装の失敗を確認してから、分割入力、重複ヘッダー、バイナリー、chunked本文、Upgrade後の未消費データ、不正な形式、上限超過、途中のEOFを検証した。空白の読み飛ばしによる上限超過も失敗を再現して修正した。要求ヘッダーは通信上の表現を含め32 KiB、要求先は8 KiB、本文は1 MiBに制限する。ヘッダーは128個までとし、本文を含めた要求全体にも1 MiBと64 KiBの合計の上限を設け、chunkの付加情報も無制限には受け付けない。

Linuxではネイティブテスト60件とテストドライバーのビルドが成功した。TCPの実通信では8 MiBの送信と別接続の進行、送受信待機の取消、80接続の待機と停止を確認した。CEF経由でもsimple／validateの両モードで既存のHTTP・認証・CORS・asset Originのテストが成功した。追加した1 MiB応答の全バイト比較、クライアントの送信側終了後の応答、接続リセット後のハンドラー中断も成功した。Windows向けのコンパイルとWineでの80接続の確認は成功しているが、実機での送信待機と終了の確認は残っている。

SSE／MCP、WebSocket、Windows実機での検証、全体の`npm test`は後続段階に残っている。段階ごとの実装や個別テストの成功を、全体の完了とは扱わない。

## 計画書保存時の確認

初回の保存では、採用方式、三つの利用形態、内部専用方式の比較、クライアントと他実装の調査、標準設定、CEFの制約、段階的な実装案と完了条件を記録した。

今回の更新では、初版のページ連動、サーバーとハンドラー登録の分離、将来の変更コスト、MPAでの継続に必要な追加範囲を反映した。公開APIは作成・登録・待受を分ける案2とFetch形式を組み合わせ、比較した他案の位置付けと将来の既存サーバー取得も記録した。実装段階、検証方法、全体の完了条件、未確定事項を採用方針に合わせて更新した。

計画書を保存した時点では、製品コード、テスト、ビルド設定を変更せず、製品のビルドと全体テストも実行していない。計画書へのリンクは一般の利用文書へ追加しない。実装開始後の進捗は実施記録へ追記し、実装と検証が終わった段階で全体の完了条件を照合する。

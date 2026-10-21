# CEF版のHTTP・WebSocketサーバーとローカルMCP連携

## 目的と採用方針

CEF版のmuonアプリからHTTP・WebSocketサーバーを公開し、アプリ内のブラウザーと、同じPCで動作する外部プログラムから利用できるようにする。主な用途は実行中のmuonアプリをCodexやClaude CodeからMCPで操作することである。MCP以外のHTTP APIやWebSocket通信にも使える汎用機能として提供する。Androidは対象に含めない。

サーバーの実装には、議論で案2として選択したCEFの`CefServer`を使う。muonの内蔵機能として受信・応答・接続管理を公開し、MCPのメソッドやアプリ固有の操作はその上のJavaScript側で実装する。Node.jsを必須依存にはしない。

標準設定はloopbackだけでの待受、HostとOriginの検証、任意のBearer認証を組み合わせる。Bearerを設定しない場合は、同じPCのローカルプロセスを信頼する。設定した場合は、認証対象の要求でトークンを必須にする。不許可のHost／Originを拒否する処理はBearerの有無にかかわらず有効にする。

作成日は2026年10月21日、muon-uiの基準コミットは`8eb1c1f899a4b88e9b4369f2f9c111b0c4e67cb3`。参照CEFは`147.0.14+g76d2442+chromium-147.0.7727.138`である。本書は議論と調査結果の保存であり、機能は未実装である。採用方針と、実装前に確認する事項を分けて記載する。公開APIの識別子、リソースの所有期間、上限値など、まだ合意していない詳細は実装案として扱う。

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
| 案2：`CefServer`を内蔵機能として公開 | CEFに含まれるHTTP・WebSocket機能を使い、Nodeを追加せずにアプリへ受信機能を提供できる。 | JavaScriptとの非同期連携と接続管理が必要。少数のローカル接続向けで、TLSやWebSocketの細かな制御には公開API上の制約がある。採用する。 |
| 独自のネイティブサーバーや別プロセス | 必要なHTTP・WebSocket仕様やTLSを選びやすい。 | 依存関係、配布、終了処理、プラットフォームごとの検証が増える。CEFの制約で必要な用途を満たせない場合の再検討候補とする。 |
| CEF内のリクエストを処理する仮想HTTPサーバー | TCPポートを開かずに、ブラウザーからのHTTP要求を処理できる。 | 外部クライアントから直接接続できない。WebSocketはHTTPリソース処理だけでは同じ形で提供できない。内部専用の別案として比較した。 |

Node sidecarの実例は[Expressのfixture](../../../core/cef-test/fixtures/node-express-project/server.mjs)と[そのE2Eテスト](../../../core/cef-test/plugin-e2e/node-plugin-express.ts)にある。fixtureは`127.0.0.1`の動的ポートで待ち受ける。この例のCORS設定を、そのまま新しい内蔵サーバーの既定値にはしない。

### CEF内だけに公開する二つの方法

| 比較項目 | loopbackと認証を使う方法 | TCPポートを開かない方法 |
| --- | --- | --- |
| 外部プロセスからのTCP接続 | 接続は可能。認証で利用を拒否する。 | サーバーのTCPポートが存在しない。 |
| HTTPクライアントとの互換性 | 通常のHTTP URLを使いやすい。 | `CefResourceHandler`などで要求を処理する。URLのscheme、Origin、リソース処理の条件を合わせる必要がある。 |
| WebSocket | 実際の待受を使える。ただし`CefServer`の対応範囲を検証する。 | 通常のWebSocketをリソースハンドラーだけで置き換えることはできず、別の通信アダプターが必要になる。 |
| 外部公開への切替 | 同じ受信機能を使い、認証と公開設定を変更できる。 | 外部向けに別の実サーバーが必要になる。 |
| 運用上の負担 | ポートの競合、資格情報の配布、Host／Origin検証を扱う。 | ポートの競合はないが、内部専用経路と外部経路を別に検証する。 |

議論では外部プロセスからTCP接続されること自体を禁止せず、認証で利用を制約する前者を選択した。その後、最も多い用途を同じPCの外部CLIからの操作と整理し、標準設定ではBearerを任意にする方針になった。

この変更によって、認証なしの標準設定を「CEF内だけに公開」と呼ぶことはできない。内部専用の利用では外部へ配布しない資格情報を使う。資格情報をJavaScriptへ渡すか、muonのネイティブ側で接続先と所有元を限定して付加するかは未確定である。同じOSユーザーの任意コードからも秘密を守れる、という保証はしない。

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

## APIとリソース管理の実装案

公開機能はサーバーの開始・停止、HTTP要求への応答、逐次応答、WebSocket接続とメッセージ、切断通知に分ける。具体的な名前空間、関数名、オブジェクトの形はまだ固定しない。simpleとvalidateの両モードで同じ意味の操作を使えるようにし、Androidには未対応の関数を公開しない。

HTTPの受信データはメソッド、URL、ヘッダー、本文、接続の情報を保持する。応答ではステータス、ヘッダー、本文を指定する。バイナリーを文字列化せず、既存のRPCのバッファー対応を使う。JavaScriptの`Request`／`Response`や`ReadableStream`に合わせたアダプターを作る場合も、ストリームやキャンセルがプロセス間でそのまま渡るとは仮定しない。

MCP SDKのサーバー処理を載せるアダプターと、汎用HTTPサーバー機能は分ける。Nodeの`http.Server`互換性やExpress互換性は、この内蔵APIの前提にはしない。ブラウザーで使えるNPMパッケージと、Node専用のパッケージも区別する。

CEF側でHost、Origin、認証を検証してから、許可した要求をJavaScriptへ渡す。要求、接続、サーバーの識別子は所有元と対応付け、別のbrowser・frame・contextから操作できないようにする。要求処理中の例外、応答の二重完了、クライアントの切断、停止との競合も結果を定義する。

rendererの数値のcontext IDだけでは、プロセスをまたいで所有元を一意に識別できない。所有元にはbrowser側で確認した情報を使い、rendererから渡された所有元の値をそのまま信用しない。

イベントの配送方法は保持したコールバックを使う方法と、[通知APIの待機型RPC](../../../core/common/renderer/notifications.ts)を使う方法を比較する。待機型RPCはイベントが届くまで要求を保留する方式で、定期ポーリングではない。どちらを選ぶ場合も、SSEなど一つの長時間処理が別のHTTP要求の受付を止めないよう、イベントの配送と各要求の処理期間を分ける。WebSocketで受信したデータはCEFのコールバック中だけ有効なので、非同期配送の前にmuonが所有するバッファーへコピーする。

最初の所有期間の案はサーバーを作成したページの実行コンテキストに合わせ、リロードや破棄時に閉じる方式である。ネイティブの待受だけを残してJavaScriptの処理先が消える状態を避けられる。アプリ全体で待受を維持し、ページの再生成後に処理先を登録し直す方式は別案として残す。リロードを越えて継続する要件は未確定であり、実装時に選択と挙動を計画へ記録する。

停止処理は接続・購読・コールバックの解放まで非同期に追跡する。CEFの専用スレッドと既存のcardio／RPCの実行経路を接続し、終了を同期的に待つことで相互に停止する構成を避ける。コールバック内から停止した場合も完了できるようにする。

同時接続数、要求本文、未処理要求、muonが保持する送信データには上限を設ける案とする。数値は未決定である。CEF内部のバッファーまでmuonが制御できるとは限らないため、どの段階で制限できるかを検証する。SSEやWebSocketの送信完了を、相手側で受信・処理が完了した保証として公開しない。

## 実装段階と完了条件

以下は今後の実装手順案である。今回の依頼は計画書の保存までであり、これらの実装は開始しない。各段階は単体の変更と実行可能なドライバーをそろえ、挙動を観測できる単位で進める。機能テストで未実装の状態を確認してから実装し、適切な粒度でコミットする。

### 段階1 ローカルHTTPサーバー

CEF内蔵サービスと最小の公開APIを接続する。ドライバーアプリで開始・停止を操作し、同じPCのHTTPクライアントからJSONの取得と更新を行えるようにする。loopback bind、Host／Origin検証、所有元の解放をこの段階に含める。認証なしでも、不許可のOriginを受け入れる途中状態は完成扱いにしない。

完了条件は外部クライアントから要求と応答を確認でき、非loopbackの待受を作らず、不正なHost／Originがアプリのハンドラーへ届かないことである。停止とページ破棄で待受を解放し、ポートの競合を呼出し側へ報告できることも確認する。

### 段階2 BearerとCEF内クライアント

任意のBearer認証、Origin許可設定、CORSを追加する。ドライバー内のfetchクライアントと外部CLIを使い、内部専用、外部利用、内外両方の利用を切り替えて確認する。simpleとvalidateの許可設定、バイナリーの要求・応答も検証する。

完了条件は認証なしの接続、正しいBearerによる接続、未提示・不一致の拒否を区別できることである。許可したブラウザーから事前確認と本体の要求が成功し、不許可のOriginや資格情報を持たない外部クライアントでは内部専用機能を操作できないことを確認する。

### 段階3 SSEとMCPの接続

逐次応答、切断時の中断、アプリ側のエラー処理を実装し、最小のMCPアダプターをドライバーへ追加する。単純な状態取得と状態変更をMCPの操作として公開し、実際のCodexとClaude Codeから接続する。ブラウザー対応のMCPクライアントも同じサーバーへ接続する。

完了条件は通常のJSON応答を処理でき、SSEイベントはHTTP応答全体の終了を待たずに逐次受信できることである。接続を切った後に要求や送信データが残り続けないことも確認する。両CLIからアプリの状態を読み書きでき、認証あり・なしの両方を確認する。使用したMCP SDK、プロトコル、クライアントのバージョンを記録する。

### 段階4 WebSocket

まずドライバーで接続、送受信、切断を観測し、CEFのtext／binaryの扱いと公開APIの制約を確定する。その結果に合わせて一般向けの対応範囲を決める。HTTPと同じHost／Origin／認証ポリシーを適用し、ブラウザー内から認証付きで接続する手段も検証する。

完了条件はCEF内と外部のクライアントで対応範囲のメッセージを往復でき、不許可の接続をUpgrade前に拒否することである。終了、切断、処理中の例外でリソースを解放し、提供できないsubprotocolやclose code等を利用文書で区別する。CEFで必要な用途を満たせない場合は、代替案を計画へ反映してから続ける。

### 段階5 対象環境と利用文書

LinuxとWindowsの配布対象でビルドと動作を確認し、汎用HTTP、WebSocket、MCPの利用例を日本語で整える。既存の英語文書へ反映する場合は日本語を原本とする。MCP専用の利用者にも、認証の既定値とローカルプロセスへの信頼が伝わるようにする。

完了条件は対象環境の必要なテストと全体の`npm test`が成功し、新たな警告を分析・対処していることである。WindowsはWineで確認できない製品上の挙動を実機で検証する。ドライバーと製品ビルドの両方で、終了後にサーバーが残らないことを確認する。

## 検証方法

| 対象 | 検証する挙動 |
| --- | --- |
| 待受 | IPv4、対応する場合はIPv6、ポート競合、開始失敗、停止後の再起動。待受アドレスがloopbackに限定されること。 |
| 認証 | Bearer未設定、正しい値、未提示、誤った値。内部専用の資格情報を持たないクライアントの拒否。 |
| Host／Origin | 正常な値、不正なホストとポート、不正なOrigin、`null`、Originなし。拒否した要求でアプリの状態が変化しないこと。 |
| CEF内のHTTP | 製品のasset Origin、開発Origin、OPTIONS、許可したヘッダー、外向きのnetwork設定との組合せ。 |
| HTTPとSSE | JSON、バイナリー、ヘッダー、エラー応答、逐次受信、途中切断、上限超過。 |
| WebSocket | 許可・拒否、メッセージ種別、双方向送信、切断、認証付きのブラウザー接続、未対応操作。 |
| 所有期間 | 明示停止、コールバック内の停止、リロード、frame破棄、ウインドウ終了、アプリ終了、別の所有元からの操作拒否。 |
| MCP | 初期化、操作一覧、状態取得と変更、SSE、クライアントからの切断。Codex、Claude Code、ブラウザー対応クライアントで確認する。 |

テストはソースに特定の文字列が存在することではなく、公開APIと実際の通信結果を確認する。ブラウザーやmuonの試行・E2EではPlaywright MCPを利用する。受信完了や切断通知などを待ち、固定時間のsleepで成否を決めない。停止時に滞留する場合は、待ち時間を増やす前に所有期間や完了通知の関係を調べる。

## 全体の完了条件

1. CEF版にHTTPとWebSocketのサーバー機能があり、MCP以外の用途にも使える。
2. CEF内だけ、外部CLI、内外両方の利用形態を、認証とOriginの条件を含めて実行例で確認できる。
3. 既定の待受がloopbackに限定され、Host／Origin検証と任意のBearer認証が機能する。
4. CodexとClaude Codeから実行中のmuonアプリをStreamable HTTPで操作できる。
5. 接続、要求、購読、コールバックの所有元と終了条件が定義され、リロードやアプリ終了後に不要な待受と参照が残らない。
6. CEFのSSE・WebSocket・TLSの制約と、対応するクライアント・プロトコルの範囲を、根拠付きで説明できる。
7. simpleとvalidateの両モード、対象のLinux・Windows環境で検証し、全体テストが成功する。Androidのサーバー対応は追加しない。

## 未確定事項

- 公開する名前空間、関数名、ハンドルとイベントの型。
- 固定ポートと動的ポートの提供方法、IPv4とIPv6を同じサーバーとして扱う範囲。
- ページの実行コンテキストに合わせる所有期間と、アプリ全体で待受を維持する方式の採否。
- 内部専用の資格情報の生成・保持・付加方法。特にブラウザーのWebSocket接続方法。
- 送信Promiseの完了条件、各リソースの上限値、要求のタイムアウト。
- 使用するMCP SDKと対応プロトコルのバージョン、CEFで提供するWebSocketの機能範囲。

LAN公開、TLS、OAuth認可サーバー、クライアントへの自動登録、TCPを使わない内部専用経路は、今回の初期実装に必須の機能とはしない。必要になった場合に、追加する用途と完了条件をこの計画へ記録する。

## 計画書保存時の確認

今回の保存では、採用方式、三つの利用形態、内部専用方式の比較、クライアントと他実装の調査、標準設定、CEFの制約、段階的な実装案と完了条件を記録した。合意済みの方針と未確定事項を区別し、外部仕様の根拠を付けた。

製品コード、テスト、ビルド設定は変更していない。計画書へのリンクを一般の利用文書へ追加しない。文書のみの保存なので、製品のビルドと全体テストは実行対象に含めない。実装の完了条件は今後の実装と検証が終わった段階で照合する。

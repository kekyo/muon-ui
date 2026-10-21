# ローカルHTTP・WebSocketサーバー

CEF版のmuonアプリは`muon.server`で同じPCから利用するHTTP・WebSocketサーバーを起動できます。ブラウザー内のJavaScriptが要求を処理し、ネイティブ側が接続と認証を管理します。Node.jsの実行環境は不要です。Androidには対応しません。

MCPはLLMクライアントへアプリの情報や操作を公開するプロトコルです。このサーバーへブラウザー対応のMCP SDKを接続すると、Codexなどから実行中のアプリを操作できます。汎用HTTP APIやWebSocketだけを使うこともできます。

## HTTPサーバーの起動

`muon.server.createServer`をプラグインの許可設定へ追加し、validateモードでは`muon:server`からインポートします。simpleモードでは`window.muon.server.createServer`を使います。[プラグインの許可設定](./muon-plugins.md)

```ts
import { createServer } from "muon:server";

const server = await createServer({
  name: "automation",
  port: 8765,
  allowedOrigins: ["asset://main"],
});

const binding = await server.attach({
  fetch: async (request) => {
    const path = new URL(request.url).pathname;
    if (path === "/status" && request.method === "GET") {
      return Response.json({ running: true });
    }
    return new Response("Not Found", { status: 404 });
  },
});

const address = await server.listen();
console.log(address.url); // http://127.0.0.1:8765

// Stop handling requests while keeping the listener.
await binding.detach();
// Release the listener and every connection.
await server.close();
```

`8765`と`/status`は例です。アプリごとにポートとパスを決めてください。起動例の最後の`detach()`と`close()`は実際に終了するタイミングで呼びます。

| 作成オプション | 内容 |
| --- | --- |
| `name` | 同じページ内で重複しない空でない名前。UTF-8で128バイトまで |
| `port` | 1025〜65535の明示したポート。動的なポート割当てには未対応 |
| `host` | 省略時は`127.0.0.1`。指定できる値も`127.0.0.1`のみ |
| `bearerToken` | 任意の共有トークン。省略するとBearer認証を行わない |
| `allowedOrigins` | 許可するページのOriginの配列。省略時は空。最大64件、各2048バイトまで |

Originはページのスキーム・ホスト・ポートからなる識別子で、例えば`asset://main`や`http://localhost:5173`です。許可リストは完全一致で扱い、パス、クエリー、ワイルドカード、`null`は指定できません。

作成時には待ち受けません。`attach()`でハンドラーを登録してから`listen()`を呼びます。一つのサーバーに同時に登録できるハンドラー群は一つです。ポート競合で`listen()`が失敗した場合は、競合を解消して再試行できます。

`detach()`後の要求は503になります。再び`attach()`すれば同じ待受を利用できます。`close()`後のサーバーは再利用できず、新たに作成する必要があります。解除と停止は繰り返し呼べます。

## 接続範囲と認証

待受はIPv4のloopbackに限定されます。loopbackは同じネットワーク環境内の自分自身へ接続するアドレスです。外部クライアントには`http://127.0.0.1:<port>`を指定してください。IPv6、LAN公開、TLS、OAuth認可サーバーには未対応です。WSLやコンテナーでは同じPCでもloopbackの接続先が異なる場合があります。

| 用途 | 設定 |
| --- | --- |
| 同じPCのCLIから利用 | 通常はBearerなし。Originのない要求を受け付ける |
| CEF内のクライアントから利用 | `allowedOrigins`と、後述の外向きネットワーク設定を追加する |
| CEF内のクライアントだけに利用を認める | アプリが保持するBearerを設定する。TCPポート自体は他のローカルプロセスからも接続可能 |
| 内外の両方から利用 | 同じサーバーを利用する。Bearerを設定する場合は外部クライアントにも同じ値を渡す |

Bearerなしでは、同じPCのローカルプロセスを信頼します。別のOSユーザーからの接続も区別しません。Hostは`127.0.0.1:<port>`か`localhost:<port>`だけを受け付け、Originがある要求は許可リストを検証します。ただし、ローカルプログラムは任意のヘッダーを送れるため、この検証はプロセスの認証にはなりません。

Bearerを有効にするには、推測されにくいトークンをアプリで生成・保持し、`bearerToken`へ渡します。使える文字はRFC 6750のb64token形式で、1〜4096文字のASCIIです。例えば`crypto.randomUUID()`で生成できます。HTTPクライアントは`Authorization: Bearer <token>`を送ります。不正または未提示の要求には401と`WWW-Authenticate: Bearer`を返します。トークンをURLやログへ含めないでください。

Originのない要求をすべてCLI由来と判断することはできません。ブラウザーによる画像取得やナビゲーションにもOriginが付かない場合があります。GET／HEADへ状態変更を割り当てず、アプリの用途に応じてBearerや追加の確認を使ってください。[Fetch StandardのOrigin付加規則](https://fetch.spec.whatwg.org/#append-a-request-origin-header)

### CEF内からの接続設定

受信側の`allowedOrigins`に加え、`muon.json`の外向き通信を許可します。以下は`asset://main`から例のサーバーへ接続する設定です。

```json
{
  "network": {
    "allow": [
      "asset://main/**",
      "http://127.0.0.1:8765/**",
      "ws://127.0.0.1:8765/**"
    ],
    "localAccess": {
      "loopbackOrigins": [{ "scheme": "asset", "domain": "main" }]
    }
  }
}
```

`loopbackOrigins`は接続元ページの指定です。開発用のOriginを許可する場合も、製品アプリの設定と区別してください。[外部ネットワーク設定](./external-network.md)

CORSの事前確認であるOPTIONSはネイティブ側が検証して応答し、アプリのハンドラーを呼びません。事前確認にはBearerを要求せず、本体の要求には通常どおり認証を適用します。アプリの応答からCORS許可を上書きすることはできません。Cookieを使う`credentials: "include"`によるCORSには対応しません。

## HTTP応答とSSE

`fetch`には標準のRequestを渡し、標準のResponseを返します。JSONやバイナリーに加え、`ReadableStream<Uint8Array>`を本文にした逐次応答を利用できます。SSEは`Content-Type: text/event-stream`の応答へテキスト形式のイベントを順次送る方式です。次の例は起動例の`attach()`に代えて使います。

```ts
const encoder = new TextEncoder();
const binding = await server.attach({
  fetch: () => {
    const body = new ReadableStream<Uint8Array>({
      start: (controller) => {
        controller.enqueue(encoder.encode('data: {"ready":true}\n\n'));
        controller.close();
      },
    });
    return new Response(body, {
      headers: { "Content-Type": "text/event-stream", "Cache-Control": "no-store" },
    });
  },
});
```

長時間の配信ではアプリのイベントに応じてデータを追加し、停止時に購読も解除してください。muonは送信の進行に合わせてストリームを読み、ネイティブ側で未送信データを無制限に蓄積しません。アプリ自身のキューも`pull()`や`desiredSize`を使って制御してください。

応答開始前のハンドラー例外には汎用の500を返します。開始後のストリームエラーでは通信を打ち切ります。検出した通信障害、登録解除、停止は要求のsignalをabortし、応答ストリームのreaderを取り消します。アプリ独自の非同期処理はsignalに応じて停止してください。TCPの受信側EOFだけでは応答待ちのクライアントと区別できず、無通信のストリームで切断の検出が遅れる場合があります。

HTTPは接続ごとに一つの要求を処理し、keep-aliveやpipeliningは提供しません。Node.jsの`http.Server`やExpressとの直接互換性もありません。

## WebSocket

同じ`attach()`の`websocket`へハンドラーを登録します。`fetch`も必須です。以下は起動例の`attach()`に代え、`/echo`だけで接続を許可する例です。

```ts
await server.attach({
  fetch: () => new Response("Not Found", { status: 404 }),
  websocket: {
    accept: (request) => new URL(request.url).pathname === "/echo" ? {} : false,
    message: async (socket, data) => {
      await socket.send(data);
    },
  },
});
```

| ハンドラー | 動作 |
| --- | --- |
| `accept(request)` | `false`で403を返す。`{}`で受理する。`{ protocol: "chat" }`でクライアントが提示したprotocolを選ぶ |
| `open(socket)` | 接続成立後、最初のメッセージより前に呼ぶ。任意 |
| `message(socket, data)` | textはstring、binaryはUint8Arrayで受け取る。任意 |
| `close(socket, event)` | 終了時に一度呼ぶ。eventは`{ code, reason, wasClean }`。任意 |

接続ハンドルは`id`、`request`、`protocol`、`send(data)`、`close(code?, reason?)`を持ちます。`request`は`accept`と同じオブジェクトです。接続元のパスやOriginを参照でき、signalは切断を検出した時点でabortします。BearerのAuthorizationと専用の資格情報は、このRequestから除かれます。

`open`と`message`のPromiseは接続ごとに順番に待ちます。別の接続は並行して処理できます。`send()`は前の送信をawaitしてから呼び、完了はOSによる受入れを表します。相手の受信・処理完了は保証しません。ハンドラー例外では1011で切断します。

ハンドラーの実行中も受信を一つ進め、データがなければTCPの終了やエラーを検出してsignalをabortします。受信済みのデータがある場合は、その処理をハンドラー完了まで待つため、後続の切断検出が遅れることがあります。必要な処理期限はアプリ側で設けてください。

`close()`の既定値はcode 1000、空のreasonです。Promiseはcloseフレームの送信後に完了し、相手の返信を待ちません。5秒以内に終了しなければ通信を打ち切ります。使えるcodeは1000〜1011のうち1004・1005・1006を除く値と、3000〜4999です。reasonはUTF-8で123バイトまでです。1012〜1014には対応しません。[採用したWslayのclose code検証](https://github.com/tatsuhiro-t/wslay/blob/release-1.1.1/lib/wslay_event.c#L554-L558)

pingへのpong、分割フレーム、textのUTF-8検証に対応します。圧縮拡張は提供しません。正常なpeer closeを受けていない切断の通知はcode 1006、空のreason、`wasClean: false`になります。1005はpeer closeにcodeがなかったことを表し、どちらも送信には使えません。

### ブラウザーからのWebSocket認証

Bearerなしなら標準の`new WebSocket(url)`を使えます。Bearerありでは、任意のAuthorizationヘッダーを指定できないブラウザーAPIのために、二つの専用protocol候補を渡します。[WebSocket標準のインターフェイス](https://websockets.spec.whatwg.org/#the-websocket-interface)

```ts
const credential = "muon.bearer." + btoa(token)
  .replace(/=/g, "").replace(/\+/g, "-").replace(/\//g, "_");
const socket = new WebSocket("ws://127.0.0.1:8765/echo", [
  "muon.auth",
  credential,
]);
```

`token`はサーバーへ設定したASCIIのBearerです。ネイティブ側で検証した後、資格情報の候補をアプリへ渡すヘッダーから除きます。応答のprotocolとしても返しません。アプリがprotocolを選ばなければ`muon.auth`を選び、ほかのprotocolを選ぶ場合はクライアントからも候補に含めてください。外部クライアントは通常のAuthorizationを使えますが、二つの認証方法の併用は拒否します。

## MCPの公開

MCP専用の要求処理はアプリ側で組み立てます。以下は検証したTypeScript SDK 2.3.1とZod 4.6.7を使う例です。[SDKのWeb標準サーバー](https://ts.sdk.modelcontextprotocol.io/v2/serving/web-standard.html)

```sh
npm install @modelcontextprotocol/server@2.3.1 zod@4.6.7
```

```ts
import { createServer } from "muon:server";
import { createMcpHandler, McpServer } from "@modelcontextprotocol/server";
import * as z from "zod/v4";

let value = "initial";
const handler = createMcpHandler(() => {
  const mcp = new McpServer({ name: "muon-app", version: "1.0.0" });
  mcp.registerTool("read_state", {
    description: "Read the current application value.",
    inputSchema: z.object({}),
    annotations: { readOnlyHint: true },
  }, async () => ({ content: [{ type: "text", text: value }] }));
  mcp.registerTool("write_state", {
    description: "Set the application value.",
    inputSchema: z.object({ value: z.string().max(4096) }),
  }, async (input) => {
    value = input.value;
    return { content: [{ type: "text", text: value }] };
  });
  return mcp;
}, { responseMode: "sse" });

const server = await createServer({
  name: "mcp", port: 8765, allowedOrigins: ["asset://main"],
});
await server.attach({
  fetch: async (request) => new URL(request.url).pathname === "/mcp"
    ? await handler.fetch(request)
    : new Response("Not Found", { status: 404 }),
});
await server.listen();
```

このSDKは要求ごとにMCPサーバーを作るため、アプリの状態はfactoryの外側で保持します。終了時には`await server.close()`と`await handler.close()`を呼びます。MCPの方式はStreamable HTTPであり、汎用WebSocketへの接続ではありません。

Codexの設定例は次のとおりです。Bearerを有効にした場合は、同じ項目へ`bearer_token_env_var = "MUON_MCP_TOKEN"`を追加し、同名の環境変数へトークンを設定します。ツールの実行にはクライアント側の承認設定も適用されます。[CodexのMCP設定](https://learn.chatgpt.com/docs/extend/mcp?surface=cli)

```toml
[mcp_servers.muon_app]
url = "http://127.0.0.1:8765/mcp"
```

Claude Codeの認証なしの登録例は次のとおりです。Bearerを使う場合はHTTPの`headers`へAuthorizationを指定できます。[Claude CodeのMCP設定](https://code.claude.com/docs/en/mcp)

```sh
claude mcp add --transport http muon-app http://127.0.0.1:8765/mcp
```

CEF内のSDKクライアントとCodex CLI 0.163.0では、Bearerあり・なしの状態取得と更新を検証しました。CEF内のクライアントはMCP 2026-07-28を指定し、外部クライアントとの旧方式の交渉も確認しています。Claude Codeの実接続検証は省略しました。すべてのSDK機能や将来のバージョンとの互換性を示すものではありません。

## 生存期間と上限

サーバーは作成元ページに属します。MPAの画面遷移、リロード、frame破棄、ウインドウ・アプリの終了で停止します。SPAでページを維持する構成は継続利用できますが、アプリ全体へ生存期間を広げる指定はありません。

登録解除時には、応答開始前の要求へ503を返し、応答中のHTTP・SSEと接続済みWebSocketを打ち切ります。新しい登録へ古い要求を再配送しません。アプリの更新が完了した直後に通信が切れる場合もあるため、再試行による重複操作への対策はアプリで行ってください。

| 対象 | 上限 |
| --- | --- |
| サーバー数 | アプリ内で32 |
| 接続数・未完了要求数 | サーバーごとに32 |
| HTTP要求本文 | 1 MiB |
| HTTP要求ヘッダー | 通信上の表現で32 KiB、128個まで。要求先は8 KiBまで |
| HTTP要求全体 | 本文と付加情報を含め1 MiB + 64 KiB |
| HTTP応答本文 | 合計サイズの上限なし。逐次送信 |
| WebSocketメッセージ | 送受信とも1 MiB |
| WebSocketの未送信キュー | 1 MiB、128件まで |
| 1回の受信処理で保持するWebSocketメッセージ | 1 MiB、32件まで |

HTTP要求やアプリのハンドラーに自動の実行期限はありません。必要な期限や定期送信はアプリ側で設定してください。上限超過は要求の拒否または接続の切断になります。

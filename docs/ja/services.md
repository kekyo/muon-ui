# バックグラウンドサービス

muonのサービスは画面を閉じた後も必要な処理を続けるための実行単位です。Linux・WindowsのCEF版ではnative、QuickJS、Node.jsから実装方式を選べます。Android版ではnativeとQuickJSを利用できます。

| `runtime` | 実装と利用できる機能 | 対応バックエンド |
| --- | --- | --- |
| `native` | C/C++と公開サービスSDK | CEF・Android |
| `quickjs` | JS/TSと、アプリが明示的に許可したnativeプラグイン | CEF・Android |
| `node` | JS/TS、Node.jsの組み込みAPI、npm依存 | CEF |

サービスは`muon.json`に名前を付けて登録します。同じアプリに複数登録でき、画面から名前で取得してメソッドを呼び出します。サービスを起動してから停止するまでを一つの実行世代と呼びます。常駐設定（`autostart`）が有効なサービスは、画面が参照を解放した後も実行を続けます。サービスの終了だけで画面が閉じることはありません。

画面から文字を保存し、ページを開き直して読み戻す例を、[QuickJSでメモサービスを作る](quickjs-service-tutorial.md)で試せます。

## 最初のTSサービス

既存のmuonアプリにSDKを追加します。QuickJS・Node.jsサービスのビルドにはVite 8以降が必要です。

```bash
npm install -D muon-plugin-sdk
```

`services/echo.ts`を作ります。`interface`は呼び出せるメソッドと引数・戻り値の型、通知イベントを定義します。`start`はサービスの起動ごとに呼ばれ、メソッドの実装と終了処理を返します。

```ts
import { defineService, defineServiceInterface } from 'muon-plugin-sdk/services';

export default defineService({
  interface: defineServiceInterface({
    schemaVersion: 1,
    methods: {
      echo: { parameters: ['string'], returns: 'string', errors: [] },
    },
    events: {},
  }),
  start: () => ({
    methods: { echo: (_call, text) => text },
    stop: async () => {},
  }),
});
```

`muon.json`に追加します。`autostart`を省略すると、画面から取得されている間だけ実行します。

```json
{
  "services": {
    "echo": { "runtime": "quickjs", "entry": "services/echo.ts" }
  },
  "plugin": {
    "mode": "simple",
    "plugins": [{ "name": "internal", "allow": ["muon.services.*"] }]
  }
}
```

サービスを含むアプリをビルドし、生成先のランチャーを実行します。

```bash
npx muon build --target linux-amd64
```

Windows向けは`windows-amd64`または`windows-i686`、Android向けは`android`を指定し、Androidでは生成APKをインストールします。CEFの開発中は`npx muon run`または`npx vite`から同じサービス定義を実行できます。Viteではサービスの変更も反映します。詳しくは[開発中の起動と更新](#開発中の起動と更新)を参照してください。

画面では参照を取得してから呼び出し、使い終わったら解放します。

```ts
const services = window.muon.services;
if (!services) throw new Error('Service access is not enabled');
const handle = await services.acquire('echo');
try {
  const reply = await handle.call('echo', ['Hello']);
  console.log(reply);
} finally {
  await handle.release();
}
```

`validate`モードでは、通常のプラグイン権限設定に従って呼び出し元へ`muon.services.*`を許可し、`muon:services`から`acquire`などをimportします。サービスの定義と、画面に与える操作権限は別の設定です。

## QuickJSサービスをnpmプロジェクトにする

サービスごとに依存関係を管理するには、アプリの `muon.json` で `project` と `entry` を指定します。`project` は `muon.json` のあるディレクトリから、`entry` はそのプロジェクトからの相対パスです。`entry` だけを指定したサービスでは `muon.json` の位置が基準になります。

```json
{
  "services": {
    "worker": {
      "runtime": "quickjs",
      "project": "./services/worker",
      "entry": "src/service.ts"
    }
  }
}
```

SDKに同梱したサンプルには、依存関係、ロックファイル、型検査、Vitestの設定があります。アプリにインストールしたSDKからコピーし、サービスのディレクトリで試せます。アプリのルートに戻ると、同じ定義を `npx muon run`、`npx vite dev`、`npx muon build --target linux-amd64` で使えます。

```sh
mkdir -p services
cp -R node_modules/muon-plugin-sdk/examples/services/quickjs-project services/worker
cd services/worker
npm install
npm run typecheck
npm test
```

`tsconfig.json` の `compilerOptions` には `"strict": true`、`"lib": ["ES2022"]`、`"types": ["muon-plugin-sdk/service-globals"]` を指定します。サービスの型は `muon-plugin-sdk/services` からインポートできます。ビルダーはサービスが使うnpm依存をプロジェクトから解決してJSにまとめます。QuickJSには、JSだけで動く依存を選んでください。Node.jsの組み込みモジュールやネイティブアドオンを必要とする依存を取り込むと、ビルド時に診断が出ます。

## 起動と終了の理由を受け取る

`start(context)` は新しい実行世代ごとに呼ばれます。`context.startReason` はその世代を開始した理由です。実行中に画面が再接続すると、同じ世代が呼び出しに応答します。

| 起動理由 | 契機 |
| --- | --- |
| `acquire` | 画面からの参照取得 |
| `app-start` | アプリ起動時の常駐設定による開始。Androidでは、端末の再起動後に同じ設定で開始する場合も含む |
| `login-autostart` | PCのログイン登録からの起動 |
| `autostart-enabled` | 利用中に常駐設定を有効にした操作 |
| `development-update` | 開発中のサービス更新 |

停止が始まると `context.signal` と実行中の呼び出しの `signal` がキャンセルされます。起動に成功した世代の `stop(info)` は一度だけ呼ばれ、最初に受理した理由が `info.reason` に入ります。終了処理でファイルを保存するときは `info.signal` を渡してください。期限を取得できた場合は `info.remainingMs` で残りのミリ秒数を確認できます。

| 停止理由 | 契機 |
| --- | --- |
| `reference-released` | 最後の参照を解放した |
| `app-exit` | アプリ全体の終了を要求した |
| `session-ending` | PCのユーザーセッションの終了が確定した、またはAndroidで端末終了の通知を受け取った |
| `development-update`、`development-exit` | 開発中のサービス更新、または開発コマンドの終了 |
| `execution-timeout` | OSがサービスの実行期限を通知した |
| `host-stop` | ホストが一般の終了要求を受けた |

例えば、PCのログイン時にサービスが起動した時刻と、セッションの終了通知を受けた時刻を別々に記録できます。この例ではサービスの `io.fs.data` に `read-write` を指定します。ログイン時の起動を試すには、サービスの常駐設定を有効にしてください。

```ts
import { defineService, defineServiceInterface } from 'muon-plugin-sdk/services';

export default defineService({
  interface: defineServiceInterface({ schemaVersion: 1, methods: {}, events: {} }),
  start: async (context) => {
    if (context.startReason === 'login-autostart') {
      await context.fs.writeText('login-start.txt', new Date().toISOString(), {
        signal: context.signal,
      });
    }
    return {
      methods: {},
      stop: async (info) => {
        if (info.reason === 'session-ending') {
          await context.fs.writeText('session-end.txt', new Date().toISOString(), {
            signal: info.signal,
          });
        }
      },
    };
  },
});
```

WindowsとLinuxではセッション終了の照会が取り消されるとサービスは動作を続けます。Linuxのセッション通知には対応するデスクトップポータルが必要です。Androidでは稼働中のサービスホストが受け取った通知を扱います。クラッシュや電源断など、ホストが終了を通知できない場合には `stop` は呼ばれません。[Windowsの終了通知](https://learn.microsoft.com/en-us/windows/win32/shutdown/wm-endsession)・[Linuxのセッション通知](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.Inhibit.html)・[Androidの終了通知](https://developer.android.com/reference/android/content/Intent#ACTION_SHUTDOWN)

## サービスのファイル、HTTP、ローカルサーバー

サービスごとに、使用する操作を `muon.json` の `io` で許可します。画面の権限は引き継ぎません。次の例は専用領域への読み書き、プロジェクト内の初期値の読み取り、ひとつのオリジンへのHTTP通信、ループバックでの待受を許可します。オリジンはスキーム、ホスト名、ポートの組み合わせです。

```json
{
  "services": {
    "worker": {
      "runtime": "quickjs",
      "project": "./services/worker",
      "entry": "src/service.ts",
      "io": {
        "fs": { "data": "read-write", "assets": { "initial": "assets/initial.bin" } },
        "http": { "origins": ["https://api.example.org"] },
        "server": { "listen": true }
      }
    }
  }
}
```

- `context.fs` の `readBytes`、`writeBytes` は `Uint8Array`、`readText`、`writeText` はUTF-8テキストを扱います。ファイル名は領域内の相対パスです。既定の領域 `data` はサービス専用で、書き込みはファイルを原子的に置き換えます。置き換え前に失敗すると元の内容が残ります。`mkdir`、`list`、`rename` も使えます。ファイル1件の上限は1 MiBです。
- `context.fs.readBytes('initial', { area: 'assets' })` で同梱した初期値を読みます。`assets` のキーが論理名、値がプロジェクト内のファイルです。追加の領域は `fs.directories` に名前ごとに `{"path":"/absolute/path","access":"read"}` のように指定します。`read-write` は書き込みも許可します。共通APIは領域の外を指すパスやリンクを拒否します。

  「初期値に戻す」メソッドの例では同梱アセットを読み、サービス専用のファイルを置き換えます。`context` は `start` の引数、`call` は公開メソッドの第1引数です。

  ```ts
  const initial = await context.fs.readBytes('initial', {
    area: 'assets',
    signal: call.signal,
  });
  await context.fs.writeBytes('state.bin', initial, { signal: call.signal });
  ```

- `context.http.request({ url, method, headers, body, responseType, signal })` は `status`、小文字の名前を使う `headers`、`body` を返します。本文は既定でバイナリー、`responseType: 'text'` でUTF-8テキストになります。4xx・5xxもHTTP応答として読めます。リダイレクトは5回までで、移動先も許可済みオリジンに限ります。HTTPSではOSの信頼設定で証明書とホスト名を検証します。要求・応答の本文は各1 MiB、同時要求は1世代8件までです。`timeoutMs` はリダイレクトを含む期限で、既定値は30000、指定範囲は1から120000です。

  このメソッドはGETでステータスを取得し、200なら本文を文字列で返します。メソッドの宣言で `errors: ['http-status']` を指定すると、それ以外のステータスを画面側でエラーとして受け取れます。試すときは `url` を実際のAPIのURLに変更し、そのオリジンを `io.http.origins` に指定してください。画面が呼び出しを取り消すと、同じ `signal` を渡したHTTP要求もキャンセルされます。

  ```ts
  const reply = await context.http.request({
    url: 'https://api.example.org/status',
    responseType: 'text',
    timeoutMs: 5000,
    signal: call.signal,
  });
  if (reply.status !== 200) {
    throw Object.assign(new Error('HTTP ' + reply.status), { code: 'http-status' });
  }
  return reply.body;
  ```

- `context.server.listen({ name, port, fetch, websocket, allowedOrigins, bearerToken })` は `127.0.0.1` のポート1025から65535で待ち受けます。`fetch` は要求の `method`、`url`、`headers`、`body` を受け取り、ステータス、ヘッダー、テキストまたはバイナリー本文を返します。WebSocketを使う場合は `websocket.accept` で接続を受け付け、`message` で文字列やバイナリーを受け取ります。ホストがHostとOriginを検証し、`bearerToken` を指定した場合は認証も行います。戻り値の `close()` で待受と接続を閉じられます。常駐サービスの待受は画面を閉じた後も続き、その世代の停止時に閉じます。

  次は `start` の本体です。HTTPの本文やWebSocketのメッセージをそのまま送り返します。サービスを取得したまま `curl --data hello http://127.0.0.1:43123/` を実行すると、応答は `hello` です。ブラウザーから接続する場合は、接続元のページのオリジンを `allowedOrigins` に指定してください。

  ```ts
  const listener = await context.server.listen({
    name: 'echo',
    port: 43123,
    allowedOrigins: ['https://app.example.org'],
    fetch: (request) => ({ body: request.body }),
    websocket: {
      accept: () => ({}),
      message: async (socket, data) => await socket.send(data),
    },
  });
  return { methods: {}, stop: async () => await listener.close() };
  ```

Bearerを設定した待受にHTTPで接続するときは、Authorizationヘッダーに `Bearer <トークン>` を指定します。ブラウザーのWebSocketクライアントでは、サブプロトコルに `muon.auth` と、トークンをBase64URLに変換して `muon.bearer.` を先頭に付けた文字列を指定します。[WebSocketの認証例](local-server.md#ブラウザーからのwebsocket認証)

共通APIの失敗は `isServiceIoError(error)` で判別できます。`error.code` は許可外なら `permission-denied`、証明書の検証に失敗したら `tls-error`、本文が大きすぎると `size-limit`、時間切れなら `timeout`、キャンセルなら `cancelled` です。Node.jsの標準ライブラリやnativeコードを直接呼ぶ操作には、プロセスのOS権限が適用されます。Androidのビルダーは通信を使うサービスに `INTERNET` 権限を付け、平文の送信を許可したホストをネットワークセキュリティ設定に反映します。[Androidの通信権限](https://developer.android.com/develop/connectivity/network-ops/connecting)・[Androidのネットワークセキュリティ設定](https://developer.android.com/privacy-and-security/security-config)

C ABI v3では `muon_service_context.io->request` で同じ操作を開始します。例えば `fs.write` には領域とパスをJSONで、内容をバイナリーバッファーで渡します。完了コールバックはサービスのディスパッチャーで一度呼ばれます。結果のバッファーはコールバックの間だけ有効です。C++20では `muon_service_io_cpp.h` の `muon::request_service_io` が所有権を持つ結果を返し、`cardio` のコルーチン内で待機できます。SDKの `examples/services` に、終了時の保存、外向きのHTTP、ローカルサーバーをそれぞれCとTSで実装したサンプルがあります。

## 参照と自動起動

サービスは、画面が取得した参照か、ホストが持つ常駐参照がある間、実行を必要とします。常駐参照はサービスごとに最大1つです。

| 操作 | サービスへの影響 |
| --- | --- |
| `acquire(id)` | その呼び出し専用の参照を取得し、必要なら起動完了を待つ |
| `handle.release()` | その参照だけを解放する。繰り返しても他の参照を減らさない |
| ページ破棄・UIとの接続切断 | そのページの参照と起動待ちを回収する |
| `setAutostart(id, true)` | ONを保存し、常駐参照を1つ持つ |
| `setAutostart(id, false)` | OFFを保存し、常駐参照だけを解放する |
| 最後の参照の解放 | サービスの終了処理を開始する |

`muon.json`の`autostart: true`は初期設定です。利用者が変更した値を優先し、次回の起動でも維持します。同じ値を繰り返し設定しても参照数は増減しません。OFFでも別のページが参照を持っていれば動き続け、その後の明示的な取得も可能です。

`setAutostart()`の完了は、設定の保存と常駐参照の変更が済んだことを表します。起動や停止の完了を待つものではありません。`release()`も参照の解放までを待ちます。画面のスイッチと実行状態は、次のように別々に表示できます。

```ts
const watcher = await services.watchState('echo', state => {
  checkbox.checked = state.autostart;
  status.textContent = `${state.state}: ${state.error}`;
});
await services.setAutostart('echo', checkbox.checked);
// When this panel is no longer used:
await watcher.close();
```

`getState(id)`と`watchState(id, listener)`は参照を増やしません。状態には`state`、`autostart`、`resident`、`clientReferences`、`generation`、`revision`、`error`があります。`state`は`stopped`、`starting`、`running`、`stopping`、`failed`のいずれかです。`generation`は実行ごとの識別子、`revision`は保存済み設定の改訂番号で、どちらも文字列として扱います。監視が遅れた場合は最新状態を通知するため、全遷移の履歴にはなりません。

サービスが終了すると、その世代のハンドルは無効になります。再利用には`acquire()`で新たなハンドルを取得します。起動失敗後の自動再試行は繰り返しません。状態の理由を確認してから再取得してください。JSの変数へハンドルを代入しても参照は増えず、GCだけを解放の契機にはしません。

## メソッド、コールバック、イベント

サービスの型定義を画面と共有する場合は、実装から独立したTSファイルに`defineServiceInterface()`の宣言を置きます。画面では`bindServiceClient(interfaceDefinition, handle)`を使うと、実行時の型の照合と、引数・戻り値・イベントの静的な型付けを行えます。元のハンドルの解放は引き続き呼び出し元が担当します。

整数の`int64`と`uint64`には`bigint`、`bytes`には`Uint8Array`を使用します。文字列、真偽値、32bit整数、浮動小数と、これらの型の配列も扱えます。型定義と合わない値は拒否されます。メソッド固有のエラーコードは`errors`に列挙し、JS/TSの実装では`code`と`message`を持つエラーをthrowします。

メソッドは`call(method, args, { signal })`でキャンセルできます。JS/TSのメソッドには最初の引数として呼び出し情報が渡り、その`signal`から取り消しを確認できます。キャンセルに対応するI/O処理を実装し、停止時には確保した資源を解放してください。

公開メソッドの `returns` には、`Promise` が解決したときの値の型を書きます。最初の `echo` サービスの `start` を次のように変更し、`io.fs.data` に `read-write` を指定してください。実装は `Promise<string>` を返すので、インターフェイスには `returns: 'string'` と宣言します。

```ts
start: (context) => ({
  methods: {
    echo: async (call, text): Promise<string> => {
      await context.fs.writeText('last-echo.txt', text, { signal: call.signal });
      return text;
    },
  },
  stop: () => {},
}),
```

画面で `await handle.call('echo', ['Hello'])` が `'Hello'` を返すのは書き込みが完了した後です。書き込みが失敗した場合は、画面側の `try` / `catch` でエラーを受け取れます。

引数の関数は、親メソッドが完了するまでのコールバックです。親メソッドの完了後やキャンセル後に保持して使うことはできません。継続的な通知にはイベントを使います。サービス側は`start`に渡る`context.emit(name, value)`で通知し、画面側は`handle.subscribe(name, listener)`で購読します。

```ts
const subscription = await handle.subscribe('progress', async value => {
  console.log(value);
});
// When the listener is no longer needed:
await subscription.close();
```

イベントは同じ購読内で順番に配送します。購読は追加の生存参照を持ちません。ハンドルの解放・接続切断・通知の滞留などで購読は終了し、`subscription.closed`が終了理由または`undefined`で解決します。

## C/C++で実装する

公開SDKからサービス用の雛形を作れます。

```bash
npx muon-plugin init native/echo --kind service --name echo --language c
npx muon-plugin build native/echo/muon-plugin.build.json --target linux-amd64 --out artifacts/echo
```

生成した`service.c`または`service.cpp`は、公開ヘッダー`muon_service_api.h`を使います。`muon_get_service_factory()`からファクトリーとインターフェイスを公開し、起動、メソッド呼び出し、キャンセル、停止の処理を実装します。ABIのコメントには引数と完了コールバックの所有権を記載しています。非同期処理が終わる前にインスタンスやコールバックの資源を解放しないでください。

同じソースを必要なターゲット向けにビルドして、成果物の`muon-plugin.json`を登録します。サービス用成果物はページ用プラグインの成果物と区別されます。

```json
{
  "services": {
    "echo": {
      "runtime": "native",
      "artifact": "artifacts/echo/muon-plugin.json",
      "autostart": true,
      "config": { "label": "Background echo" }
    }
  }
}
```

OS固有の処理は各ターゲットで実装する必要があります。サービスAPI自体はCEFやJNIに依存せず、同じC99・C++20のソースを利用できます。SDK内の`examples/services/native-echo.c`はメソッドを公開する例です。ビルド済みnative成果物を使うアプリの組み立てには、nativeコンパイラは不要です。

アプリからソースもビルドする場合は、`artifact`の代わりに`source`へSDKのビルド設定を指定します。

```json
{
  "services": {
    "echo": {
      "runtime": "native",
      "source": "native/echo/muon-plugin.build.json"
    }
  }
}
```

`source`と`artifact`は同時に指定できません。`source`を使う場合は、必要に応じてクロスコンパイル用の`sysroot`も指定できます。これらのパスは`muon.json`のディレクトリを基準に解決します。SDKと対象ターゲットのビルド環境を用意してください。ビルド方式と依存ライブラリの宣言は[SDKガイド](muon-plugin-sdk.md#makecmake独自ビルド)を参照してください。

## Node.jsで実装する

Node.jsサービスはQuickJSと同じ`defineService()`と画面用APIを使います。サービスごとにNode.js子プロセスを持ち、画面が終了しても参照の規約に従って処理を継続します。

サービスのプロジェクトには`package.json`を置き、利用するnpmパッケージをインストールします。例えば`services/worker/package.json`を次のように作成できます。

```json
{
  "name": "worker",
  "private": true,
  "type": "module",
  "engines": { "node": ">=22.12" }
}
```

```bash
npm install --prefix services/worker muon-plugin-sdk
```

SDK内の`examples/services/node-files.ts`と`node-greeting.txt`をこのプロジェクトへコピーし、次の設定を`muon.json`へ追加します。このサンプルはNode.jsのファイルAPIでアセットを読み、`greet`メソッドを公開します。

```json
{
  "services": {
    "worker": {
      "runtime": "node",
      "project": "services/worker",
      "entry": "node-files.ts",
      "autostart": true
    }
  }
}
```

`project`は`muon.json`のディレクトリを基準に、`entry`はそのプロジェクトを基準に解決します。エントリーとローカルのソースはプロジェクト内に置きます。`autostart`と文字列の`config`は他の実装方式と共通です。Node.jsには`artifact`、`source`、`plugins`を指定しません。Androidを含むビルドで`runtime: "node"`を指定すると、該当サービスを示してビルドに失敗します。

TSはESモジュールへ変換され、相対ディレクトリ構成とアセットを保ったまま配布されます。`import.meta.url`を基準にしたファイル読み込みを使えます。実行時のnpm依存と間接依存も配置されるため、対象OSで使用できる依存物を事前にインストールしてください。必要なパッケージがない場合は準備に失敗します。muonのビルド処理から`npm install`やパッケージのスクリプトを実行することはありません。

muonは、各サービスの`package.json`にある`engines.node`とmuonの対応範囲をすべて満たすNode.jsを準備します。条件が両立しない場合は、該当するプロジェクトとバージョン条件をエラーに示します。

通常の停止では、実行中のメソッドとコールバックの後片付け、サービスの非同期`stop()`の完了を待ちます。サンプルは`start`に渡される`context.dataDirectory`へ最後の名前を保存します。このディレクトリはサービス専用の永続データ領域です。起動・停止の期限は各10秒で、期限超過、子プロセスの異常終了、通信断は失敗として報告されます。

## QuickJSからnativeコードを呼ぶ

QuickJSサービスの`plugins`へ、利用するプラグインと関数の許可を明示します。プラグインは[公開SDK](muon-plugin-sdk.md)で作成する通常のC/C++プラグインです。

```json
{
  "services": {
    "worker": {
      "runtime": "quickjs",
      "entry": "services/worker.ts",
      "plugins": {
        "counter": {
          "native": { "manifest": "artifacts/counter/muon-plugin.json" },
          "allow": ["muon.counter.increment"]
        }
      }
    }
  }
}
```

このサービスでは`import { increment } from 'muon:counter'`を使用でき、`await increment(41)`で呼び出せます。`native`には通常のプラグインと同じ`source`、`manifest`、`package`のいずれかを指定できます。TSのモジュール型宣言はプラグイン作者が配布します。SDK内の`examples/services/quickjs-native.ts`と`counter.d.ts`が利用例です。

画面用のプラグイン権限は継承しません。同じプラグインを複数サービスで使う場合は成果物と設定を共通にし、関数の許可はサービスごとに絞れます。raw pointer型は扱えません。サービス通信の文字列はNULを含められますが、プラグインABIのC文字列にはNULを含められません。native側の長期コールバックや関数プロキシは、停止時に解放する必要があります。

## QuickJSの実行環境

サービスごとに独立したJS実行環境を持ちます。標準のJS機能に加えて`setTimeout`、`clearTimeout`、`console`を使用できます。型宣言が必要なら`import type {} from 'muon-plugin-sdk/service-globals'`を追加します。

DOM、`window.muon`、Node.jsの組み込みモジュール、`fetch`、`setInterval`は提供しません。ファイルと通信には、各サービスに許可した共通APIを使えます。ビルダーはサービスと依存モジュールを単一JSへまとめ、宣言したアセットをサービスと一緒に配布します。

SDKの `quickjs-project` ではnpmパッケージ `base64-js` でバイナリーをBase64文字列に変換しています。自分のサービスで使う場合は、サービスのプロジェクトで `npm install base64-js` を実行してください。[base64-jsのAPI](https://github.com/beatgammit/base64-js#methods)

```ts
import { fromByteArray } from 'base64-js';

const bytes = Uint8Array.of(0, 128, 255);
console.log(fromByteArray(bytes)); // AID/
```

ファイルを読む例、HTTP要求を送る例、HTTPとWebSocketで応答する例は、[サービスのファイル、HTTP、ローカルサーバー](#サービスのファイルhttpローカルサーバー)にあります。

現在の上限は、JSバンドル8 MiB、JSヒープ64 MiB、スタック1 MiB、1回のJS実行2秒、起動・停止各10秒です。非同期I/Oの待ち時間と、CPUを占有するJS実行時間は区別します。上限超過や実行エラーはサービスの失敗として報告します。nativeコードの停止不能やプロセスの異常終了は、同じプロセスで動く他サービスにも影響します。

## 開発中の起動と更新

CEFでは配布ビルドと同じ`services`設定を使い、`npx muon run`でローカルの画面とサービスを起動できます。muonのViteプラグインを使うプロジェクトでは`npx vite`で画面とサービスを起動します。サービスの準備に失敗した場合はエラーを表示し、画面の起動も失敗させます。

Viteはサービスのソース、アセット、設定、npm依存、nativeのビルド入力や成果物の変更を監視します。新しい構成を準備できたらサービス全体を再起動し、ページを再読み込みします。ページの初期化処理でサービスを取得し直してください。変更対象以外のサービスも再起動し、古いハンドルは無効になります。`muon run`にはこの変更監視はありません。

HMRはページ全体を再読み込みせずに、変更したモジュールを更新するViteの機能です。画面だけのHMRではサービスホストを再起動しません。ただし、ページの破棄で非常駐サービスの最後の参照がなくなれば、通常どおり停止します。画面を閉じても常駐サービスがあれば開発コマンドは待機を続けます。`muon.browser.shutdown()`、Viteの終了、開発コマンドの中断はサービスを含む開発環境全体を終了します。

ビルド失敗中は旧サービスが動き続け、次の編集で更新を再試行します。起動や停止に失敗した場合も診断を表示し、修正後の編集で再試行できます。この場合は旧ハンドルを再利用できません。連続した編集は順に処理し、更新や終了で使用しなくなったプロセスと配置を回収します。

開発用のプロファイルとサービスのデータは配布アプリから分離します。サービスやUIの再起動では自動起動設定を維持しますが、開発コマンドを終了するとそのセッションの設定とデータを削除します。開発中にOSのログイン起動を登録することはありません。Androidではnative・QuickJSのビルドを利用でき、端末への開発起動やViteのライブプレビューには対応していません。

## PCでの画面とログイン起動

PCではサービスをUIとは別のプロセスで動かします。すべての画面を閉じても常駐参照があれば継続し、アプリを再表示して同じ実行世代へ接続できます。`muon.browser.close()`は画面を閉じ、`muon.browser.shutdown()`はサービスを含むアプリ全体を終了します。明示終了では自動起動の設定を消さず、次回の正規の起動に適用します。

`muon.environments.getAutostart()`と`setAutostart()`はログイン時のUIの自動表示を扱います。サービス側は`muon.services.setAutostart()`で個別に設定します。UIの自動表示がOFFでも、いずれかのサービスの自動起動がONならログイン起動の登録を維持します。ユーザーセッションの終了後まで動かすOSデーモンやWindows Serviceではありません。

## Androidでの動作条件

AndroidではサービスをUIとは別のプロセスで動かし、foreground serviceとして通知を表示します。Android向けにビルドする各サービスには、用途と通知を指定します。たとえば接続機器との通信を実装する場合は次の形です。

```json
{
  "android": {
    "foregroundServiceType": "connectedDevice",
    "notification": {
      "channelId": "device-work",
      "title": "Device connection",
      "text": "Communicating with your device"
    }
  }
}
```

この`android`を`services.<id>`に追加します。実際の機能に合うtypeを選び、そのtypeが要求する権限・実行条件を満たしてください。`specialUse`には用途を示す`specialUseSubtype`も必要です。宣言できるtypeと条件は[Androidのservice type一覧](https://developer.android.com/develop/background-work/services/fgs/service-types)を参照してください。

実行中のサービスにも、種類に応じてAndroidの実行時間制限が適用されます。[フォアグラウンドサービスのタイムアウト](https://developer.android.com/develop/background-work/services/fgs/timeout)

画面を閉じる操作と、OSによるアプリ全体の停止は異なります。Androidの「アクティブなアプリ」から停止するとアプリ全体が終了し、終了コールバックも保証されません。muonは参照数だけを理由にOSの停止を回避しません。[ユーザーによる停止の仕様](https://developer.android.com/develop/background-work/services/fgs/handle-user-stopping)

Android API 30以降では、サービスホストの起動時にOSの終了履歴を読み、`getState()`と`watchState()`の`lastHostExit`で返します。理由を表す`reason`、OSの理由コード`reasonCode`、終了時刻の`timestampMs`などを取得できます。時刻はUnix epochからのミリ秒を表す10進文字列です。OSから記録を取得できない場合、API 29以前、PCでは`null`になります。[ApplicationExitInfoの仕様](https://developer.android.com/reference/android/app/ApplicationExitInfo)

この情報はサービスホスト全体の過去の終了を表します。同じホストで動くサービスは同じ記録を返し、個々のサービスがその時点で動いていたとは限りません。現在の実行状態や失敗理由は、引き続き`state`と`error`で確認してください。

```ts
const state = await window.muon.services!.getState('echo');
if (state.lastHostExit) {
  console.log(state.lastHostExit.reason, state.lastHostExit.timestampMs);
}
```

### 再起動後の自動起動

インストール後に一度アプリを開くと、端末の再起動後、最初のロック解除でサービスを自動起動できます。muonは `services.<id>.autostart`（省略時はOFF）を初期値に使い、`muon.services.setAutostart(id, enabled)` で保存したONまたはOFFを優先します。全件がOFFならサービスは起動しません。ONのサービスは、Androidの起動条件を満たせばアプリの画面を開く前に動き始めます。その後で画面を開いたり、起動通知が再び届いたりしても、起動済みのサービスは同じ実行世代で動き続けます。[起動通知の仕様](https://developer.android.com/reference/android/content/Intent#ACTION_BOOT_COMPLETED)・[インストール後の初回起動](https://developer.android.com/develop/background-work/services/alarms)

muonがビルドするAPKでは、次の種類を一つでも指定したサービスの開始を、アプリの画面が表示されるまで保留します。[Androidの種類別起動制限](https://developer.android.com/about/versions/15/changes/foreground-service-types)

| 端末のOS | 起動通知から開始できない種類 |
| --- | --- |
| Android 14以降 | `microphone` |
| Android 15以降で追加 | `camera`、`dataSync`、`mediaPlayback`、`mediaProjection`、`phoneCall` |

ほかの種類でも、位置情報などに必要な権限が足りず、Androidが開始を拒否する場合があります。muonはそのサービスの設定をONのまま保持し、理由を `getState(id)` の `error` に記録します。他のサービスはそれぞれの条件に従って起動します。保留中のサービスは画面を開いたり画面に戻ったりしたときに再試行するので、必要な権限を利用者が許可すれば開始できます。[バックグラウンドからの起動条件](https://developer.android.com/develop/background-work/services/fgs/restrictions-bg-start)・[ビルド時に追加される権限と実行時の許可](android.md#ビルド時に追加される権限と実行時の許可)

利用者がアプリを強制停止した場合は、再びアプリを開く必要があります。Android 13以降では、利用者がアプリのバックグラウンドでのバッテリー使用を「制限」に設定すると、Androidは別の理由でアプリが起動するまで起動通知の配信を保留します。[強制停止を解除したときの配信](https://developer.android.com/reference/android/content/Intent#ACTION_BOOT_COMPLETED)・[バッテリー使用の制限](https://developer.android.com/about/versions/13/behavior-changes-13#battery-resource-utilization)

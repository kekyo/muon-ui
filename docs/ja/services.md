# バックグラウンドサービス

muonのサービスは画面を閉じた後も必要な処理を続けるための実行単位です。Linux・WindowsのCEF版とAndroid版で、C/C++のサービスとQuickJSによるJS/TSのサービスを利用できます。QuickJSからは、アプリが許可したC/C++プラグインを呼び出せます。Node.jsをサービスとして実行する方式は未対応です。

サービスは`muon.json`に名前を付けて登録します。同じアプリに複数登録でき、画面から名前で取得してメソッドを呼び出します。サービスの終了だけで画面が閉じることはありません。

## 最初のTSサービス

既存のmuonアプリにSDKを追加します。QuickJSサービスのビルドにはVite 8以降が必要です。

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

Windows向けは`windows-amd64`または`windows-i686`、Android向けは`android`を指定し、Androidでは生成APKをインストールします。現在の`muon run`と`vite dev`はサービス定義を組み立てません。サービスを変更した後は、アプリを再ビルドして生成物から実行してください。

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

メソッドは`call(method, args, { signal })`でキャンセルできます。QuickJSのメソッドには最初の引数として呼び出し情報が渡り、その`signal`から取り消しを確認できます。キャンセルに対応するI/O処理を実装し、停止時には確保した資源を解放してください。

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

DOM、`window.muon`、Node.jsの組み込みモジュール、`fetch`、`setInterval`は提供しません。ファイルやネットワークのI/Oには明示的に許可したnativeプラグインを使用します。ビルダーはサービスと依存モジュールを単一JSへまとめ、未許可の外部モジュールや別アセットへの依存を拒否します。

現在の上限は、JSバンドル8 MiB、JSヒープ64 MiB、スタック1 MiB、1回のJS実行2秒、起動・停止各10秒です。非同期I/Oの待ち時間と、CPUを占有するJS実行時間は区別します。上限超過や実行エラーはサービスの失敗として報告します。nativeコードの停止不能やプロセスの異常終了は、同じプロセスで動く他サービスにも影響します。

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

自動起動ONは、OSが常時実行を保証する意味ではありません。バックグラウンドからの開始、使用中権限、実行時間にはOSの制限があります。開始できない場合は保存済みの設定を保持したまま、実行状態と理由を返します。起動条件は[バックグラウンド開始の制限](https://developer.android.com/develop/background-work/services/fgs/restrictions-bg-start)、時間制限は[foreground serviceのタイムアウト](https://developer.android.com/develop/background-work/services/fgs/timeout)を参照してください。

画面を閉じる操作と、OSによるアプリ全体の停止は異なります。Androidの「アクティブなアプリ」から停止するとアプリ全体が終了し、終了コールバックも保証されません。muonは参照数だけを理由にOSの停止を回避しません。[ユーザーによる停止の仕様](https://developer.android.com/develop/background-work/services/fgs/handle-user-stopping)

このバージョンでは端末の起動完了からサービスを開始する登録を追加しません。アプリの正規の起動時に保存済み設定を評価します。Androidアプリのビルド、署名、配布形式については[Androidガイド](android.md)を参照してください。

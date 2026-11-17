# バックグラウンドサービス

muonのサービスは画面を閉じた後も必要な処理を続けるための実行単位です。Linux・WindowsのCEF版ではnative、QuickJS、Node.jsから実装方式を選べます。Android版ではnativeとQuickJSを利用できます。

| `runtime` | 実装と利用できる機能 | 対応バックエンド |
| --- | --- | --- |
| `native` | C/C++と公開サービスSDK | CEF・Android |
| `quickjs` | JS/TSと、アプリが明示的に許可したnativeプラグイン | CEF・Android |
| `node` | JS/TS、Node.jsの組み込みAPI、npm依存 | CEF |

サービスは`muon.json`に名前を付けて登録します。同じアプリに複数登録でき、画面から名前で取得してメソッドを呼び出します。サービスの終了だけで画面が閉じることはありません。

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

Node.jsの実行ファイルはmuonが準備します。サービスだけを使う場合に画面用の`node.project`設定は不要です。画面用Node.jsと各サービスの`engines.node`、muonの対応範囲をすべて満たすランタイムを使用し、条件が両立しない場合はプロジェクトと条件を示して準備を失敗させます。

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

DOM、`window.muon`、Node.jsの組み込みモジュール、`fetch`、`setInterval`は提供しません。ファイルやネットワークのI/Oには明示的に許可したnativeプラグインを使用します。ビルダーはサービスと依存モジュールを単一JSへまとめ、未許可の外部モジュールや別アセットへの依存を拒否します。

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

自動起動ONは、OSが常時実行を保証する意味ではありません。バックグラウンドからの開始、使用中権限、実行時間にはOSの制限があります。開始できない場合は保存済みの設定を保持したまま、実行状態と理由を返します。起動条件は[バックグラウンド開始の制限](https://developer.android.com/develop/background-work/services/fgs/restrictions-bg-start)、時間制限は[foreground serviceのタイムアウト](https://developer.android.com/develop/background-work/services/fgs/timeout)を参照してください。

画面を閉じる操作と、OSによるアプリ全体の停止は異なります。Androidの「アクティブなアプリ」から停止するとアプリ全体が終了し、終了コールバックも保証されません。muonは参照数だけを理由にOSの停止を回避しません。[ユーザーによる停止の仕様](https://developer.android.com/develop/background-work/services/fgs/handle-user-stopping)

Android API 30以降では、サービスホストの起動時にOSの終了履歴を読み、`getState()`と`watchState()`の`lastHostExit`で返します。理由を表す`reason`、OSの理由コード`reasonCode`、終了時刻の`timestampMs`などを取得できます。時刻はUnix epochからのミリ秒を表す10進文字列です。OSから記録を取得できない場合、API 29以前、PCでは`null`になります。[ApplicationExitInfoの仕様](https://developer.android.com/reference/android/app/ApplicationExitInfo)

この情報はサービスホスト全体の過去の終了を表します。同じホストで動くサービスは同じ記録を返し、個々のサービスがその時点で動いていたとは限りません。現在の実行状態や失敗理由は、引き続き`state`と`error`で確認してください。

```ts
const state = await window.muon.services!.getState('echo');
if (state.lastHostExit) {
  console.log(state.lastHostExit.reason, state.lastHostExit.timestampMs);
}
```

このバージョンでは端末の起動完了からサービスを開始する登録を追加しません。アプリの正規の起動時に保存済み設定を評価します。Androidアプリのビルド、署名、配布形式については[Androidガイド](android.md)を参照してください。

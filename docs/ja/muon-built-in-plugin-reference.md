# muon内蔵プラグインリファレンス

この章では、プラグイン名前空間と関数パスを分かりやすく示すため、`window.muon.*` 形式でAPIを表記しています。
これは `plugin.mode: "simple"` で実際に公開されるオブジェクト階層でもあります。
既定の `validate` モードでは、対応するvirtual moduleから関数をインポートして使用します。

Android WebViewバックエンドは、この章にあるデスクトップAPIの一部だけを公開します。Androidで利用できる関数とプラットフォーム固有の挙動は、[Android WebViewバックエンドの制約](./limitation.md#android-webviewバックエンド)と[Androidガイド](./android.md#設定とmuon-api)を参照してください。公開されない関数をno-opへ置き換えることはありません。

例えば `window.muon.executor.spawn` は、`plugin.plugins[].imports` またはVite `pluginAccess.plugins[].imports` で `muon.executor.spawn` を許可したうえで、
`muon:executor` から `spawn` をインポートします:

```ts
import { spawn } from "muon:executor";
```

## 通知とAndroid固有機能

Android・Windows・Linuxのローカル通知は`muon.notifications`で操作します。Androidのシステムバー、通知権限・チャンネル、OS設定画面は`muon.android`以下にあります。CEF版には`muon.android`を公開しません。

| simpleモードの名前空間 | validateモードのimport元 | 用途 |
| --- | --- | --- |
| `window.muon.notifications` | `muon:notifications` | 通知の投稿、取り消し、操作イベント |
| `window.muon.android.systemBars` | `muon:android.systemBars` | システムバーと安全領域 |
| `window.muon.android.notifications` | `muon:android.notifications` | Androidの通知権限とチャンネル |
| `window.muon.android.settings` | `muon:android.settings` | アプリに関するOS設定画面 |

通知APIは`notifications.enabled: true`でビルドしたアプリだけに含まれます。関数の許可も必要です。validateモードでは、例えば`muon.notifications.post`を`imports[].allow`へ指定します。通知を無効にするビルドでは、通知関数のimportと許可設定も削除してください。[通知のビルド設定](./muon-json-reference.md#notificationsキー)

### 共通通知API

| 関数 | 戻り値 | 動作 |
| --- | --- | --- |
| `post(options)` | `Promise<void>` | 同じアプリ内の`id`を指定して投稿・更新する |
| `cancel(id)` | `Promise<void>` | 指定した通知を取り消す。存在しないIDも成功する |
| `getCapabilities()` | `Promise<MuonNotificationCapabilities>` | 環境が対応する機能と理由を取得する |
| `getStatus(category?)` | `Promise<MuonNotificationStatus>` | 現在の投稿可否を取得する |
| `watchEvents(listener)` | `Promise<MuonNotificationWatcher>` | このページで新しく受け取った操作を購読する |
| `getPendingEvents()` | `Promise<readonly MuonNotificationEvent[]>` | 未受領イベントを古い順に取得する。取得だけでは削除しない |
| `acknowledgeEvent(eventId)` | `Promise<void>` | 処理済みイベントの受領を確認し、未受領一覧から削除する |

`post()`の必須項目は文字列の`id`、`title`、`body`です。`category`は用途を表す識別子で、省略時には`notifications.defaultCategory`を使用します。Androidでは登録済みチャンネルと対応付けます。CEFでは任意の用途分類を指定でき、Android用のチャンネル設定は不要です。

```ts
import { getCapabilities, post } from "muon:notifications";

const capabilities = await getCapabilities();
await post({
  id: "order-42",
  title: "Order ready",
  body: "Your order is ready to collect.",
  category: "orders",
  data: { orderId: 42 },
  ...(capabilities.actions.state === "supported"
    ? { actions: [{ id: "view", title: "View order" }] }
    : {}),
});
```

`id`と`category`は最大128、`title`は最大1024、`body`は最大4096 UTF-16コード単位です。`body`は空文字でも構いません。`data`にはJSONで表せる値を渡せます。上限はUTF-8で16 KiB、深さ64で、省略時のイベントデータは`null`です。関数や循環参照は保存できません。`actions`は最大3件で、各`id`と`title`は空でない最大128コード単位の文字列です。操作IDは通知内で重複できません。

`post()`は暗黙に権限を要求しません。無効化を検出した場合、Androidで未登録の分類を指定した場合、非対応の追加ボタンなどを指定した場合はPromiseを拒否します。成功はOSへの投稿要求が完了したことを表し、バナーの表示やユーザーの閲覧を保証しません。OSの通知抑止や保持方針も適用されます。[Androidの通知権限](https://developer.android.com/develop/ui/views/notifications/notification-permission)、[Linuxの通知仕様](https://specifications.freedesktop.org/notification/latest/)

`getCapabilities()`の各項目は`{ state, reason }`です。`state`は`supported`、`unsupported`、`unknown`のいずれかで、説明が不要な`reason`は`null`です。

| 項目 | 確認する機能 |
| --- | --- |
| `interaction` | 実行中のアプリへの通知操作の配送 |
| `launch` | 終了済みアプリの起動と操作の配送 |
| `persistence` | 通常のプロセス終了後の通知保持 |
| `actions` | 名前付き追加ボタン |
| `appStatus` | アプリ全体の投稿可否の取得 |
| `categoryStatus` | 用途分類別のOS設定の取得 |

機能への対応と現在の投稿可否は別です。`getStatus()`は`app`と`category`を返し、投稿可否の`state`は`enabled`、`disabled`、`unknown`です。引数を省略した場合の`category`は`null`で、既定分類を暗黙には調べません。分類を指定した場合はその`id`も返します。Linuxでは全体・分類別、Windowsでは分類別の状態を取得できず、`unknown`になる場合があります。`unknown`を許可済みと解釈しないでください。

### 通知イベントと受領確認

イベントは`{ eventId, id, type, actionId, data }`です。通常の通知クリックは`type: "activate"`と`actionId: null`、追加ボタンは`type: "action"`と指定した操作IDを返します。`"default"`という操作IDも通常クリックと区別します。`data`は操作された投稿時点の値です。

購読を開始してから未受領一覧を読み、`eventId`で重複を除いて処理してください。コールバックが正常終了しても自動では受領確認しません。アプリの処理が完了した後に`acknowledgeEvent(eventId)`を呼びます。アプリの処理と受領確認の間に終了すると再配送されるため、注文の確定など重複実行できない処理には、保存した`eventId`を使ったアプリ側の重複対策も必要です。

`watchEvents()`は既存イベントを再配送せず、新しい操作だけを通知します。リスナーは購読ごとに順番に呼ばれます。戻り値の`close()`で解除でき、ページ破棄でも解除されます。解除しても通知と未受領イベントは残ります。複数ページに同じイベントが届く場合があり、受領確認はアプリ全体へ作用します。すでに開始したコールバックは取り消しません。

保存には上限があります。未操作の投稿と未受領イベントは合計256枠で、満杯の場合は新しい投稿を拒否します。未操作の投稿は投稿後30日、未受領イベントは受信後30日で期限切れになります。OSやユーザーが通知を消しただけでは枠を解放しないため、不要になったIDは`cancel()`で取り消してください。同じIDの更新は未操作の旧投稿を無効にし、受信済みイベントは保持します。古い投稿への遅れた操作から新しいイベントを作ることはありません。

Linuxでは更新中に新旧通知が一時的に並ぶ場合があります。OSへの投稿とローカル保存は一つの原子的な処理ではありません。AndroidのAPK更新ではOSが既存通知を削除する場合があり、自動再投稿はしません。同じアプリIDとデータを維持する更新では、保存済みの未受領イベントを取得できます。アンインストールやデータ消去は保持の対象外です。[Androidのパッケージ変更と通知削除](https://android.googlesource.com/platform/frameworks/base/+/1cdfff555f4a21f71ccc978290e2e212e2f8b168/services/core/java/com/android/server/notification/NotificationManagerService.java#2010)

CEFの起動対応条件、更新・削除の手順は[通知の配布登録](./package-install-location-advanced-topics.md#通知の配布登録)を参照してください。

### Androidのシステムバーと安全領域

| 関数 | 戻り値 | 動作 |
| --- | --- | --- |
| `configure(options)` | `Promise<void>` | 上下のバーの表示、アイコン色、コンテンツ配置を要求する |
| `getState()` | `Promise<MuonAndroidSystemBarsState>` | 要求値`requested`とOSの報告値`actual`を取得する |
| `getInsets()` | `Promise<MuonAndroidSystemBarsInsets>` | 初回の画面情報を待ち、現在の余白を取得する |
| `watchInsets(listener)` | `Promise<MuonAndroidInsetsWatcher>` | 初回と以降の余白変化を購読する。`close()`で解除する |

`configure()`は部分更新です。`statusBar`と`navigationBar`へ`visible`と`icons: "light" | "dark"`を指定し、省略した値は維持します。`contentLayout`は`safe-area`または`edge-to-edge`です。非対応項目や不正値は拒否し、要求全体を部分的には適用しません。API 24・25ではナビゲーションバーの濃色アイコンを指定できません。

`requested`はアプリの要求値で、ユーザーのスワイプによる一時表示では変わりません。`actual`はAndroidの公開APIが報告する値です。初期化前は各バーの`visible`と`icons`が`null`になります。画面上の描画を測定した値ではなく、一時表示を「非表示」と報告する場合があります。API 29以前ではAndroidXのInsets情報に基づく近似も含まれます。[WindowInsets.isVisible](https://developer.android.com/reference/android/view/WindowInsets#isVisible(int))、[AndroidのInsetsController](https://android.googlesource.com/platform/frameworks/base/+/refs/heads/main/core/java/android/view/InsetsController.java)

`browser.enterFullscreen()`は上下を非表示、`exitFullscreen()`は上下を表示する要求として状態を共有します。これらの操作はアイコン色と配置方式を変更しません。`configure()`の完了は要求の適用であり、OSアニメーションの終了ではありません。

Insetsは画面の端からの距離で、`top`、`right`、`bottom`、`left`をCSSピクセルで返します。`systemBars`は表示中のバー、`displayCutout`はノッチなどの欠けた領域、`systemGestures`はジェスチャー予約領域、`ime`はソフトウェアキーボードです。`safeArea`はバーとカットアウトの辺ごとの最大値です。キーボードとジェスチャー領域は自動では合算しません。

`safe-area`ではネイティブ側が安全領域を確保し、Web側の追加余白`contentSafeArea`は0になります。`edge-to-edge`ではWeb側で配置します。CSS変数`--muon-safe-area-top`、`--muon-safe-area-right`、`--muon-safe-area-bottom`、`--muon-safe-area-left`は`contentSafeArea`に対応し、回転やズームで更新されます。Android版では、この変数と同じ余白をCSSの`env(safe-area-inset-*)`で重ねて加えないでください。

```css
.app-controls {
  padding: var(--muon-safe-area-top, 0px)
    var(--muon-safe-area-right, 0px)
    var(--muon-safe-area-bottom, 0px)
    var(--muon-safe-area-left, 0px);
}
```

起動時の配置とバー設定は[Androidガイド](./android.md#システムバーと安全領域)を参照してください。

### Androidの通知権限とチャンネル

| 関数 | 動作 |
| --- | --- |
| `getPermissionState()` | 実行時権限とアプリ全体の通知設定を取得する |
| `requestPermission()` | 前面のActivityから明示的に権限を要求する |
| `getCapabilities()` | Android固有の`channels`の対応状況を取得する |
| `ensureChannel(options)` | チャンネルを登録し、OSに保存された設定を返す |
| `getChannel(id)` | 現在のチャンネル設定を返す。未登録なら`null` |

`getPermissionState()`は`runtimePermission: "not-required" | "granted" | "denied"`、`notificationsEnabled`、`requestedBefore`、`shouldShowRationale`、`settingsSuggested`を返します。API 33未満の実行時権限は`not-required`ですが、OS設定で通知を無効にできます。`denied`には未要求も含みます。`requestedBefore`はMuonが要求APIを呼んだ履歴で、ダイアログが表示された証拠ではありません。`settingsSuggested`も永久拒否の判定ではありません。

`requestPermission()`は最新の`state`と、`result: "granted" | "not-granted" | "interrupted" | "not-required"`を返します。`not-granted`から拒否、閉じる操作、ダイアログ抑止のすべてを区別することはできません。要求の重複、前面Activityの不在、要求元ページの破棄はエラーになります。起動時や投稿時に権限ダイアログを自動表示しません。[Androidの通知権限の結果](https://developer.android.com/develop/ui/views/notifications/notification-permission)

チャンネルはAPI 26以降で利用できます。登録には`id`、`name`、`importance: "min" | "low" | "default" | "high"`を指定し、`description`は省略できます。既存チャンネルのユーザー設定は上書きしません。戻り値は`{ id, name, description, importance, enabled }`で、無効化された重要度は`none`です。API 24・25ではチャンネル操作を拒否し、ビルド時に宣言した用途分類だけに投稿できます。[Androidのチャンネル仕様](https://developer.android.com/develop/ui/views/notifications/channels)

### AndroidのOS設定画面

`isAvailable(target)`は画面そのものの利用可否を返します。`open(target, options?)`は前面Activityから画面を開き、`{ requestedTarget, target, fallbackUsed }`を返します。設定変更の有無を返すAPIではありません。復帰後に権限や状態を再取得してください。

`target`には`"app-details"`、`"notifications"`、`{ type: "notification-channel", channelId }`を指定できます。チャンネル画面はAPI 26以降の登録済みIDが必要です。通知専用画面がない場合、`{ fallback: "app-details" }`を明示したときだけアプリ情報へ切り替えます。API 24・25の通知専用画面は非対応です。Activityが利用できない場合や遷移先がない場合はPromiseを拒否します。[Androidの設定画面アクション](https://developer.android.com/reference/android/provider/Settings)

## muon.browser名前空間

`window.muon.browser` は、現在のmuonブラウザウインドウとページ表示を操作します。

| 関数                  | 引数                | 戻り値          | 説明                                                             |
| :-------------------- | :------------------ | :-------------- | :--------------------------------------------------------------- |
| `reload()`            | なし                | `Promise<void>` | 現在のページを再読み込みします。                                 |
| `hardReload()`        | なし                | `Promise<void>` | キャッシュを無視して現在のページを再読み込みします。             |
| `toggleFullscreen()`  | なし                | `Promise<void>` | フルスクリーン状態を切り替えます。                               |
| `enterFullscreen()`   | なし                | `Promise<void>` | フルスクリーン状態にします。                                     |
| `exitFullscreen()`    | なし                | `Promise<void>` | フルスクリーン状態を解除します。                                 |
| `zoomIn()`            | なし                | `Promise<void>` | ページのズームレベルを上げます。                                 |
| `zoomOut()`           | なし                | `Promise<void>` | ページのズームレベルを下げます。                                 |
| `resetZoom()`         | なし                | `Promise<void>` | ページのズームレベルを初期値へ戻します。                         |
| `show()`              | なし                | `Promise<void>` | 現在のウインドウを表示します。                                   |
| `hide()`              | なし                | `Promise<void>` | 現在のウインドウを非表示にします。                               |
| `focus()`             | なし                | `Promise<void>` | 現在のウインドウをフォーカスします。                             |
| `blur()`              | なし                | `Promise<void>` | 現在のウインドウのフォーカスを外します。                         |
| `minimize()`          | なし                | `Promise<void>` | 現在のウインドウを最小化します。                                 |
| `maximize()`          | なし                | `Promise<void>` | 現在のウインドウを最大化します。                                 |
| `restore()`           | なし                | `Promise<void>` | 最小化または最大化されたウインドウを通常状態に戻します。         |
| `getWindowBounds()`   | なし                | `Promise<MuonWindowBounds>` | 現在のトップレベルウインドウ領域を取得します。             |
| `setWindowBounds(bounds)` | `bounds: MuonWindowBounds` | `Promise<void>` | 現在のトップレベルウインドウ領域の変更を要求します。     |
| `setContextMenuItems(items, handler?)` | `items: MuonBrowserContextMenuItem[]`, `handler?: (command) => void` | `Promise<void>` | ネイティブコンテキストメニューへ追加するカスタム項目を登録します。 |
| `clearContextMenuItems()` | なし | `Promise<void>` | 登録済みカスタムコンテキストメニュー項目を解除します。 |
| `createTray(options, handler?)` | `options: MuonBrowserTrayOptions`, `handler?: (event) => void` | `Promise<string>` | ブラウザ所有のシステムトレイ項目を作成します。 |
| `setTrayMenu(id, items, handler?)` | `id: string`, `items: MuonBrowserTrayMenuItem[]`, `handler?: (event) => void` | `Promise<void>` | システムトレイ項目のメニューとイベントハンドラを置き換えます。 |
| `setTrayIcon(id, iconPath)` | `id: string`, `iconPath: string` | `Promise<void>` | システムトレイ項目のPNGアイコンを置き換えます。 |
| `setTrayTooltip(id, tooltip)` | `id: string`, `tooltip: string \| null` | `Promise<void>` | システムトレイ項目のツールチップを設定または解除します。 |
| `removeTray(id)` | `id: string` | `Promise<void>` | システムトレイ項目を削除します。 |
| `setTitleBarVisibility(visible)` | `visible: boolean` | `Promise<void>` | タイトルバーの表示/非表示を切り替えます。                         |
| `setTitleBarIcon(path)` | `path: string \| null` | `Promise<void>` | 現在のウインドウのタイトルバーアイコンを設定または解除します。 |
| `close()`             | なし                | `Promise<void>` | 現在のウインドウを閉じます。                                     |
| `shutdown(exitCode?)` | `exitCode?: number` | `Promise<void>` | muonプロセスを終了します。`exitCode` を省略した場合は `0` です。 |
| `recycle()`           | なし                | `Promise<void>` | muonプロセスを終了し、起動元が対応している場合は自動再起動します。 |

- `reload()`, `hardReload()`, `close()`, `shutdown()`, `recycle()` はページコンテキストの破棄やプロセス終了を伴うため、返されたPromiseを観測する前にJavaScript側の実行環境が消えることがあります。
- `recycle()` は `muon-launcher` や `muon run` など、起動元がリサイクル終了コードに対応している場合だけ自動再起動します。`shutdown(88)` はリサイクル用の予約終了コードのため拒否されます。
- `close()` は、対象ウインドウが所有しているモーダルファイルダイアログを中断してからウインドウを閉じます。
- `getWindowBounds()` と `setWindowBounds()` の bounds はブラウザ表示領域ではなく、muonカスタムタイトルバーやネイティブフレームを含むトップレベルウインドウ領域です。
  座標とサイズの単位はCEF Viewsと同じDIP screen coordinatesです。
  `setWindowBounds()` では `x`, `y`, `width`, `height` に32bit符号付き整数範囲のsafe integerを指定し、`width` と `height` は1以上である必要があります。
  Waylandではトップレベルウインドウの配置がcompositorに管理されるため、位置やサイズの要求が無視または調整されることがあります。
  厳密な位置制御が必要な場合はX11バックエンド（例: `--ozone-platform=x11`）を使用して下さい。
- `setContextMenuItems()` はブラウザウインドウ単位で1つの登録を保持し、再度呼び出すと置き換えます。
  main frame navigation、ブラウザ終了、`clearContextMenuItems()` で登録は解除されます。
  通常項目は `id`, `label`, `enabled`, `placement`, `when` を指定でき、セパレータは `type: "separator"` を指定します。
  `placement` は `"start"`, `"afterEdit"`, `"end"` のいずれかで、省略時は `"end"` です。
  `when` には `editable`, `selection`, `link`, `image`, `canCopy`, `canPaste` のboolean条件を指定出来ます。
  `id` は空文字、制御文字、`muon.` 始まり、`standard.` 始まりを使用出来ません。
- `createTray()` はブラウザウインドウ単位でトレイ項目を保持し、main frame navigation、ブラウザ終了、`removeTray()` で削除されます。
  live中のトレイ項目はブラウザウインドウごとに16個、プロセス全体で64個までです。上限を超える作成要求はネイティブリソースを確保する前に拒否されます。
  `options.id` を省略するとmuonが一意なIDを生成して返します。
  `id` は空文字、制御文字、`muon.` 始まり、`standard.` 始まりを使用出来ません。
  メニュー項目は通常項目、`type: "separator"`、`type: "checkbox"`、`type: "radio"` に対応します。
- `setTitleBarVisibility()` はmuonカスタムタイトルバーの表示/非表示を切り替えます。
  Linux X11のネイティブタイトルバーでは、ウインドウマネージャーへネイティブ装飾の表示/非表示ヒントを設定します。
  このヒントはウインドウマネージャー依存であり、非対応環境では反映されないことがあります。
- `setTitleBarIcon()` はアイコンのアセットパスを受け取り、`null` を指定すると現在のウインドウのタイトルバーアイコンを解除します。
  `path` には `"asset://main/icons/app.png"` のような `asset://main/` URL、または `"icons/app.png"` のような `main` からの相対アセットパスを指定します。
  PNGはエンコード済みデータが1 MiB以下、幅・高さが各256 px以下、総画素数が65,536以下である必要があります。この制限は`createTray()`と`setTrayIcon()`で指定するPNGにも適用されます。
  `"muon"` タイトルバーではSVGなどブラウザが表示できる画像形式を指定出来ます。
  `"native"` タイトルバーではPNG以外を指定するとPromiseが拒否されます。

```js
await window.muon.browser.zoomIn();
await window.muon.browser.resetZoom();
const bounds = await window.muon.browser.getWindowBounds();
await window.muon.browser.setWindowBounds({
  ...bounds,
  width: 960,
  height: 640,
});
await window.muon.browser.setContextMenuItems(
  [
    {
      id: "app.searchSelection",
      label: "Search Selection",
      placement: "afterEdit",
      when: { selection: true },
    },
    { type: "separator", placement: "end" },
  ],
  (command) => {
    console.log(command.id, command.selectionText);
  },
);
const trayId = await window.muon.browser.createTray(
  {
    id: "main",
    icon: "icons/app.png",
    tooltip: "Ready",
    menu: [
      { id: "open", label: "Open" },
      { type: "separator" },
      { id: "quit", label: "Quit" },
    ],
  },
  async (event) => {
    if (event.type === "activate" || event.id === "open") {
      await window.muon.browser.show();
      return;
    }
    if (event.type === "menu" && event.id === "quit") {
      await window.muon.browser.shutdown(0);
    }
  },
);
await window.muon.browser.setTrayTooltip(trayId, "Running");
await window.muon.browser.setTitleBarVisibility(false);
await window.muon.browser.setTitleBarIcon("icons/app.png");
await window.muon.browser.shutdown(0);
await window.muon.browser.recycle();
```

## muon.launcher名前空間

`window.muon.launcher` は、次回 `muon-launcher` 起動時に使われるruntime catalog更新設定を扱います。
設定はruntimeディレクトリの `muon-launcher.ini` に保存され、現在実行中のCEFやNode.js sidecarには影響しません。

| 関数                    | 引数                                                                           | 戻り値                           | 説明                                                                 |
| :---------------------- | :----------------------------------------------------------------------------- | :------------------------------- | :------------------------------------------------------------------- |
| `getSettings()`         | なし                                                                           | `Promise<MuonLauncherSettings>` | 現在有効なlauncher設定を返します。                                  |
| `setSettings(settings)` | `MuonLauncherSettingsPatch`                                                   | `Promise<void>`                  | 次回起動時に使うCEF version policyやruntime catalog更新間隔を保存します。`null` を指定した項目は明示設定を削除します。 |
| `triggerUpdate()`       | なし                                                                           | `Promise<void>`                  | 次回 `muon-launcher` 起動時に、適用対象のCEFとNode.jsのcatalog更新を試行するよう要求します。 |

```js
await window.muon.launcher.setSettings({
  cefVersionPolicy: "compat-latest",
  catalogRefreshIntervalSeconds: 604800,
});
await window.muon.launcher.triggerUpdate();
```

- `cefVersionPolicy` と `cefExactVersion` はCEF専用です。Node.jsのversionは、ビルド時にlauncherへ埋め込まれたNode runtime requirementから選択され、このAPIにはNode.js用のversion policy設定はありません。
- `catalogRefreshIntervalSeconds` と `triggerUpdate()` は、次回のruntime準備で適用対象となるcatalogに共通で作用します。Node.js catalogは、アプリが実行にNode.jsを必要とする場合だけ適用対象になります。
- catalog更新やruntimeの置き換えは実行中に行われません。現在のCEFとすべてのNode.js sidecarはそのまま動作し、変更は次回のlauncher起動時に反映されます。

## muon.environments名前空間

`window.muon.environments` は、muonプロセスの環境情報と自動起動設定を扱います。

| 関数                    | 引数               | 戻り値                            | 説明                                                                                                            |
| :---------------------- | :----------------- | :-------------------------------- | :-------------------------------------------------------------------------------------------------------------- |
| `getVariables()`        | なし               | `Promise<Record<string, string>>` | 現在のプロセス環境変数を返します。                                                                              |
| `getConfigValues()`     | なし               | `Promise<Record<string, string>>` | トップレベルのアプリケーション `config` の実効値を返します。                                                    |
| `getCommandLine()`      | なし               | `Promise<string[]>`               | muon起動時に記録されたコマンドラインを返します。利用可能な場合は `argv[0]` も含みます。                         |
| `getProcessId()`        | なし               | `Promise<number>`                 | ネイティブmuonプロセスIDを返します。                                                                            |
| `getRuntimeInfo()`      | なし               | `Promise<MuonRuntimeInfo>`        | 現在のバックエンドに応じたruntime情報を返します。                                                               |
| `getAutostart()`        | なし               | `Promise<boolean \| undefined>`   | ユーザーセッション開始時に現在のアプリを自動起動する設定かどうかを返します。判別不能な場合は `undefined` です。 |
| `setAutostart(enabled)` | `enabled: boolean` | `Promise<void>`                   | 自動起動設定を有効または無効にします。                                                                          |

- `getRuntimeInfo()` は `backend` で分岐する型を返します。
  `backend === "cef"` の場合は、従来のmuon-coreビルド情報、参照CEF情報、実行中CEF情報を返します。`muonCore` には `version`, `gitCommitHash`, `buildDate`, `gitCommitDate` が含まれ、日付はISO 8601形式です。
  `backend === "android-webview"` の場合は、Android OS version/API level、ABI、application ID/version、WebView provider package/versionを返します。CEF固有fieldは存在しません。
- Android WebViewバックエンドで公開されるenvironment関数は、`getVariables()`, `getConfigValues()`, `getProcessId()`, `getRuntimeInfo()`です。`getCommandLine()`, `getAutostart()`, `setAutostart()`は公開されません。
- Androidはapp processを再生成できるため、process IDやruntime情報をinstallation IDまたは永続的なsession IDとして使用しないでください。
- `getAutostart()` と `setAutostart()` は、起動時のlaunch sourceに応じたプラットフォームバックエンドを使用します。
  POSIX desktopではXDG Autostart、Windowsでは現在のユーザーのRun registry entryを使用します。

Windowsの自動起動では、引用符を含む登録コマンド全体が260 UTF-16コード単位以内である必要があります。UTF-16はWindowsの文字列表現で、日本語文字の多くは1単位、絵文字の多くは2単位として数えます。上限を超える場合は`setAutostart(true)`が`Windows Run command is too long`でrejectされ、登録は作成されません。[Runキーの上限](https://learn.microsoft.com/en-us/windows/win32/setupapi/run-and-runonce-registry-keys)

```js
const variables = await window.muon.environments.getVariables();
const config = await window.muon.environments.getConfigValues();
const processId = await window.muon.environments.getProcessId();
const runtimeInfo = await window.muon.environments.getRuntimeInfo();

if (runtimeInfo.backend === "android-webview") {
  console.log(runtimeInfo.apiLevel, runtimeInfo.webViewVersion);
} else {
  const commandLine = await window.muon.environments.getCommandLine();
  const autostart = await window.muon.environments.getAutostart();
  console.log(runtimeInfo.muonCore.version, runtimeInfo.cefRuntime.version);
  console.log(commandLine);
  if (autostart !== true) {
    await window.muon.environments.setAutostart(true);
  }
}
```

`validate` モードでは、import元に `muon.environments.getConfigValues` を許可して、`muon:environments` virtual moduleを使用します:

```ts
import { getConfigValues } from "muon:environments";

const config = await getConfigValues();
const baseUrl = config.apiBaseUrl;
```

## muon.executor名前空間

`window.muon.executor` は、muon外部の機能を実行する機能を集約しています。
例えば、子プロセスを起動したり、動的ライブラリ内の指定した関数を呼び出せます。

これらの機能は非常に強力であり、かつmuonやCEFのセキュリティ境界面による保護がありません。
任意のパスの実行ファイルを実行出来たり、任意の動的ライブラリのネイティブコードを制限なく実行出来ます。

使用する際は細心の注意が必要です。これらの機能が必要でない場合は、`muon.json` のホワイトリストに乗せないようにして下さい。

| 関数                | 引数                                | 戻り値                         | 説明                                       |
| :------------------ | :---------------------------------- | :----------------------------- | :----------------------------------------- |
| `spawn(options)`    | `options: MuonExecutorSpawnOptions` | `Promise<MuonExecutorProcess>` | 子プロセスを起動し、操作用ハンドルを返します。 |
| `loadLibrary(path)` | `path: string`                      | `Promise<MuonAdhocLibrary>`    | `.so` / `.dll` を読み込み、アドホックFFI用ハンドルを返します。 |

### 子プロセスの実行

`MuonExecutorSpawnOptions`:

| プロパティ | 型                             | 説明                                                                                                          |
| :--------- | :----------------------------- | :------------------------------------------------------------------------------------------------------------ |
| `command`  | `string`                       | 実行ファイルのパス、または `PATH` から解決する実行ファイル名です。必須で、空文字列やNUL文字は使用出来ません。 |
| `args`     | `readonly string[]`            | コマンドライン引数です。シェル解釈は行われず、各要素がそのまま子プロセスへ渡されます。                        |
| `cwd`      | `string`                       | 子プロセスの作業ディレクトリです。                                                                            |
| `env`      | `Record<string, string>`       | 環境変数の上書き値です。現在のプロセス環境とマージされます。キーは空文字列、`=`, NUL文字を含められません。    |
| `daemon`   | `boolean`                      | デーモンとして起動するかを指定します。省略時は `false` です。                                                  |
| `onStdout` | `(chunk: Uint8Array) => void`  | 標準出力のチャンクを逐次受け取ります。指定した場合、`wait()` の結果に `stdout` は含まれません。                |
| `onStderr` | `(chunk: Uint8Array) => void`  | 標準エラーのチャンクを逐次受け取ります。指定した場合、`wait()` の結果に `stderr` は含まれません。              |

`MuonExecutorProcess`:

| プロパティ/関数      | 型                                                   | 説明                                                                                 |
| :------------------- | :--------------------------------------------------- | :----------------------------------------------------------------------------------- |
| `processId`          | `number`                                             | 起動した子プロセスIDです。                                                           |
| `writeStdin(data)`   | `(data: string \| BufferSource) => Promise<void>`    | 標準入力へ逐次書き込みます。文字列はUTF-8としてエンコードされ、呼び出し順に処理されます。 |
| `closeStdin()`       | `() => Promise<void>`                                | 未完了の書き込みを処理した後、標準入力を閉じます。                                   |
| `wait()`             | `() => Promise<MuonExecutorSpawnResult>`             | ルートプロセスの終了を待ちます。同じPromiseを再利用します。                          |
| `kill()`             | `() => Promise<void>`                                | 接続中のハンドルが管理するプロセスツリー全体の終了を要求します。                     |
| `release()`          | `() => Promise<void>`                                | ネイティブハンドルを解放します。プロセスの扱いは `daemon` によって異なります。       |

`MuonExecutorSpawnResult`:

| プロパティ  | 型           | 説明                                                                 |
| :---------- | :----------- | :------------------------------------------------------------------- |
| `processId` | `number`     | 起動した子プロセスIDです。                                           |
| `exitCode`  | `number`     | 子プロセスの終了コードです。非0終了でもPromiseは解決されます。       |
| `stdout`    | `Uint8Array` | `onStdout` が指定されていない場合に、標準出力として収集されたバイト列です。 |
| `stderr`    | `Uint8Array` | `onStderr` が指定されていない場合に、標準エラーとして収集されたバイト列です。 |

`daemon: false` の場合、muonは起動したプロセスツリーを所有します。
ルートプロセスの自然終了、`release()`、コンテキストの解放、またはmuonの終了時に、残っているプロセスツリーを終了します。

`daemon: true` の場合、`release()`、コンテキストの解放、またはmuonの終了時にシグナルを送らず、実行中のプロセスツリーをデタッチします。
`wait()` はどちらのモードでもルートプロセスの終了を基準として解決され、ハンドルを自動的に解放します。
従って `daemon: true` では、ルートプロセスの終了後も残っている子孫プロセスはデタッチされます。
ただし、ハンドルが接続されている間の `kill()` は、どちらのモードでもプロセスツリー全体を終了します。

ハンドルの解放時にはmuon側の標準入出力とコールバックも閉じられます。
デタッチされたプロセスは標準入力のEOFや、標準出力・標準エラーへの書き込み失敗を観測する場合があります。
解放したプロセスへ再接続することは出来ません。

Linuxでは、起動時に作成した同一プロセスグループ内をプロセスツリーとして扱います。
プロセス自身がそのグループから離脱した場合は管理対象外です。
WindowsではJob Objectに属するプロセスが管理対象です。

Windowsでは、`command`に長い実行ファイルパスを指定できます。引数は`args`へ分けてください。単なるコマンド名の場合は従来どおり検索されます。`cwd`には、解決後に260 UTF-16コード単位未満となる作業ディレクトリを指定してください。子プロセス自身が受け取る引数や扱うファイルの上限は、そのプログラムの仕様に従います。[Windowsのプロセス起動と作業ディレクトリ](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-setcurrentdirectory)

```js
const child = await window.muon.executor.spawn({
  command: "node",
  args: ["script.js"],
  onStdout: (chunk) => console.log(new TextDecoder().decode(chunk)),
});

await child.writeStdin("input text");
await child.closeStdin();

const result = await child.wait();
console.log(result.exitCode);
```

例えば、muonの終了後もプロセスを継続させる場合は `daemon: true` を指定します:

```js
const service = await window.muon.executor.spawn({
  command: "node",
  args: ["service.js"],
  daemon: true,
});

await service.release();
```

### 動的ライブラリのロードと実行

`loadLibrary()` は、指定された動的ライブラリをロードし、ライブラリ内のエントリポイントを呼び出し可能にします。

返されたハンドル (`MuonAdhocLibrary`) を使用して、ライブラリ内の関数エントリポイントを特定して、JavaScriptの関数オブジェクトを取得できます。
この関数オブジェクトを通じて、動的ライブラリの機能を呼び出せます。

`MuonAdhocLibrary`:

| 関数                              | 型                                                           | 説明                                                              |
| :-------------------------------- | :----------------------------------------------------------- | :---------------------------------------------------------------- |
| `getFunction(name, signature)`    | `<T>(name: string, signature: MuonAdhocSignature) => Promise<T>` | native symbolを解決し、async JavaScript proxy関数を返します。       |
| `release()`                       | `() => Promise<void>`                                       | 新規呼び出しを拒否し、実行中呼び出しの完了後にライブラリを解放します。 |

対象のエントリポイントの関数がどのようなシグネチャ（引数と戻り値の型）を想定しているかは、 `getFunction()` の引数で指定する必要があります。
従って、不明なシグネチャのエントリポイントを呼び出すことは出来ません。

`MuonAdhocSignature` は `{ argTypes, returnType }` で、型定数には `voidType`, `boolType`, `int8Type`, `uint8Type`, `int16Type`, `uint16Type`, `int32Type`, `uint32Type`, `int64Type`, `uint64Type`, `float32Type`, `float64Type`, `stringType`, `pointerType`, `bufferViewType`, `usizeType` を指定出来ます。
64bit整数と `usize` はJSONではdecimal stringとして運ばれ、JavaScript側の引数には `number`, `bigint`, `string` を渡せます。

以下の例は、 `libc.so` 内の `malloc` と `free` を呼び出す例です:

```ts
import {
  loadLibrary,
  pointerType,
  usizeType,
  voidType,
} from "muon:executor";

type MallocType = (size: MuonAdhocIntegerValue) => Promise<MuonNativePointer>;
type FreeType = (p: MuonNativePointer) => Promise<void>;

// libc.soをロードする
const library = await loadLibrary("libc.so");
try {
  // mallocへの関数オブジェクトを得る
  const malloc = await library.getFunction<MallocType>("malloc", {
    argTypes: [usizeType],
    returnType: pointerType,
  });

  // freeへの関数オブジェクトを得る
  const free = await library.getFunction<FreeType>("free", {
    argTypes: [pointerType],
    returnType: voidType,
  });

  // 呼び出す
  const p = await malloc(123n);
  await free(p);
} finally {
  await library.release();
}
```

- すべてのエントリポイント関数は、一般的な戻り値のシグネチャ、つまり同期的な戻り値を想定します。
  しかしJavaScript側では、 `getFunction()` が返す関数オブジェクトが必ず `Promise<T>` を返します。
  つまり、内部では同期的にエントリポイント関数を呼び出していますが、JavaScript側では必ず `await` で `Promise<T>` が完了するのを待機する必要があります。
- エントリポイント関数の呼び出しは、一時ワーカースレッドで実行されるため、関数が呼び出しを長時間ブロックしてもmuonの動作に支障はありません。
  但し、`loadLibrary()` でロードしたライブラリを `release()` で解放する場合は、これらの関数呼び出しの `Promise` がすべて完了している必要があります。完了していない場合は `release()` が待機させられます。

> 注釈: この機能は、非常に簡易的でアドホックな利用を想定しています。
> 例えば、戻り値や引数のポインタは、 `MuonNativePointer` として扱われ、muonは所有権を管理しません。
> また、 `getFunction()` で扱う型メタデータ定義には、任意の構造を持つ構造体などを定義できません。
> 複雑なコールバック所有権や、寿命の長いあるいは厳密な管理が必要なインスタンスを扱う場合、構造体によるデータのやり取りが必要な場合は、通常のmuonプラグインとして実装することを検討して下さい。

## muon.fs名前空間

`window.muon.fs` は、ローカルファイルシステムを操作します。
各関数の `path`, `source`, `destination`, `target` は、ファイル位置を表す文字列です。
多くの関数は最後の引数に `{ signal?: AbortSignal }` を指定出来ます。
すでにabort済みのsignalを渡した場合は即座にrejectされ、処理中にabortされた場合は可能な範囲でネイティブ処理のキャンセルを要求します。

Linux環境ではGIO/GVfsを利用するため、通常の `muon.fs` 関数に渡すファイル位置引数にはローカルパスまたはURIを指定出来ます。
GTKファイルダイアログで `gtk.localOnly: false` の場合にGVfs上のURIが選択結果として返ったときも、そのURIを通常の `muon.fs` 関数へ渡せます。
Linux以外の環境では、通常の `muon.fs` 関数はローカルファイルシステム上のパスを扱います。
Android WebViewバックエンドの現在の実装は、Android OSが許可する実際のfilesystem pathだけを扱います。`content://` URIは現在rejectされ、直接指定する対応は後続作業です。`muon.fs.dialogs`も、そのURIを扱う契約が実装されるまではAndroidへ公開されません。

Windowsでは、通常表記の長いパスを読み書き、列挙、コピー、移動、削除、監視などに使用できます。長いパスのための管理者操作やOS設定の変更は不要です。相対パスは既存の基準ディレクトリから解決されます。`symlink()`の相対ターゲットは、リンク内に相対表記で保存されます。

`realpath()`は同じ対象を表せる場合に通常の絶対パスを返します。入力で明示した`\\?\`や、末尾空白・ピリオドなどの名前を正確に表すために必要な接頭辞は保持します。`readlink()`が返す相対ターゲットも相対のままです。返却パスは接頭辞を手動で除かず、次の`muon.fs`操作へ渡してください。`readdir()`が返す名前も文字を削らずに扱ってください。[Windowsの配置制約とパス長](./limitation.md#windowsの長いパス)

| 関数                                             | 引数                                                                                                                 | 戻り値                    | 説明                                                                                                                                                       |
| :----------------------------------------------- | :------------------------------------------------------------------------------------------------------------------- | :------------------------ | :--------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `readFile(path, options?)`                       | `path: string`, `options?: { position?: number, length?: number, signal?: AbortSignal }`                             | `Promise<ArrayBuffer>`    | バイナリファイルを読み込みます。`position` は読み取り開始バイト位置、`length` は要求する読み取りバイト数です。どちらも非負のsafe integerである必要があります。 |
| `writeFile(path, data, options?)`                | `path: string`, `data: BufferSource`, `options?: { position?: number, signal?: AbortSignal }`                        | `Promise<void>`           | バイナリデータを書き込みます。`position` 省略時はファイル全体を置き換え、指定時はそのバイト位置へ書き込みます。                                            |
| `readTextFile(path, encoding, options?)`         | `path: string`, `encoding: "utf8" \| "utf-8"`, `options?: { signal?: AbortSignal }`                                  | `Promise<string>`         | UTF-8テキストファイルを読み込みます。ファイルはNUL文字を含まない有効なUTF-8で、raw byte数が設定上限以内である必要があります。                                |
| `writeTextFile(path, data, encoding, options?)`  | `path: string`, `data: string`, `encoding: "utf8" \| "utf-8"`, `options?: { signal?: AbortSignal }`                  | `Promise<void>`           | UTF-8テキストとしてファイル全体を置き換えます。                                                                                                            |
| `stat(path, options?)`                           | `path: string`, `options?: { signal?: AbortSignal }`                                                                 | `Promise<MuonFsStats>`    | シンボリックリンクをたどってメタデータを返します。パスが存在しない場合はrejectします。                                                                     |
| `lstat(path, options?)`                          | `path: string`, `options?: { signal?: AbortSignal }`                                                                 | `Promise<MuonFsStats>`    | シンボリックリンクをたどらずにメタデータを返します。                                                                                                       |
| `exists(path, options?)`                         | `path: string`, `options?: { signal?: AbortSignal }`                                                                 | `Promise<boolean>`        | パスが存在し、検査可能なら `true` を返します。検査時のファイルシステムエラーは `false` になります。                                                        |
| `access(path, options?)`                         | `path: string`, `options?: { mode?: readonly ("read" \| "write" \| "execute")[], signal?: AbortSignal }`             | `Promise<boolean>`        | パスが存在し、指定したアクセスモードがすべて許可されている場合に `true` を返します。`mode` 省略時は存在確認のみです。                                      |
| `readdir(path, options?)`                        | `path: string`, `options?: { withFileTypes?: false, signal?: AbortSignal }`                                          | `Promise<string[]>`       | ディレクトリエントリ名を返します。                                                                                                                         |
| `readdir(path, options)`                         | `path: string`, `options: { withFileTypes: true, signal?: AbortSignal }`                                             | `Promise<MuonFsDirent[]>` | ディレクトリエントリ名とメタデータを返します。                                                                                                             |
| `mkdir(path, options?)`                          | `path: string`, `options?: { recursive?: boolean, signal?: AbortSignal }`                                            | `Promise<void>`           | ディレクトリを作成します。`recursive` 省略時は `false` です。                                                                                              |
| `rm(path, options?)`                             | `path: string`, `options?: { recursive?: boolean, force?: boolean, signal?: AbortSignal }`                           | `Promise<void>`           | ファイルまたはディレクトリを削除します。ディレクトリツリーの削除には `recursive: true` が必要です。`force: true` はパス未存在エラーを抑制します。          |
| `unlink(path, options?)`                         | `path: string`, `options?: { signal?: AbortSignal }`                                                                 | `Promise<void>`           | ファイルまたはシンボリックリンクを削除します。ディレクトリの場合はrejectします。                                                                           |
| `rmdir(path, options?)`                          | `path: string`, `options?: { signal?: AbortSignal }`                                                                 | `Promise<void>`           | 空ディレクトリを削除します。                                                                                                                               |
| `rename(oldPath, newPath, options?)`             | `oldPath: string`, `newPath: string`, `options?: { signal?: AbortSignal }`                                           | `Promise<void>`           | パスをリネームまたは移動します。                                                                                                                           |
| `copyFile(source, destination, options?)`        | `source: string`, `destination: string`, `options?: { overwrite?: boolean, signal?: AbortSignal }`                   | `Promise<void>`           | 通常ファイルをコピーします。`overwrite` 省略時は `true` です。                                                                                             |
| `appendFile(path, data, options?)`               | `path: string`, `data: BufferSource`, `options?: { signal?: AbortSignal }`                                           | `Promise<void>`           | バイナリデータをファイル末尾へ追加します。ファイルが存在しない場合は作成します。                                                                           |
| `appendTextFile(path, data, encoding, options?)` | `path: string`, `data: string`, `encoding: "utf8" \| "utf-8"`, `options?: { signal?: AbortSignal }`                  | `Promise<void>`           | UTF-8テキストをファイル末尾へ追加します。ファイルが存在しない場合は作成します。                                                                            |
| `truncate(path, length?, options?)`              | `path: string`, `length?: number`, `options?: { signal?: AbortSignal }`                                              | `Promise<void>`           | ファイルを指定バイト長へ切り詰め、または拡張します。`length` は非負のsafe integerです。省略時は `0` です。                                                 |
| `truncate(path, options)`                        | `path: string`, `options: { signal?: AbortSignal }`                                                                  | `Promise<void>`           | `options` を第2引数に渡し、ファイルを0バイトに切り詰めます。                                                                                               |
| `realpath(path, options?)`                       | `path: string`, `options?: { signal?: AbortSignal }`                                                                 | `Promise<string>`         | 正規化された絶対パスを返します。パスまたは構成要素が存在しない場合はrejectします。                                                                         |
| `readlink(path, options?)`                       | `path: string`, `options?: { signal?: AbortSignal }`                                                                 | `Promise<string>`         | シンボリックリンクのリンク先を返します。                                                                                                                   |
| `symlink(target, path, type?, options?)`         | `target: string`, `path: string`, `type?: "file" \| "dir" \| "junction"`, `options?: { signal?: AbortSignal }`       | `Promise<void>`           | シンボリックリンクを作成します。`type` 省略時は `"file"` です。`"dir"` と `"junction"` はディレクトリリンクを作成します。                                  |
| `symlink(target, path, options)`                 | `target: string`, `path: string`, `options: { signal?: AbortSignal }`                                                | `Promise<void>`           | `options` を第3引数に渡し、ファイルリンクを作成します。                                                                                                    |
| `watch(path, listener, options?)`                | `path: string`, `listener: (event: MuonFsWatchEvent) => void \| Promise<void>`, `options?: { signal?: AbortSignal }` | `Promise<MuonFsWatcher>`  | パスの変更を監視し、watcherを返します。現在はスナップショットのポーリングで差分を通知します。                                                              |

- `readFile()` は、ネイティブ層で1回の操作ごとに読み取り上限を強制します。既定値は64 MiB (`67108864` byte) です。
  明示した `length` が上限を超える場合は、ファイルへのアクセス前に `Promise` をrejectします。
- `length` を省略した場合は `position` からファイル末尾までの全byteが上限内である必要があり、上限まで暗黙に切り詰めることはありません。
  `length` が設定上限内で、 `position` がファイル末尾以降の場合は空の `ArrayBuffer` を返します。`length: 0` の場合もファイルへアクセスせず空の `ArrayBuffer` を返します。
- `readTextFile()` も、ネイティブ層で1回の操作ごとに独立したraw byte読み取り上限を強制します。既定値は64 MiB (`67108864` byte) です。
  上限ちょうどのソースは読み取れますが、上限を超えるソースは切り詰めず、UTF-8/NUL検証や文字列化の前に `Promise` をrejectします。

上限は `muon.json` の内蔵プラグイン設定で変更できます。JavaScript APIのシグネチャは変わりません。

```json
{
  "plugin": {
    "plugins": [
      {
        "name": "internal",
        "config": {
          "fs.readFile.maxBytes": "67108864",
          "fs.readTextFile.maxBytes": "67108864"
        }
      }
    ]
  }
}
```

`fs.readFile.maxBytes` と `fs.readTextFile.maxBytes` は、それぞれbyte単位の符号なし10進整数文字列です。空文字列、符号、空白、小数点、指数表記、単位suffixは使用できず、値は `uint64_t` の範囲内である必要があります。先頭ゼロと `"0"` は有効です。
`fs.readFile.maxBytes` が `"0"` の場合は空のrangeだけが成功し、 `fs.readTextFile.maxBytes` が `"0"` の場合は空のソースだけが成功します。不正値は既定値へフォールバックせず、内蔵プラグインの初期化を失敗させます。
2つの上限は独立しており、それぞれ対応する関数だけに適用されます。どちらも複数操作を合計したquotaではありません。

`MuonFsStats`:

| プロパティ/メソッド | 型                                                                                                          | 説明                                                          |
| :------------------ | :---------------------------------------------------------------------------------------------------------- | :------------------------------------------------------------ |
| `type`              | `"file" \| "directory" \| "symlink" \| "blockDevice" \| "characterDevice" \| "fifo" \| "socket" \| "other"` | エントリ種別です。                                            |
| `size`              | `number`                                                                                                    | 通常ファイルのバイトサイズです。それ以外では `0` です。       |
| `mtimeMs`           | `number`                                                                                                    | 最終更新時刻です。Unix epochからのミリ秒で表されます。        |
| `readonly`          | `boolean`                                                                                                   | 書き込み権限ビットが1つも設定されていない場合に `true` です。 |
| `isFile()`          | `() => boolean`                                                                                             | `type === "file"` の場合に `true` を返します。                |
| `isDirectory()`     | `() => boolean`                                                                                             | `type === "directory"` の場合に `true` を返します。           |
| `isSymbolicLink()`  | `() => boolean`                                                                                             | `type === "symlink"` の場合に `true` を返します。             |

- `MuonFsDirent` は `MuonFsStats` に `name: string` を加えた型です。
- `name` は読み取ったディレクトリからの相対エントリ名です。

`MuonFsWatchEvent`:

| プロパティ  | 型                                | 説明                                                               |
| :---------- | :-------------------------------- | :----------------------------------------------------------------- |
| `eventType` | `"rename" \| "change" \| "error"` | 通知種別です。作成と削除は `"rename"` として通知されます。         |
| `filename`  | `string \| null`                  | 変更されたエントリ名です。監視対象そのものの変更では `null` です。 |
| `message`   | `string`                          | `eventType === "error"` の場合のエラーメッセージです。             |

- `MuonFsWatcher` は `close(): Promise<void>` を持ちます。
- `close()` は複数回呼んでも問題ありません。
- `watch()` の `listener` が例外を投げたりrejectされた `Promise` を返した場合、そのエラーは無視されます。
  watcher作成前にabortされた場合は `watch()` がrejectされ、作成後にabortされた場合はwatcherが閉じられます。
- 1つのrenderer V8 contextが同時に保持できるfilesystem watcherは16個までです。
  上限を超える `watch()` は初回スナップショットを開始する前に `"Filesystem watcher limit exceeded"` でrejectされます。

```js
await window.muon.fs.writeTextFile("/tmp/muon-note.txt", "hello\n", "utf8");
const text = await window.muon.fs.readTextFile("/tmp/muon-note.txt", "utf8");

const entries = await window.muon.fs.readdir("/tmp", { withFileTypes: true });
for (const entry of entries) {
  console.log(entry.name, entry.isDirectory() ? "dir" : entry.type);
}

const watcher = await window.muon.fs.watch("/tmp/muon-note.txt", (event) => {
  console.log(event.eventType, event.filename);
});
await watcher.close();
```

## muon.fs.dialogs名前空間

`window.muon.fs.dialogs` は、ネイティブファイルダイアログを表示します。
この名前空間の関数はファイルやディレクトリを作成・変更せず、ユーザーが選択したパスまたはURIを返すだけです。

Windowsの選択結果は、OSのシェルにより8.3形式の短い別名や`\\?\`付きのパスになる場合があります。指定した初期フォルダーと文字列表記が同じとは限りません。返されたパスをそのまま`muon.fs`へ渡してください。ファイルI/Oとシェルの長いパス対応は別の制約です。[Microsoftのシェルに関する説明](https://learn.microsoft.com/en-us/windows/win32/fileio/maximum-file-path-limitation)

| 関数                          | 引数                                | 戻り値                    | 説明                                                                          |
| :---------------------------- | :---------------------------------- | :------------------------ | :---------------------------------------------------------------------------- |
| `selectFile(options?)`        | `options?: MuonFsOpenDialogOptions` | `Promise<string \| null>` | ファイルを1つ選択するダイアログを表示します。キャンセル時は `null` です。     |
| `selectFiles(options?)`       | `options?: MuonFsOpenDialogOptions` | `Promise<string[]>`       | 複数ファイルを選択するダイアログを表示します。キャンセル時は空配列です。      |
| `selectDirectory(options?)`   | `options?: MuonFsOpenDialogOptions` | `Promise<string \| null>` | ディレクトリを1つ選択するダイアログを表示します。キャンセル時は `null` です。 |
| `selectDirectories(options?)` | `options?: MuonFsOpenDialogOptions` | `Promise<string[]>`       | 複数ディレクトリを選択するダイアログを表示します。キャンセル時は空配列です。  |
| `selectSaveFile(options?)`    | `options?: MuonFsSaveDialogOptions` | `Promise<string \| null>` | 保存先ファイルを選択するダイアログを表示します。キャンセル時は `null` です。  |

共通オプション:

| プロパティ    | 型                                                           | 説明                                                                                                                                                             |
| :------------ | :----------------------------------------------------------- | :--------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `signal`      | `AbortSignal`                                                | ダイアログ操作を中断するsignalです。                                                                                                                             |
| `title`       | `string`                                                     | ダイアログタイトルです。                                                                                                                                         |
| `defaultPath` | `string`                                                     | 初期表示パスまたはURIです。GTKでは `gtk.localOnly: false` の場合にGVfs URIも受け付けます。                                                                       |
| `buttonLabel` | `string`                                                     | 決定ボタンのラベルです。                                                                                                                                         |
| `modal`       | `boolean`                                                    | 呼び出し元ブラウザビューをダイアログ表示中に無効化するかどうかです。省略時は `true` です。`true` の場合、所有ウインドウが閉じられるとダイアログはabortされます。 |
| `showHidden`  | `boolean`                                                    | バックエンドが対応している場合、隠しファイルを表示します。                                                                                                       |
| `filters`     | `readonly { name: string, extensions: readonly string[] }[]` | ファイル種別フィルタです。`extensions` は `"png"`, `".png"`, `"*.png"`, `"*"` のように指定出来ます。各filterには空ではない `name` と1つ以上の拡張子が必要です。  |
| `gtk`         | `MuonFsGtkDialogOptions`                                     | GTK固有オプションです。                                                                                                                                          |
| `win32`       | `MuonFsWin32DialogOptions`                                   | Win32固有オプションです。                                                                                                                                        |

`MuonFsSaveDialogOptions` では、共通オプションに加えて以下を指定出来ます。

| プロパティ         | 型        | 説明                                                           |
| :----------------- | :-------- | :------------------------------------------------------------- |
| `defaultName`      | `string`  | 保存ダイアログに表示する初期ファイル名です。                   |
| `confirmOverwrite` | `boolean` | 既存ファイルを置き換える前に確認します。省略時は `true` です。 |

GTK固有オプション:

| プロパティ      | 型                  | 説明                                                                                                 |
| :-------------- | :------------------ | :--------------------------------------------------------------------------------------------------- |
| `localOnly`     | `boolean`           | ローカルファイルだけに制限します。省略時は `false` で、GVfsロケーションからURIが返ることがあります。 |
| `createFolders` | `boolean`           | 対応している保存・フォルダ選択ダイアログでフォルダ作成を許可します。                                 |
| `mimeTypes`     | `readonly string[]` | 追加のMIME typeフィルタです。各要素は空ではない文字列です。                                          |

Win32固有オプション:

| プロパティ           | 型        | 説明                                                                   |
| :------------------- | :-------- | :--------------------------------------------------------------------- |
| `forceFilesystem`    | `boolean` | ファイルシステムに裏付けられたshell itemだけを選択できるようにします。 |
| `noDereferenceLinks` | `boolean` | ショートカットやリンクの参照先ではなく、リンク項目自体を返します。     |
| `dontAddToRecent`    | `boolean` | 選択した場所を最近使ったドキュメントに追加しません。                   |
| `noValidate`         | `boolean` | 通常のshell検証を通らないパス入力を許可します。                        |
| `strictFileTypes`    | `boolean` | 入力されたファイル名を設定済みファイル種別に制限します。               |
| `pathMustExist`      | `boolean` | 選択パスが存在することを要求します。                                   |
| `fileMustExist`      | `boolean` | 選択ファイルが存在することを要求します。                               |

```js
const path = await window.muon.fs.dialogs.selectFile({
  title: "Open image",
  filters: [{ name: "Images", extensions: ["png", "jpg", "jpeg"] }],
});

if (path !== null) {
  const image = await window.muon.fs.readFile(path);
  console.log(image.byteLength);
}
```

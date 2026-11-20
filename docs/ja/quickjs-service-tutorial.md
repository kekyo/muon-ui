# QuickJSでメモサービスを作る

画面で入力したメモをQuickJSサービスのファイルに保存します。ページを再読み込みして「Load」を押すと、先ほどのメモを読み戻せます。

LinuxまたはWindowsで、Vite 8以降とmuonのViteプラグインを設定したアプリを使います。新しく用意する場合は、[muonを始める](getting-started.md)の手順で開発ウィンドウが開くところまで進めてください。以下のパスとコマンドは、そのアプリのルートを基準にします。

## サービスを登録する

```sh
npm install -D muon-plugin-sdk
```

次の `muon.json` はチュートリアル用の設定全体です。`note` という名前でサービスを登録し、`io.fs.data` で専用領域の読み書きを、`plugin` で画面からのサービス取得を許可します。既存のsimpleモードのアプリでは、サービスの定義と画面側の許可を既存の設定に追加してください。

```json
{
  "services": {
    "note": {
      "runtime": "quickjs",
      "entry": "services/note/service.ts",
      "io": { "fs": { "data": "read-write" } }
    }
  },
  "plugin": {
    "mode": "simple",
    "plugins": [{ "name": "internal", "allow": ["muon.services.*"] }]
  }
}
```

## 読み書きするメソッドを作る

`services/note/interface.ts` に、画面とサービスで共有するメソッドを宣言します。`load` は文字列を返し、`save` は文字列を1つ受け取ります。`save` の戻り値は `void` です。画面側では `Promise<void>` を `await` して保存の完了を待てます。

```ts
import { defineServiceInterface } from 'muon-plugin-sdk/services';

export const noteInterface = defineServiceInterface({
  schemaVersion: 1,
  methods: {
    load: { parameters: [], returns: 'string', errors: [] },
    save: { parameters: ['string'], returns: 'void', errors: [] },
  },
  events: {},
});
```

同じディレクトリに `service.ts` を作成します。初回などで `note.txt` が見つからない場合、`load` の戻り値は空文字列です。それ以外の読み取りエラーは呼び出し元に伝えます。`save` はファイルへの書き込みを待ってから完了します。

```ts
import { defineService, isServiceIoError } from 'muon-plugin-sdk/services';
import { noteInterface } from './interface.ts';

export default defineService({
  interface: noteInterface,
  start: (context) => ({
    methods: {
      load: async (call): Promise<string> => {
        try {
          return await context.fs.readText('note.txt', { signal: call.signal });
        } catch (error) {
          if (isServiceIoError(error) && error.code === 'not-found') return '';
          throw error;
        }
      },
      save: async (call, text): Promise<void> => {
        await context.fs.writeText('note.txt', text, { signal: call.signal });
      },
    },
    stop: () => {},
  }),
});
```

## 画面から保存と読み込みを呼ぶ

チュートリアル用アプリの `index.html` を次の内容にします。

```html
<!doctype html>
<html lang="en">
  <meta charset="utf-8">
  <title>Service notes</title>
  <fieldset>
    <legend>Notes</legend>
    <label>Note <textarea></textarea></label>
    <button type="button" value="save">Save</button>
    <button type="button" value="load">Load</button>
    <output></output>
  </fieldset>
  <script type="module" src="/src/note-page.ts"></script>
</html>
```

`src/note-page.ts` を作成します。ボタンを押すたびにサービスを取得し、`finally` で解放します。`bindServiceClient` に共有の宣言を渡すと、`await note.call('load', [])` の結果の型は `string` です。

```ts
import type {} from 'muon-ui';
import { bindServiceClient } from 'muon-plugin-sdk/services';
import { noteInterface } from '../services/note/interface.ts';

const services = window.muon.services;
if (!services) throw new Error('Service access is not enabled');
const panel = document.querySelector('fieldset')!;
const text = document.querySelector('textarea')!;
const status = document.querySelector('output')!;

for (const button of panel.querySelectorAll('button')) {
  button.onclick = async () => {
    panel.disabled = true;
    try {
      const handle = await services.acquire('note');
      try {
        const note = bindServiceClient(noteInterface, handle);
        if (button.value === 'save') {
          await note.call('save', [text.value]);
          status.value = 'Saved';
        } else {
          text.value = await note.call('load', []);
          status.value = 'Loaded';
        }
      } finally {
        await handle.release();
      }
    } catch (error) {
      status.value = String(error);
    } finally {
      panel.disabled = false;
    }
  };
}
```

## 保存したメモを確かめる

```sh
npx vite dev
```

1. muonのウィンドウで初めて「Load」を押すと、入力欄は空になり「Loaded」と表示されます。
2. `買い物リスト` と入力して「Save」を押し、「Saved」と表示されるのを待ちます。
3. 開発コマンドを動かしたままページを再読み込みします。「Load」を押すと `買い物リスト` が戻ります。

開発コマンドを終了すると、その開発セッションのデータは削除されます。配布アプリの再起動をまたぐ保存を試す場合は、[配布用ビルド](getting-started.md#配布用ビルド)で作成したアプリを起動し、「Save」、アプリの終了、再起動、「Load」の順に操作してください。

# muon DevTools

muonは、muon DevToolsを表示出来ます。これは、ChromiumやChromeのDevToolsと同じ機能を持ち、アドボックな簡易デバッグや、パフォーマンスの測定、診断などを行うことが出来ます。

`vite dev` でViteから起動した場合は、`F12` のmuon DevToolsキーバインドと `Ctrl+F12` のリサイクルキーバインドが自動的に有効化されますが、
配布ビルド後のmuonアプリは、既定ではmuon DevToolsを開くことは出来ません。

`muon.json` に明示的に以下の定義を加えることで、ビルド後のmuonアプリでもmuon DevToolsを表示させることが出来ます:

```json
{
  "browser": {
    "keybind": {
      "devtools": "f12"
    },
  }
}
```

これは、`F12` キーをDevTools表示に割り当てた例です。他のキーを指定することや、`shift+f12` のように組み合わせることも出来ます。

DevToolsは他の方法でも表示出来ます。
CDP (Chrome DevTools Protocol)というリモートデバック機能を使用すれば、ChromiumやChromeを使用して、リモートでDevToolsを表示出来ます。

但し、この機能も既定では無効化されています。同じく`muon.json`に以下の定義を加えます:

```json
{
  "cdp": {
    "enable": true
  },
}
```

この構成でmuonアプリを起動した後、ChromiumまたはChromeで、 `chrome://inspect/` ページを表示させます。

![chrome://inspect/](../../images/inspect.png)

ここで、"Remote Target" に表示されたmuonのインスタンスの `inspect` リンクをクリックすれば、そのmuonとCDPで接続してリモートDevToolsを表示出来ます。

muonは自力でDevToolsを表示できるので、わざわざCDPを使ってリモートDevToolsを表示させる必要性は薄いのですが、
CDPが使用できると他のデバッガも使用できるようになります。

例えば、vscodeの構成ファイルである `.vscode/launch.json` に以下の定義を加えることで、デバッグセッションを開始出来ます:

```json
{
  "version": "0.2.0",
  "configurations": [
    {
      "type": "chrome",
      "request": "attach",
      "name": "Attach to muon",
      "port": 9222,
      "webRoot": "${workspaceFolder}"
    }
  ]
}
```

`port` には、`muon.json` の `cdp.port` と同じ値を指定します。既定値は `9222` です。

---


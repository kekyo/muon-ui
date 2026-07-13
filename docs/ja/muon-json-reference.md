# muon.jsonリファレンス

`muon.json` は、muonの動作を決定し、いくつかの機能はこのファイルでのみ決定出来ます。
特に、動作を許可するホワイトリストは、プログラマブルに変更出来ません。

設定ファイルは `muon.json5`、`muon.jsonc`、`muon.json` の順に探索されます。
muon Viteプラグインから起動する場合 (`vite dev`) に設定ファイルが存在しない場合やパースできない場合は警告を表示し、プロジェクト設定をすべて既定値として起動します。
`muon run` では、設定ファイルが存在しない場合は、開発用の生成設定だけで起動しますが、存在するファイルが読み取れない場合やパースできない場合はエラーになります。

一方で `muon build` では、設定ファイルが存在しない場合はすべて既定値として扱いますが、存在するファイルが読み取れない場合やパースできない場合はビルドエラーになります。
`--config` で明示した設定ファイルが存在しない場合もエラーです。

以下は、 `muon.json` の例です:

```json
{
  "iconPath": "icons/app.png",
  "browser": {
    "initialWindowState": "normal",
    "backgroundColor": "system",
    "titleBarType": "muon",
    "initialTitleBarVisibility": true,
    "keybinds": {
      "devtools": "f12",
      "zoomIn": "ctrl+plus",
      "zoomOut": "ctrl+minus"
    }
  },
  "network": {
    "allow": [
      "asset://main/**",
      "https://img.examples.com/images/**"
    ]
  },
  "plugin": {
    "mode": "validate",
    "pages": ["asset://main/**"],
    "plugins": [
      {
        "name": "internal",
        "config": {
          "fs.readFile.maxBytes": "67108864"
        },
        "imports": [
          {
            "sources": ["src/native/**"],
            "allow": ["muon.environments.getCommandLine"]
          },
          {
            "packages": ["@example/trusted-muon-helper"],
            "allow": ["muon.fs.readTextFile"]
          }
        ]
      },
      {
        "name": "foobar",
        "config": {
          "foobar.mode": "strict"
        },
        "imports": [
          {
            "sources": ["src/native/**"],
            "allow": ["foobar.native.*"]
          }
        ]
      }
    ]
  },
  "cdp": {
    "enable": true,
    "port": 9222
  }
}
```

## トップレベルキー

| キー       | 型       | 既定値           | 概要                                                                 |
| :--------- | :------- | :--------------- | :------------------------------------------------------------------- |
| `iconPath` | `string` | muon既定アイコン | 静的アプリアイコンとして使うPNGファイルです。                        |

- `iconPath` は `.png` のみ受け付けます。相対パスは `muon.json` が置かれているディレクトリから解決されます。
- `iconPath` はWindows PE/NSIS、Linux desktop、起動時タイトルバーアイコンの共通ソースです。
  内部的には、 `asset://main/.muon/app-icon.png"` に配置されます。従って、既存アセットに同じエントリがある場合はビルドエラーになります。
- Windowsだけ、またはLinuxだけ別アイコンにしたい場合は、それぞれ `windows.resource.iconPath`、`linux.desktop.iconPath` をoverrideとして指定します。
- 実行中に通常ブラウザウインドウのタイトルバーアイコンだけを変更する場合は、`window.muon.browser.setTitleBarIcon()` を使用します。

## browserキー

| キー                                  | 型                                        | 既定値                    | 概要                                                                                     |
| :------------------------------------ | :---------------------------------------- | :------------------------ | :--------------------------------------------------------------------------------------- |
| `startPage`                           | `string`                                  | `"asset://main/index.html"` | 起動時に最初に読み込むURLです。                                                          |
| `profile`                             | `string`                                  | state profile             | Chromiumプロファイルを保存するディレクトリです。                                         |
| `initialWindowState`                  | `string`                                  | `"normal"`                | 起動時のウインドウ状態です。                                                             |
| `backgroundColor`                     | `string`                                  | `"system"`                | ページ読み込み前やページが背景色を指定しない場合のブラウザ背景色です。                   |
| `titleBarType`                        | `string`                                  | `"muon"`                  | 通常ブラウザウインドウのタイトルバー実装です。                                           |
| `contextMenu`                         | `object`                                  | `{"mode":"standard"}`     | ページ領域のネイティブコンテキストメニュー設定です。                                     |
| `initialTitleBarVisibility`           | `boolean`                                 | `true`                    | muonカスタムタイトルバーを起動時に表示するかどうかです。                                 |
| `keybind`                             | `object`                                  | `{}`                      | ブラウザ操作に割り当てるキーボードショートカットです。                                   |
| `allowUnsafeJavaScriptParentAccess`   | `readonly string[]`                       | `[]`                      | popupから親ページへのJavaScriptアクセスを許可するURLリストです。                         |

- `profile` に相対パスを指定した場合は、 `muon.json` からの相対パスとして解決されます。
  `profile` を明示しない場合は、ユーザーステートディレクトリの `<appId>/profile/` が使用されます。
- `initialWindowState` には `"normal"`, `"hidden"`, `"minimized"`, `"maximized"`, `"fullscreen"` を指定出来ます。
  ただし、最終的な表示状態はOSやウインドウマネージャーによって調整される場合があります。
- `backgroundColor` には `"system"` またはRGB 16進表記の `"RRGGBB"` / `"#RRGGBB"` を指定出来ます。
  `"system"` はOSの明暗設定が取得できる場合に黒または白として反映し、取得できない場合はCEFの既定値を使用します。
- `titleBarType` には `"muon"` または `"native"` を指定出来ます。
  `"muon"` はlibmuon-uiが提供するテーマ追従のタイトルバーを使用し、`"native"` はOS/ウインドウマネージャのネイティブ装飾を使用します。
  Linuxで`"native"`を指定した場合、X11ではウインドウマネージャーの装飾に任せますが、
  Waylandなどネイティブ装飾を使用できないと判断した場合は警告ログを出力し、`"muon"`相当へフォールバックします。
  `"muon"` タイトルバーでは、ページ内の任意の要素へCSSで `-webkit-app-region: drag` を指定すると、その領域をドラッグしてウインドウを移動出来ます。
  リンク、ボタン、入力欄など通常のページ操作を受け取る要素には `-webkit-app-region: no-drag` を指定してください。
- `contextMenu.mode` には `"standard"`, `"disabled"`, `"custom"` を指定出来ます。
  `"standard"` はCEF標準のコピー/ペーストなどのメニューを表示し、登録済みのmuonカスタム項目を追加します。
  `"disabled"` は標準項目とカスタム項目の両方を含めてネイティブコンテキストメニューを表示しません。
  `"custom"` はCEF標準項目を消し、登録済みのmuonカスタム項目だけを表示します。
- `initialTitleBarVisibility` は、通常ブラウザウインドウのタイトルバーを初期表示するかどうかを指定します。
  `false` を指定すると、起動直後はタイトルバーが非表示になります。
- 起動時のタイトルバーアイコンは、配布ビルド時に `iconPath` から生成されます。
  ページがfaviconを指定した場合、muonはCEFから通知されるfavicon URLを順に試し、取得と変換に成功した最初の画像をタイトルバーアイコンへ反映します。
  ページ遷移時、faviconが存在しない場合や取得・変換できない場合は、生成済みの初期タイトルバーアイコン、または内蔵muonアイコンへ戻ります。
  favicon URLの取得は通常のページリクエストと同じネットワーク制限の対象です。
  faviconのレスポンスは画像形式にかかわらず1 MiBまでです。PNGはさらに幅・高さが各256 px以下、総画素数が65,536以下である必要があり、制限を超えた候補は使用せず次の候補または初期アイコンへフォールバックします。
  この制限はタイトルバー／アプリケーション／トレイ用アイコンのデコード経路だけに適用され、ページ内の通常の`<img>`などはCEFの画像処理に委ねられます。
  `"muon"` タイトルバーではSVGなどブラウザが表示できる画像形式を使用出来ますが、`"native"` タイトルバーではPNGとして読み込める画像だけが使用されます。
  Linuxでの`"native"`の場合は、タイトルバーに関する指定が正しく反映されない場合があります。
- `keybind` では `devtools`, `reload`, `hardReload`, `fullscreen`, `zoomIn`, `zoomOut`, `resetZoom`, `recycle` を指定出来ます。
  値は `"ctrl+shift+i"` のように、修飾キーとキーを `+` で連結した文字列です。
  修飾キーには `shift`, `ctrl`/`control`, `alt`, `meta`/`cmd`/`command`/`super` を使用出来ます。
  キーには `f1` から `f24`, `a` から `z`, `0` から `9`, `plus`, `equal`, `minus`, `backspace`, `tab`, `enter`/`return`, `escape`/`esc`, `space`, `insert`, `delete`/`del`, `home`, `end`, `pageup`, `pagedown`, `left`, `right`, `up`, `down` を使用出来ます。
  空文字列、修飾キーだけの指定、同一ショートカットの重複は設定エラーになります。
- `allowUnsafeJavaScriptParentAccess` は安全性を下げる互換設定です。通常、この項目を指定する必要はありません。
  未指定または空配列の場合、popupから親ページへのJavaScriptアクセスは許可されません。
  この場合のpopupは `noopener` 相当の独立ウインドウとして開かれ、 `window.open()` は `null` を返します。
  ページ側で `noopener` または `noreferrer` を指定した場合も、許可リストの内容に関係なく `window.opener` は `null` になります。

## linuxキー

`linux.desktop` は配布ビルド用のLinux desktop entry metadataです。
この設定は `muon build` と `muon pack` のビルド時にだけ使われ、`muon-core` やlauncherへ埋め込まれる実行時設定からは除外されます。

```json
{
  "linux": {
    "desktop": {
      "desktopId": "com.example.my-app",
      "name": "My App",
      "comment": "Example app",
      "iconPath": "icons/app.png",
      "categories": ["Utility"],
      "startupNotify": true
    }
  }
}
```

| キー                      | 型         | 既定値                     | 概要                                                                       |
| :------------------------ | :--------- | :------------------------- | :------------------------------------------------------------------------- |
| `desktop.desktopId`       | `string`   | `appId`                    | `.desktop` ファイル名、`StartupWMClass`、Wayland app ID、X11 WM_CLASSです。 |
| `desktop.name`            | `string`   | `package.json`名           | ランチャーに表示されるアプリ名です。                                       |
| `desktop.comment`         | `string`   | `package.json.description` | desktop entryの`Comment`です。                                             |
| `desktop.iconPath`        | `string`   | `iconPath`またはmuon既定アイコン | Linuxターゲットだけで使用するPNGアイコンoverrideです。                    |
| `desktop.categories`      | `string[]` | `["Utility"]`              | desktop menu categoryです。                                                |
| `desktop.startupNotify`   | `boolean`  | `true`                     | desktop entryの`StartupNotify`です。                                       |

- `desktop.iconPath` は `.png` のみ受け付けます。入力PNGはビルド時に正規化され、Linux配布ディレクトリには `muon-desktop-icon.png` として配置されます。
- ポータブル配布物(.tar.gz)から起動した場合、`muon-bootstrap` は展開先の `<packageName>/<target>` 直下へCEFを準備し、CEFプロファイルを同じディレクトリ直下の `profile/` に置き、`~/.local/share/applications/<desktopId>.desktop` を生成または更新します。
  このdesktop entryの `Exec`, `TryExec`, `Icon` は、展開先ディレクトリ配下の絶対パスを指します。
- 同じ展開先で再起動した場合は準備済みCEFを再利用します。別のディレクトリへ展開した配布物から起動した場合は、その展開先ごとにCEF準備とdesktop entry更新が行われます。
- `muon pack --type deb` は `/usr/share/applications/<desktopId>.desktop` と `/usr/share/icons/hicolor/256x256/apps/<desktopId>.png` を生成します。
  debでインストールされたruntimeには `muon-install.json` が含まれ、`muon-bootstrap` はユーザーhomeへ新規desktop entryを作成しません。
  既存のmuon-managed user desktop entryがある場合だけ、`TryExec=/usr/bin/<packageName>` を持つdeb-aware entryへ更新します。
- 相対パスは、値を定義したファイルのディレクトリから解決されます。
  CLI/Vite optionはproject root、`muon.json` は設定ファイルのディレクトリです。
- Linuxアイコンの解決順は、CLI/Vite optionの `linuxDesktop.iconPath` または `--linux-icon`、`muon.json` の `linux.desktop.iconPath`、統一 `iconPath`、`project.json.iconPath`、muon既定アイコンです。
- Linux desktop metadataの解決順はフィールドごとに、CLI/Vite option、`muon.json` の `linux.desktop`、`package.json`、既定値です。

## windowsキー

`windows.resource` は配布ビルド用のWindows PE/NSIS resource metadataです。
この設定は `muon build` と `muon pack` のビルド時にだけ使われ、`muon-core` やランチャーへ埋め込まれる実行時設定からは除外されます。
`windows.codeSigning` も同じくビルド時だけ使われ、実行時設定へは埋め込まれません。

```json
{
  "windows": {
    "resource": {
      "iconPath": "icons/app.png",
      "productName": "My App",
      "fileDescription": "My App",
      "companyName": "Example Inc.",
      "version": "1.2.3",
      "copyright": "Copyright Example Inc."
    },
    "codeSigning": {
      "command": "signtool",
      "args": ["sign", "/fd", "SHA256", "{path}"],
      "targets": ["runtime", "launcher", "nsisInstaller", "nsisUninstaller"]
    }
  }
}
```

| キー                   | 型       | 既定値                     | 概要                                                                     |
| :--------------------- | :------- | :------------------------- | :----------------------------------------------------------------------- |
| `resource.iconPath`    | `string` | `iconPath`またはmuon既定アイコン | Windowsターゲットだけで使用するPNGアイコンoverrideです。                 |
| `resource.productName` | `string` | `package.json`名           | Windows version resourceの`ProductName`です。                            |
| `resource.fileDescription` | `string` | `package.json.description` | Windows version resourceの`FileDescription`です。                  |
| `resource.companyName` | `string` | `package.json.author`      | Windows version resourceの`CompanyName`です。                            |
| `resource.version`     | `string` | `package.json.version`     | `FileVersion`/`ProductVersion`です。固定値は4要素に正規化されます。      |
| `resource.copyright`   | `string` | `package.json.copyright`   | Windows version resourceの`LegalCopyright`です。                         |
| `resource.language`    | `number` | `1033`                     | version resourceとicon resourceのlanguage IDです。                       |
| `resource.codePage`    | `number` | `1200`                     | version resourceのcode pageです。                                        |
| `codeSigning.command`  | `string` | なし                       | 署名対象ファイルごとに実行する外部署名コマンドです。                     |
| `codeSigning.args`     | `string[]` | なし                     | 署名コマンドの引数です。`{path}` が必須です。                            |
| `codeSigning.targets`  | `string[]` | 全署名対象                | `runtime`, `launcher`, `nsisInstaller`, `nsisUninstaller` から選択します。 |

- `resource.iconPath` は `.png` のみ受け付けます。muonは、Windows PE/NSISが必要とする `.ico` ファイルをビルド時に自動生成します。
- 相対パスは、値を定義したファイルのディレクトリから解決されます。
  CLI/Vite optionはproject root、`muon.json` は設定ファイルのディレクトリ、`project.json` はproject rootです。
- Windowsアイコンの解決順は、CLI/Vite optionの `windowsResource.iconPath` または `--windows-icon`、`muon.json` の `windows.resource.iconPath`、統一 `iconPath`、`project.json.iconPath`、muon既定アイコンです。
- Windows resource metadataの解決順はフィールドごとに、CLI/Vite option、`muon.json` の `windows.resource`、`project.json`、`package.json`、既定値です。
  ただし `muon pack` で `--package-version` を指定した場合、`resource.version` では `package.json.version` の位置に `--package-version` の値を使用します。
  `--windows-version`、`muon.json`、`project.json` による明示的なWindows resource versionは、引き続き `--package-version` より優先されます。
- Windowsコードサイニングの解決順は、CLI/API option、Vite pluginの `build.windowsCodeSigning`、`muon.json` の `windows.codeSigning` です。
  `command` はmuonが実装する署名処理ではなく、CIや開発者環境から供給される `signtool` や署名用wrapper scriptを指定します。
  `args` の `{path}` は署名対象ファイルパス、`{target}` は `windows-amd64` などのmuon target、`{kind}` は署名対象種別に置換されます。
- `version` が `1.2.3` の場合、PE固定値とNSISの `VIProductVersion` / `VIFileVersion` は `1.2.3.0` になります。
  文字列版の `FileVersion` / `ProductVersion` には元の `1.2.3` が入ります。
- `muon build` はランチャーのconfig埋め込み後に `muon-builder resource` でPEファイルのリソースを直接更新します。
  コードサイニング署名は、このPE更新後に `runtime` と `launcher` に対して実行されます。
- `muon pack --type nsis` は、同じ解決済みmetadataからNSIS scriptへ `Icon`, `UninstallIcon`, `VIProductVersion`, `VIFileVersion`, `VIAddVersionKey` を出力します。
  setup本体と `Uninstall.exe` の表示情報を揃えるため、NSISについてはPE後処理ではなくNSIS directiveを使用します。
  `nsisInstaller` と `nsisUninstaller` のコードサイニングも、NSISの `!finalize` / `!uninstfinalize` を介して実行されます。
  NSISの `Name` / `DisplayName` / installer path / uninstall registry key / state削除先はWindows architecture別になりますが、`ProductName` などのWindows resource metadataと成果物ファイル名は同じmetadata規則を維持します。

## assetキー

| キー        | 型       | 既定値   | 概要                                                                 |
| :---------- | :------- | :------- | :------------------------------------------------------------------- |
| `sourcePath` | `string` | `assets` | `asset://` URLとして公開するアセットディレクトリまたはZIPファイルです。 |

- `sourcePath` を省略した場合は、実行ファイルと同じディレクトリにある `assets` が使用されます。
  相対パスは `muon.json` からの相対パスとして解決されます。
  ディレクトリを指定した場合はその内容が、ZIPファイルを指定した場合はZIP内の内容が `asset://main/...` として参照出来ます。

> 注釈: ここに挙げられていない `signature`, `salt` については、 `muon build` または `muon pack` 時に自動的に計算・挿入される値です。
> 解説は省略します。

## networkキー

| キー               | 型                 | 既定値          | 概要                                                                                 |
| :----------------- | :----------------- | :-------------- | :----------------------------------------------------------------------------------- |
| `allow`            | `readonly string[]` | `["asset://**"]` | 読み込みを許可するURLパターンのリストです。                                          |
| `authorizedOrigin` | `readonly object[]` | `[]`            | 指定オリジンから発生したリクエストに、追加のネットワークアクセスを許可します。       |

- `allow` はホワイトリストです。
  空配列を指定すると、ローカルアセットを含むすべてのネットワークアクセスが許可されなくなります。
  `data:` プロトコルのURLも例外ではないため、インラインデータURLを読み込む場合は、 `data:image/**` のようにMIMEタイプを絞ってください。
  `data:**` は `data:text/html,...` も含むため、信頼できない入力から生成されたHTMLが読み込まれると、任意のHTMLやJavaScriptの実行、UI偽装、許可済みネットワークへのアクセスにつながる可能性があります。
  URLパターンでは `*` と `**` を使用出来ます。
  `*` は `:`, `/`, `?`, `#` の区切りを越えず、 `**` は以降のすべての文字にマッチします。
  パターンの大文字小文字は区別されます。
- `authorizedOrigin` の各要素には `scheme` と `domain` を必ず指定し、必要な場合だけ `port` を指定します。
  - `scheme` と `domain` は小文字に正規化されます。
  - `domain` には `:`, `/`, `?`, `#`, `*` を含められません。
  - `port` を指定する場合は `1` から `65535` の整数である必要があります。
  - この設定は権限移譲に近い挙動になるため、信頼できる認証プロバイダーなど、リクエスト元として信頼できるオリジンだけを指定してください。

## pluginキー

| キー                         | 型                            | 既定値                 | 概要                                                                              |
| :--------------------------- | :---------------------------- | :--------------------- | :-------------------------------------------------------------------------------- |
| `path`                       | `string`                      | `"./plugins"`          | 外部プラグインファイルを探索するディレクトリです。                                |
| `mode`                       | `"validate" \| "simple"`      | `"validate"`           | プラグイン名前空間や関数の露出方式です。                                          |
| `pages`                      | `readonly string[]`           | `["asset://main/**"]`  | プラグイン名前空間や関数をページから参照可能にするURLのリストです。               |
| `plugins`                    | `readonly object[]`           | `[]`                   | 有効化するプラグインのリストです。                                                |
| `plugins[].name`             | `string`                      | なし                   | 有効化するプラグイン名です。                                                      |
| `plugins[].config`           | `readonly object`             | `{}`                   | プラグイン初期化時に渡す文字列key-value設定です。                                 |
| `plugins[name="internal"].config["fs.readFile.maxBytes"]` | `string` | `"67108864"` | `muon.fs.readFile` 1回あたりの読み取り上限をbyte単位で指定します。 |
| `plugins[].allow`            | `readonly string[]`           | なし                   | `simple` モードで公開する関数パスの許可リストです。                               |
| `plugins[].imports`          | `readonly object[]`           | なし                   | `validate` モードで使用するimport元ごとの許可リストです。                         |
| `plugins[].imports[].sources`  | `readonly string[]`         | なし                   | プロジェクトルートからの相対importerパスglobです。                                |
| `plugins[].imports[].packages` | `readonly string[]`         | なし                   | importerが属するNPMパッケージ名の完全一致リストです。                             |
| `plugins[].imports[].allow`    | `readonly string[]`         | なし                   | そのimportルールで許可するプラグイン関数パスglobです。                            |

- `path` に相対パスを指定した場合は、 `muon.json` からの相対パスとして解決されます。
- `mode` に `"validate"` を指定した場合、Viteなどのバンドラーが生成したcapability付きvirtual module importからだけプラグイン関数を呼び出せます。
  `"simple"` では、従来通り `window.muon` とその下の名前空間オブジェクトをページに公開します。
- `pages` は、ページにプラグインAPIブリッジを注入するかどうかだけを制御します。
  `"validate"` ではcapability呼び出し用の非列挙ブリッジ、`"simple"` では `window.muon` 階層が注入対象です。
  ページやサブリソースを実際に読み込めるかどうかは、別途 `network.allow` で許可する必要があります。
- `plugins` を省略した場合、プラグイン関数は公開されません。
  内蔵プラグインを有効化する場合は `name` に `"internal"` を指定します。
  外部プラグインを有効化する場合は、拡張子を除いたファイル名を `name` に指定します。
  例えば `muon_fs_dialogs_gtk3.so` を使う場合の `name` は `"muon_fs_dialogs_gtk3"` です。
  `"internal"` は予約名であり、外部プラグイン名としては使用出来ません。
  同じ `name` を複数回指定することは出来ません。
- `plugins[].config` は、そのプラグインへ初期化時に渡す設定テーブルです。
  未指定または `{}` の場合は空の設定として扱われます。
  keyは空文字列不可で、keyとvalueはいずれもNUL文字を含められません。
  valueに指定できるのは文字列だけです。空文字列や改行を含む文字列は指定できますが、object、array、number、boolean、nullは設定エラーになります。
  複数行の値やglob/正規表現の区切りなど、値の意味と解釈は各プラグインの責務です。
- 内蔵プラグインの `fs.readFile.maxBytes` は、 `muon.fs.readFile` 1回あたりの上限をbyte単位で指定します。
  値はASCII数字だけで構成される符号なし10進整数文字列で、文字列全体が `uint64_t` の範囲内である必要があります。先頭ゼロは使用できます。
  空文字列、符号、空白、小数点、指数表記、単位suffix、overflowは不正です。
  未指定時は64 MiB (`67108864` byte) です。`"0"` も有効で、空のrangeだけを許可します。
  不正値は既定値へフォールバックせず、 `fs.readFile.maxBytes must be an unsigned decimal byte count` で内蔵プラグインの初期化に失敗します。
  設定はプラグイン初期化時に確定し、実行中には変更されません。変更後の起動またはrecycleで反映されます。
  この設定は `readFile()` だけに適用され、 `readTextFile()` や複数操作を合計したquotaには適用されません。
- `plugins[].allow` は、`simple` モードでプラグインが持つ関数パスを `window` 階層に公開するためのホワイトリストです。
  `simple` モードでは必須で、`validate` モードでは指定できません。
  `muon.fs.*` のようなパターンを指定出来ます。
  `*` は `.` の区切りを越えず、 `**` は以降のすべての文字にマッチします。
  パターンの大文字小文字は区別されます。
  ページ側で関数を呼び出せるようにするには、 `plugin.pages` で対象ページへのAPIブリッジ注入も許可する必要があります。
- `plugins[].imports` は `validate` モード用のimport許可です。
  `validate` モードでプラグインエントリを指定する場合は必須かつ空配列不可で、`simple` モードでは指定できません。
  `sources` はVite project rootからの相対importerパスに対するglobで、`packages` はimporterから最寄りの `package.json` を探索して得た `name` との完全一致です。
  プロジェクトルート自身の `package.json` は `packages` 判定対象ではなく、通常のソースファイルとして扱われます。
  `sources` と `packages` は片方または両方を指定でき、両方指定した場合はいずれかに一致すれば許可されます。
  どちらも指定しないimportルールは設定エラーです。
- `plugins[].imports[].allow` は `validate` モードでそのimport元に許可する関数パスです。
  必須かつ空配列不可です。
  virtual moduleがexportできるのは具体的な関数名のみです。`muon.executor.*` のようなwildcardだけで許可しても、具体的なexportを作れないためVite側でエラーになります。

> 注釈: ここに挙げられていない `capabilities`, `signature`, `salt` については、 `muon build` または `muon pack` 時に自動的に計算・挿入される値です。
> 解説は省略します。

## logキー

| キー                 | 型       | 既定値     | 概要                                                             |
| :------------------- | :------- | :--------- | :--------------------------------------------------------------- |
| `level`              | `string` | `"info"`   | 全ログソースの基準ログレベルです。                               |
| `output.type`        | `string` | `"stderr"` | ログの出力先です。                                               |
| `output.path`        | `string` | なし       | `output.type` が `"file"` の場合に使用する出力ファイルパスです。 |
| `sources.muon`       | `string` | `"info"`   | muon本体のログレベルです。                                       |
| `sources.cef`        | `string` | `"warning"` | CEF/Chromium内部ログのログレベルです。                           |
| `sources.console`    | `string` | `"debug"`  | JavaScript console出力のログレベルです。                         |
| `sources.plugin`     | `string` | `"info"`   | ネイティブプラグイン出力のログレベルです。                       |

- ログレベルには `"debug"`, `"info"`, `"warning"`/`"warn"`, `"error"`, `"fatal"`, `"off"` を指定出来ます。
`off` を指定したソースのログは出力されません。
- `level` を指定すると、すべてのログソースの基準レベルがその値に揃います。
  そのうえで `sources` に `muon`, `cef`, `console`, `plugin` を指定すると、対象ソースだけを個別に上書き出来ます。
  `sources` だけを指定した場合も、未指定ソースは現在の `level` に揃えられてから個別上書きされます。
  そのため、既定値の `cef: "warning"` や `console: "debug"` を維持したい場合は、必要に応じて `sources` に明示してください。
- `output.type` には `"stdout"`, `"stderr"`, `"file"` を指定出来ます。
  POSIX環境では `"syslog"` も使用出来ます。
  Windows環境では `"debug"` と `"eventlog"` も使用出来ます。
- `output.type` が `"file"` の場合、 `output.path` は必須です。
- 相対パスは `muon.json` からの相対パスとして解決され、親ディレクトリは必要に応じて作成されます。
  ファイル出力は追記で行われます。
- `output.type` が `"file"` 以外の場合、 `output.path` を指定すると設定エラーになります。

## cdpキー

| キー     | 型        | 既定値 | 概要                                             |
| :------- | :-------- | :----- | :----------------------------------------------- |
| `enable` | `boolean` | `false` | Chrome DevTools Protocolを有効化します。         |
| `port`   | `number`  | `9222` | DevTools Protocolの待ち受けポート番号です。      |

- `cdp.enable` を `true` にすると、外部のDevToolsやCDPクライアントから接続できるようになります。
  開発・デバッグ用の設定であり、配布ビルドでは必要な場合だけ有効化してください。
- `port` は `1024` から `65535` の整数である必要があります。

## bootstrapキー

| キー                   | 型       | 既定値     | 概要                                                                                                  |
| :--------------------- | :------- | :--------- | :---------------------------------------------------------------------------------------------------- |
| `defaultVersionPolicy` | `string` | `"tested"` | `muon-bootstrap.ini` に `versionPolicy` が保存されていない場合に使うCEF version policyです。 |

> 注釈: ここに挙げられていない `appId` については、 `muon build` または `muon pack` 時に自動的に計算・挿入される値です。
> 解説は省略します。

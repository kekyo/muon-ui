# muon Viteプラグインリファレンス

muon Viteプラグインの引数 `options` は省略可能で、省略時は開発起動と配布用ビルドのどちらも既定動作を使用します。

```ts
import { defineConfig } from 'vite';
import muon from 'muon-ui/vite';

export default defineConfig({
  plugins: [
    muon({  // muon Viteプラグインのオプション
      build: {
        targets: ['linux-amd64', 'windows-amd64'],
        outputRoot: 'release',
        appName: 'my-app',
      },
    }),
  ],
});
```

## rootキー

| キー             | 型                    | 既定値                      | 概要                                                                 |
| :--------------- | :-------------------- | :-------------------------- | :------------------------------------------------------------------- |
| `muonPath`       | `string`              | 同梱muonランタイム          | 開発起動で使用するmuon-coreランタイムディレクトリです。              |
| `cefPath`        | `string`              | muon-builderの自動取得      | 開発起動で使用するCEFディレクトリ、またはCEF archive rootです。      |
| `stagePath`      | `string`              | `".muon/<public-target>"`   | 開発起動用にmuonランタイムを配置するディレクトリです。               |
| `enableDebugger` | `boolean`             | `true`                      | 開発起動時にCDP、`F12` のmuon DevToolsキーバインド、`Ctrl+F12` のリサイクルキーバインドを有効化します。 |
| `exitWithServer` | `boolean`             | `true`                      | muon-coreが通常終了した時にVite dev serverも終了するかどうかです。   |
| `dev`            | `object`              | `{}`                        | Vite開発起動のmuonプロセスだけに適用する上書き設定です。             |
| `pluginAccess`   | `false \| object`     | `muon.json` の `plugin` 設定 | プラグインAPIの露出方式とvirtual module importの上書き設定です。     |
| `build`          | `boolean \| object`   | `true`                      | `vite build` 後に配布用ディレクトリを生成するかどうか、または生成時のオプションです。 |

- `muonPath`, `cefPath`, `stagePath`, `open`, `enableDebugger`, `exitWithServer` は `vite dev` に影響します。
  `muon run` は `muonPath`, `cefPath`, `stagePath`, `enableDebugger` と `build.configPath` を読み取り、 `open` と `exitWithServer` は無視します。
  `vite build` ではこれらの開発起動用オプションは無視されます。
- `build: false` は、`--assets` を省略した `muon run` のVite-backed起動ではエラーになります。
  `--assets` を明示した場合はVite-backed起動を使用しないため、指定アセットを従来どおり起動します。
- `muonPath`, `cefPath`, `stagePath` に相対パスを指定した場合は、Vite project rootからの相対パスとして解決されます。
- `muonPath` を省略した場合は、インストール済みのmuonパッケージに同梱された `runtime/<public-target>` を使用します。
- `cefPath` を省略した場合は、muon-builderが `muonPath` のランタイム情報を元に、テスト済みのCEF artifactをダウンロードしてキャッシュします。
- `stagePath` を省略した場合は、Vite project root配下の `.muon/<public-target>` が使用されます。
- `enableDebugger` を有効にした場合、開発起動用の上書き設定でCDPが有効化され、muon DevToolsを `F12` で開き、muonを `Ctrl+F12` でリサイクル再起動できるようになります。
  配布ビルドでmuon DevToolsを有効化したい場合は、Viteプラグイン引数ではなく `muon.json` の `cdp` や `browser.keybind` を設定します。
- `exitWithServer` を省略または `true` にした場合、muon-coreが通常終了するとVite dev serverも終了します。
  muonのリサイクル再起動ではVite dev serverは終了しません。

## devキー

`dev.config` は、`vite dev` で起動するmuonプロセスだけにアプリケーション `config` の値を渡します。
これらの値は、プロジェクトの `muon.json` より後に読み込まれる開発用生成設定へ書き込まれます。
従って、同じキーはプロジェクト設定を上書きし、その他のキーは追加されます。

```ts
import { defineConfig } from "vite";
import muon from "muon-ui/vite";

export default defineConfig({
  server: {
    proxy: {
      "/api": {
        target: "http://127.0.0.1:5000",
      },
    },
  },
  plugins: [
    muon({
      dev: {
        config: {
          apiBaseUrl: "/api",
        },
      },
    }),
  ],
});
```

```json
{
  "config": {
    "apiBaseUrl": "https://api.example.com"
  }
}
```

アプリケーションコードは、`muon.environments.getConfigValues()` で選択された値を取得します。
`dev.config` は `vite build`、`muon run`、配布物、インストール後のパッケージでは無視され、これらの起動形式では通常の `muon.json` の値を使用します。
`dev.config` の値はすべて文字列で、rendererコードから参照できるため、秘密情報を含めないで下さい。

## Viteサーバーのみを起動する

muonを起動せず、通常のVite dev serverのみを起動する場合は、Viteの引数終端を表す `--` より後ろに `--no-muon` を指定します。

```console
vite dev -- --no-muon
```

ポートなどのViteオプションは `--` より前に指定します。`--` より後ろの引数はViteオプションとして解釈されません。

```console
vite dev --port 3000 -- --no-muon
```

`--no-muon` は `vite dev` の開発用ランタイム準備とmuon起動のみを無効化し、`muon({ open: true })` よりも優先されます。virtual module、watch設定、muon設定の読み込みなど、muon Viteプラグインのその他の機能は有効なままで、`.gitignore` のmuon出力項目も更新されます。`vite build` の動作には影響しません。

`package.json` のscriptが `"dev": "vite dev"` の場合、npmからはViteへ引数終端も渡すため、次のように実行します。

```console
npm run dev -- -- --no-muon
```

Viteサーバーのみを起動するscriptを用意する場合は、引数終端をscript側に含められます。

```json
{
  "scripts": {
    "dev:vite": "vite dev --"
  }
}
```

```console
npm run dev:vite -- --no-muon
```

## pluginAccessキー

`pluginAccess` は、`muon.json` の `plugin` 設定と同じ形で、Vite側から一部を上書きするための設定です。
省略した場合は `muon.json` の `plugin` 設定をそのまま使用し、`plugin.mode` の省略時は `"validate"` として扱います。
`validate` モードでは `window.muon` は公開されず、プラグイン関数は許可されたvirtual module importからだけ呼び出せます。

```ts
muon({
  pluginAccess: {
    mode: "validate",
    pages: ["asset://main/**"],
    plugins: [
      {
        name: "internal",
        imports: [
          {
            sources: ["src/native/**"],
            allow: ["muon.executor.spawn"],
          },
          {
            packages: ["@example/trusted-muon-helper"],
            allow: ["muon.executor.spawn"],
          },
        ],
      },
    ],
  },
});
```

| キー                         | 型                            | 既定値                 | 概要                                                                              |
| :--------------------------- | :---------------------------- | :--------------------- | :-------------------------------------------------------------------------------- |
| `mode`                       | `"validate" \| "simple"`      | `plugin.mode`          | プラグイン名前空間や関数の露出方式です。                                          |
| `pages`                      | `readonly string[]`           | `plugin.pages`         | プラグイン名前空間や関数をページから参照可能にするURLのリストです。               |
| `plugins`                    | `readonly object[]`           | `plugin.plugins`       | 有効化するプラグインとimport許可のリストです。                                    |
| `plugins[].name`             | `string`                      | なし                   | 有効化するプラグイン名です。                                                      |
| `plugins[].allow`            | `readonly string[]`           | なし                   | `simple` モードで公開する関数パスの許可リストです。                               |
| `plugins[].imports`          | `readonly object[]`           | なし                   | `validate` モードで使用するimporterごとのcapability import許可リストです。        |
| `plugins[].imports[].sources`  | `readonly string[]`         | なし                   | Vite project rootからの相対importerパスglobです。                                  |
| `plugins[].imports[].packages` | `readonly string[]`         | なし                   | importerが属するNPMパッケージ名の完全一致リストです。                             |
| `plugins[].imports[].allow`    | `readonly string[]`         | なし                   | そのimporterに許可するプラグイン関数パスglobです。                                |

- 各項目は、`muon.json` の `plugin` と同様です。未定義の既定値は、`muon.json` の各項目にフォールバックします。

## buildキー

`build` に `false` を指定すると、Viteの通常ビルドだけを実行し、muon配布用ディレクトリの生成を無効化します。
この状態では `muon build` と `muon pack` もエラーになり、配布用ビルドは行われません。
`build` にオブジェクトを指定すると、 `vite build` 後のmuon配布用ビルドに追加オプションを渡せます。
`build` に `true` を指定した場合、または省略した場合は、 `{}` 相当として扱われます。

| キー               | 型                  | 既定値                         | 概要                                                                            |
| :----------------- | :------------------ | :----------------------------- | :------------------------------------------------------------------------------ |
| `targets`          | `readonly string[]` | 全対応ターゲット               | ビルド対象の公開ターゲットIDのリストです。                                      |
| `allTargets`       | `boolean`           | `targets` 省略時は `true` 相当 | インストール済みパッケージが対応する全ターゲットをビルドするかどうかです。      |
| `appName`          | `string`            | `package.json` の `name`      | アプリケーションランチャーのファイル名です。                                    |
| `appId`            | `string`            | `package.json` の `name`      | ランタイムアプリ識別子のbase IDです。Windowsターゲットでは `<appId>.<arch>` が埋め込まれます。 |
| `outputRoot`       | `string`            | `"."`                          | `dist-muon/linux-amd64/` のようなターゲット別出力ディレクトリを作成する親ディレクトリです。 |
| `configPath`       | `string`            | 自動探索                       | ランタイムとランチャーに埋め込むmuon設定ファイルです。                          |
| `iconPath`         | `string`            | `muon.json`またはmuon既定アイコン | 静的アプリアイコンとして使うPNGファイルです。                                |
| `distributionFiles` | `readonly string[]` | `package.json` の `files`        | 配布ディレクトリ直下へ追加コピーするファイルリストです。                    |
| `windowsResource`  | `object`            | `windows.resource`             | Windows launcherとNSIS installer/uninstallerに埋め込むresource metadataです。   |
| `windowsCodeSigning` | `object \| false`  | `windows.codeSigning`          | Windows実行ファイルに対する外部コードサイニングコマンドです。              |
| `linuxDesktop`     | `object`            | `linux.desktop`                | Linux desktop entryとicon用metadataです。                                      |
| `packageDirectory` | `string`            | インストール済みmuonパッケージ | `runtime/` と `native/` を含むmuonパッケージディレクトリです。                  |

- `targets` と `allTargets` をどちらも省略した場合は、インストール済みmuonパッケージが対応する全ターゲットを生成します。
  `allTargets` が `true` の場合、 `targets` よりも優先されます。
  `targets` には `linux-amd64`, `linux-armhf`, `linux-arm64`, `windows-i686`, `windows-amd64` のいずれかを指定出来ます。
- `appName` を省略した場合は、 `package.json` にある `name` から生成します。
  `name` が存在しない場合は `muon-app` を使用します。
  scope付きパッケージ名ではscopeを除いた名前を使用し、ランチャー名として使えない文字は `-` に正規化されます。
  Windowsターゲットでは `.exe` が自動的に付与されます。
- `appId` を省略した場合も、 `package.json` にある `name` からbase IDを生成します。
  `@scope/name` は `scope.name` になり、その他の使用できない文字は `-` に正規化されます。
  Linuxターゲットでは生成された値をそのまま `launcher.appId` として `muon-core` とランチャーに埋め込みます。
  Windowsターゲットでは `windows-amd64` に `<appId>.amd64`、`windows-i686` に `<appId>.i686` を埋め込みます。
- `outputRoot` と `configPath` に相対パスを指定した場合は、Vite project rootからの相対パスとして解決されます。
  `configPath` を省略した場合は、Vite project rootから `muon.json5`, `muon.jsonc`, `muon.json` の順に探索します。
  設定ファイルが存在しない場合は `{}` 相当として扱います。
- `iconPath` は Vite project rootからの相対パスとして解決されます。
  `windowsResource.iconPath` と `linuxDesktop.iconPath` は、それぞれのターゲットだけに適用されるoverrideです。
- `distributionFiles` を省略した場合は、 `package.json` にある `files` を候補リストとして使用します。
  指定した場合は、 `package.json` の `files` より優先され、空配列は追加ファイルなしとして扱われます。
  候補パスは相対パスだけを受け付け、絶対パスは使用できません。
  コピーされるのは通常ファイルのみで、コピー先はターゲット別配布ディレクトリ直下の `basename` です。
  ディレクトリ、glob、npm の暗黙include/excludeは展開しません。
  `asset.sourcePath` として解決されたパス配下、`node_modules`、`.git`、出力先配布ディレクトリ配下は除外されます。
  同じ `basename` へコピーされる候補が複数ある場合はエラーになります。
- Viteプラグイン経由のビルドでは、Viteの `build.outDir` がアセット元として使用され、ZIP内のアセットには `main/` プレフィックスが付きます。
  そのため、ビルド後のアセットは `asset://main/` から参照出来ます。
- `windowsResource` は `muon.json` の `windows.resource` と同じキーを受け付け、CLIの `--windows-*` オプションと同じ優先度で扱われます。
- `windowsCodeSigning` は `muon.json` の `windows.codeSigning` と同じキーを受け付け、`false` を指定すると `muon.json` の署名設定を無効化します。
- `linuxDesktop` は `muon.json` の `linux.desktop` と同じキーを受け付け、CLIの `--linux-*` オプションと同じ優先度で扱われます。

> 注釈: `packageDirectory` については、テストやパッケージ検証向けの引数です。
> 解説は省略します。

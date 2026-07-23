# muonを始める

早速muonでアプリケーション（muonアプリ）を作ってみましょう。
新規にアプリケーションを開発する場合だけでなく、既存のウェブアプリケーションをmuonアプリ化するのも非常に簡単です。

これを明らかにするため、まずは、ごく一般的なウェブアプリケーションプロジェクトを用意する事から始めましょう。
例えば、 [Viteのテンプレート](https://vite.dev/guide/) を使って、"my-muon-app" を作りましょう:

```bash
npm create vite@latest my-muon-app -- --template react-ts
```

ここではReact+TypeScriptを選択しましたが、もちろん他の選択肢でもOKです。
TypeScriptは使用したほうが良いでしょう。理由は後で説明します。

そして、以下のコマンドで必要なパッケージのインストールを行って、アプリケーションを実行します:

```bash
cd my-muon-app
npm install
npm run dev
```

これで、Viteの開発サーバーが起動するので、リンクをクリックしてブラウザでページを表示出来ます。
まだmuonは導入していません。素のViteプロジェクトです。

このプロジェクトにmuonを導入します。必要な作業は:

1. muonパッケージをインストール
2. muon Viteプラグインを構成

だけです。しかも、それぞれ非常に簡単です。

## muonパッケージをインストール

muonパッケージ(正式名: `muon-ui`)は、 `devDependencies` にインストールします:

```bash
npm install -D muon-ui
```

muonパッケージには、以下の要素を含みます:

- muon CLI
- muon Viteプラグイン
- muon内蔵プラグインのTypeScript型定義
- プラットフォーム別のmuonバイナリアセット

CEF本体バイナリは、NPMパッケージに含まれません。CEFは必要になった時点で、公式CDNからダウンロードされます。

## muon Viteプラグインを構成

muonパッケージを導入しただけでほぼ準備は整っていますが、muonのViteプラグインを有効化して、
HMR (Hot Module Replacement) とmuonアプリのビルドを使用出来るようにします。

HMRを知らない人に簡単に説明すると、開発中にViteが擬似的なサーバーとなって、ブラウザにページを表示可能にし、かつ、ページを編集した時にほぼリアルタイムで表示を更新してくれます。
つまり、編集結果を自動的にプレビュー表示する、非常に便利な機能です。

muonのViteプラグインはHMRに対応していて、ブラウザの代わりにmuonを起動して、muonアプリ上でHMRを機能させます。
これを有効化するために、`vite.config.ts` に以下のコードを追加します:

```ts
import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
import muon from 'muon-ui/vite'

export default defineConfig({
  plugins: [
    react(),  // Reactプラグイン
    muon(),   // muonプラグイン (追加する)
  ],
})
```

`defineConfig()` 引数の `plugins` 配列に、`muon()` を加えて下さい。これでmuon Viteプラグインが有効化されます。これで準備は完了です。

いよいよmuonを起動します:

```bash
npm run dev
```

で、muonのウインドウとあなたのページが表示されるはずです!

![Get started](../../images/get-started.png)

ページのソースコードを変更して、ブラウザで表示させていた時と遜色なく、HMRが機能することを確認してみて下さい。
例えば、 `src/App.tsx` 内の `<h1>Get started</h1>` の行を `<h1>Get started with muon!</h1>` に書き換えて保存すれば、
再起動すること無く瞬時にmuonウインドウ側の表示も書き換わるはずです。

そして、このページの中央に配置されてるカウンタボタン "Count is 0" をクリックすると、カウント値が増加することが確認できるはずです。
これで、ViteテンプレートのReactが正しく動作していることが確認出来ます。

また、 "Explore Vite" のようなボタンをクリックした場合、新たなウインドウが開いて、以下のような「がっかり」するページが表示されます:

![Forbidden](../../images/forbidden.png)

そして、これこそが、muonの特徴である、ネットワークアクセスのホワイトリストフィルタが機能している証拠です。
このボタンは、Viteの公式サイト(`https://vite.dev/`)を表示しようとしますが、muonの既定では「ローカルのアセットのみ」アクセスが許可されているので、
それ以外のサイトコンテンツへのアクセスが遮断されているのです。
このホワイトリストの指定方法は別章で詳しく示します。

また、`F12` キーでmuon DevToolsを起動出来ます:

![muon DevTools](../../images/devtools.png)

CDP (Chrome DevTools Protocol) も有効化されているので、Playwrightで操作したりvscodeでデバッグが可能です（詳しくば別章を参照）。

`Ctrl+F12` キーでmuonをリサイクル再起動出来ます。リサイクル再起動は、 `muon.json` の変更など、HMRで反映できない更新を行った場合に使用出来ます。

## 配布用ビルド

muonアプリの実装がある程度出来上がってくると、これをどのようにユーザーに配布できるか、ということが気になってくるでしょう。
muonを使っていれば、配布用のファイル群を生成するのも簡単ですが、その前に、アプリの名称やクレジットなどを追加する必要があります。

これらは NPMパッケージの慣例に習って、 `package.json` に記述すれば、それが反映されます。
例えば:

```json
{
  "name": "my-muon-app",
  "version": "0.1.0",
  "description": "First time muon app.",
  "files": [
    "README.md",
    "LICENSE",
    "dist"
  ],
  // :
  // :
}
```

のように記述することで、muonアプリの名称・バージョン・説明を反映させることが出来ます。

`files` に通常ファイルを指定すると、条件を満たすものは配布ディレクトリ直下へ追加コピーされます。
上記の例では `README.md` と `LICENSE` が含まれます。
`dist` のようにアセット元として解決されたパスは追加コピー対象から除外されます。

準備が出来たら `npm run build` を実行すると、Viteの通常ビルドに続いてmuon配布用ディレクトリも生成されます。

```bash
npm run build
```

> 注釈: これは `package.json` の `scripts` に定義された `vite build` の別名です。

Viteの `build.outDir` に出力されたファイル群は `assets.zip` にまとめられます。
既定では、muonがサポートする全ターゲットをビルドし、`dist-muon/` ディレクトリ配下に出力されます:

```text
dist-muon/
├── linux-amd64/
│   ├── assets.zip
│   ├── :
│   └── vite-project
├── linux-arm64/
│   ├── assets.zip
│   ├── :
│   └── vite-project
├── linux-armhf/
│   ├── assets.zip
│   ├── :
│   └── vite-project
├── windows-amd64/
│   ├── assets.zip
│   ├── :
│   └── vite-project.exe
└── windows-i686/
    ├── assets.zip
    ├── :
    └── vite-project.exe
```

ビルドするターゲットや出力先を細かく指定したい場合は、Viteプラグインの引数 `build` で指定出来ます:

```ts
import { defineConfig } from 'vite';
import muon from 'muon-ui/vite';

export default defineConfig({
  plugins: [
    muon({
      build: {  // ビルドオプションの指定
        targets: ['linux-amd64', 'windows-amd64'],
        iconPath: 'icons/app.png',
        distributionFiles: ['README.md', 'LICENSE'],
        linuxDesktop: {
          name: 'My App (linux)',
        },
        windowsResource: {
          productName: 'My App (windows)',
        }
      },
    }),
  ],
});
```

`build.targets` に指定可能なターゲット名は、以下のとおりです:

- Linuxターゲット: `linux-amd64`, `linux-armhf`, `linux-arm64`
- Windowsターゲット: `windows-i686`, `windows-amd64`

既定では、アプリケーションの実行ファイル名は `package.json` の `name` から生成され、スコープ付きパッケージ名の場合はスコープを除いた名前を使用します。
また、`build.iconPath` にアイコンとなるPNGフォーマットの画像を配置しておけば、muonアプリのアイコンとして機能するようになります。
`build.distributionFiles` を指定した場合は `package.json` の `files` より優先され、空配列を指定すると追加コピーを行いません。
上記例のように、LinuxターゲットやWindowsターゲットに固有の指定を追加することも出来ます。

## パッケージ生成

muonは、インストーラーやアーカイブなどの配布用パッケージも簡単に生成出来ます。

前節の配布用ビルドの準備が出来ていれば、 `muon pack` コマンドを実行するだけです:

```bash
npx muon pack
```

> 注釈: 上記を思い出すのが難しければ、 `package.json` の `scripts` に `pack` エントリを追加しても良いと思います。私はそうしています :)

`muon pack` は `muon build` と同じビルドシーケンスで配布用ディレクトリを生成してから、指定した形式にパッケージ化します。

- muon Viteプラグインがある場合は `vite build` を実行して muonアプリを生成します。
- muon Viteプラグインが無い場合は `vite build` を実行せず、既に存在するアセットを使用します。

その後、指定した形式ごとに `artifacts/` へ最終配布物を出力します。
`deb` のパッケージツリーや `nsis` の `.nsi` スクリプトなど、パッケージ生成中の作業ファイルは `.muon/pack/` 配下に生成されます。

`muon pack` にオプションを指定しない場合は、すべてのターゲットに対応したファイル群を生成します。
以下にオプション指定の例を示します:

```bash
npx muon pack --target windows
npx muon pack --target amd64
npx muon pack --type tgz
npx muon pack --type nsis
npx muon pack --target linux-amd64 --type tgz,deb 
npx muon pack --target windows-amd64 --type nsis
```

- ターゲットは `--target` または `--all` で指定でき、未指定時はViteプラグインの `build` 設定を使用します。
  muon Viteプラグインが無い場合や未指定時は、すべての対応ターゲットをパッケージ候補にします。
  完全なターゲット名に加えて、プラットフォーム名の `linux`, `windows`、アーキテクチャ名の `amd64`, `arm64`, `armhf`, `i686` も指定出来ます。
- `--type` は `zip`, `tgz`, `tar.gz`, `deb`, `nsis` をカンマ区切りまたは複数指定出来ます。
  省略時は `zip`, `tgz`, `deb`, `nsis` のすべてを対象にします。
- `zip` はWindowsターゲットだけで使用でき、各 `<packageName>/<target>` ディレクトリを含むポータブルZIPです。
- `tgz` または `tar.gz` はLinuxターゲットだけで使用でき、各 `<packageName>/<target>` ディレクトリを含むポータブルgzip圧縮tarです。
  `tgz` は `tar.gz` の別名で、出力ファイル名は常に `*.tar.gz` です。
  ポータブル配布物にはCEFバイナリを含めず、初回起動時に展開先の `<packageName>/<target>` 直下へCEFを準備します。
  プロファイルも同じディレクトリ直下の `profile/` が使われます。
- `deb` はLinuxターゲットだけで使用でき、実行環境のPATH上に `dpkg-deb` が必要です。
  Debian/Ubuntuでは、 `sudo apt install dpkg-deb` でインストール出来ます。
- `nsis` はWindowsターゲットだけで使用でき、実行環境のPATH上に `makensis` が必要です。
  Debian/Ubuntuでは、 `sudo apt install nsis` でインストール出来ます。
  Windows環境では [Nullsoft Scriptable Install System](https://nsis.sourceforge.io/Main_Page) からダウンロード出来ます。
- 指定した形式とターゲットに対応しない組み合わせはスキップされ、有効な組み合わせだけが生成されます。
  例えば `muon pack --type nsis` はWindowsターゲットのNSISだけを生成し、Linuxターゲットは生成しません。
- CLIオプションは、 `muon build` で指定できる `--icon`, `--windows-icon`, `--linux-desktop-id` などと同様に指定可能です。
  `packageName`, `version`, `description`, `author` は `package.json` を既定値に使い、CLIオプションで上書き出来ます。
- `muon pack` では `--package-version` の指定値がWindows resource versionの `package.json.version` fallbackとしても使われます。
  例えば [screw-up](https://github.com/kekyo/screw-up/) 以降でGit由来のバージョンを適用する場合は、 `npx muon pack --package-version "$(screw-up format -e '{version}')"` のように指定出来ます。

---

## 補足: muonのアーキテクチャ

ここまでの解説で、muonアプリを実装する基本は伝わったと思います。
これを読んでいる読者はElectronも触ったことがあるかも知れないので、ここで両者の内部構造の違いを簡単に説明します。

決定的な違いは、開発者が記述するアプリケーションコードの起点と、UIのライフサイクルを管理する層です。

- Electronは[Chromiumのマルチプロセスモデル](https://www.electronjs.org/docs/latest/tutorial/process-model)を継承しています。
  開発者が用意したエントリポイントはNode.js環境のmain processで実行され、そこで`BrowserWindow`を作成して、ページをChromiumのrenderer processへロードします。
  ページ内JavaScriptは、通常preload scriptを介して公開されたIPC APIでmain processと連携します。
- muonでは、C++で実装されたmuon coreがCEFを初期化します。
  CEFのbrowser processがアプリケーションのライフサイクル、ウインドウ、native plugin runtimeを管理し、開発者が記述するアプリケーションの主な起点はrenderer processへロードされるページです。
  native pluginの関数はCEF IPCを経由する非同期APIとしてページへ公開されるため、開発者がNode.jsのmain processを用意する必要はありません。
- `node.project`を構成した場合だけ、browser process内のnative Node pluginが有効になります。
  最初の`importModule()`で別プロセスのNode.js sidecarを遅延起動し、以降の呼び出しを中継します。
  sidecarはUIを生成または所有せず、処理のオフロードやNode.js ecosystemの機能を利用するためのオプションです。

Electron:

```mermaid
flowchart LR
  Main["Main process<br/>Node.js + Electron APIs"]
  Renderer["Chromium renderer process(es)<br/>Page JavaScript"]
  Main -->|"Create BrowserWindow<br/>and load pages"| Renderer
  Main <-->|"IPC, usually exposed through preload"| Renderer
```

muon:

```mermaid
flowchart LR
  subgraph Browser["CEF browser process"]
    Core["muon core<br/>C++ / CEF"]
    Runtime["Native plugin runtime"]
    NodePlugin["Optional native Node plugin"]
    Core --- Runtime
    Runtime ---|"Plugin ABI / tra-ffic"| NodePlugin
  end
  subgraph Renderers["CEF renderer process(es)"]
    Page["Application page<br/>JavaScript"]
  end
  Node["Optional Node.js sidecar<br/>Separate process"]
  Core -->|"Create window and load page"| Page
  Page <-->|"CEF IPC / asynchronous facades"| Runtime
  NodePlugin <-->|"Lazy start / sidecar IPC"| Node
```

開発者から見ると、muonアプリの主な起点は読み込まれたページであり、一般的なViteやReactのウェブアプリケーションと同じ開発手法を使用出来ます。
内部的には設定、policy、plugin runtimeなどの初期化がウインドウ作成に先行しますが、開発者がこれらをmain process用JavaScriptとして実装する必要はありません。
このページ中心のモデルにより、ウェブアプリ開発を知っていればmuonアプリ開発を始められます。

これは、すべての処理をrenderer processへ配置しなければならない、という意味ではありません。
別のbackendを採用しない場合、ドメインロジックやデータリポジトリ実装もページのbundleに含まれますが、必要に応じてnative plugin、[Node.js sidecar](./nodejs-sidecar.md)、リモートWeb APIへ責務を分離出来ます。
Node.js sidecarを使用すれば、UIと関係のないNode.jsコードを別プロセスの通常のNode.jsプロジェクトとして構成出来ます。

大きなbackend実装をmuonアプリへ同梱する前に、クラウドサービスやホストされた仮想マシン上のWeb APIとして実装すべきかも検討してください。
ページからWeb APIへアクセスする場合は、通常のCORSなどのブラウザ要件に加えて、必要なendpointだけを[`network.allow`](./external-network.md)で許可します。
`network.allow`が制御するのはCEFによるページのnetwork requestであり、Node.js sidecar内のnetwork accessには適用されません。

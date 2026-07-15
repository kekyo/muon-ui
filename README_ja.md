# muon

CEFをバックエンドで使用する、マルチプラットフォームGUIアプリケーション基盤

![muon](./images/muon-120.png)

[![Project Status: WIP – Initial development is in progress, but there has not yet been a stable, usable release suitable for the public.](https://www.repostatus.org/badges/latest/wip.svg)](https://www.repostatus.org/#wip)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![npm version](https://img.shields.io/npm/v/muon-ui.svg)](https://www.npmjs.com/package/muon-ui)

---

[(English language is here)](./README.md)

## これは何?

あなたは、古くなってしまったネイティブGUIアプリケーションを、どうにかして最新のモダン化されたアプリケーションに更新したいと考えたことはありますか？
アプリケーションのリプレースメントは非常に複雑で、私達をいつも悩ませて来ました。

その原因は多岐に渡るため、「この手法を使えば簡単に解決する！」というような、魔法のソリューションは存在しません。
それでもなお、できるだけ低いコストで現代的に刷新したいとは、誰しもが考えることでしょう。

muon (ミューオン, /ˈmjuːɒn/) は、CEF (Chromium Embedded Framework) を使用する、マルチプラットフォームGUIアプリケーション基盤です。
あなたは恐らく、 [Electron](https://www.electronjs.org/) に代表される同種のプロジェクトをいくつか聞いたことがあるかもしれません。
 
乱暴に言えば、muonはそれらと同じポジションの基盤ソフトウェアです。
つまり、ローカルで動作するGUIアプリケーションを、ウェブアプリケーションとして実装出来るソフトウェアです。

muonを使用すれば、最も発達し、かつ最も使われているブラウザ環境のUIフレームワーク、例えばReactやVueを使用して、
ネイティブアプリケーションを作ることが出来ます。

![vscode](./images/vscode.png)

では、muonは先発の基盤ソフトウェアとどう違うのでしょうか？

それは、外部のウェブリソースへのアクセスをホワイトリスト方式（基本的にdeny）とすることで、
ローカル環境でのGUIアプリケーション開発に特化しているということです。

CEFを使うのに、世界中のウェブサイトにアクセス出来ないのはもったいない！ と思うかもしれません。
ですが、既存のソリューションが非常に複雑で扱いづらくなっている最大の問題点はそこにあると考えています。

また、muonは外部リソースに全くアクセス出来ないわけではなく、あくまでホワイトリスト方式なので、
許可されたウェブサイトにはアクセス可能です。

そして、ネイティブライブラリとの相互運用を実現する、完全に非同期処理に対応したプラグインシステムも搭載しています。
これらも事前構成に基づいて使用可能になるため、使われない機能を野放しにすることはありません。

CEFベースなので、Chromiumで想定されるWebGL, WebGPU, WebAssembly, Web worker, Local storageなども当然使用出来ます。
DevToolsを表示させることも可能です。CDP (Chrome DevTools Protocol) も使用できるため、vscodeなどでデバッグしたり、Playwrightを接続して操ることも可能です。
Chromium/Chromeから、 `chrome://inspect/` でリモートDevToolsを使用することも可能です。

つまり、GTK/Qt/Windowsなどのネイティブアプリケーションをウェブアプリケーションに移行する際に発生する、大きな問題点の一つをわかりやすく除外することで、
ウェブベース技術のエコシステムを使って現代的なローカルGUIアプリケーションを開発出来ます。

更に、muonでアプリケーションを作る時のステップを最小化して、muonアプリを作る障壁を低く抑えています。
最小構成なら、NPMパッケージを導入して構成ファイルに1行加えるだけで、最初のmuonアプリが動き出します。

### 特徴

- すべてのネットワークアクセスを、ホワイトリストフィルターで制限することで、問題を起こすコンテンツを完全に除外出来ます。
- 扱いやすいNPMパッケージとして提供され、あなたのウェブアプリケーションプロジェクトを簡単にネイティブGUIアプリケーション化出来ます。複雑な構成や変更は不要です。
- レンダリングを担うブラウザはCEF (Chromium Embedded Framework)です。つまり、ウェブアプリケーションから見た場合は、ChromiumやChromeを使用しているのとほぼ同等です。
- Viteプラグインに対応しています。更に、ViteのHMRに対応しているため、開発時にプレビューのリアルタイム更新を行えます。
- Linux (deb) とWindows (NSIS) のパッケージ生成、あるいはポータブル運用に対応しています。
- DevToolsを使用出来ます。更にCDP (Chrome DevTools Protocol)に対応しているため、外部からリモートデバッグを行うことが出来ます。
- プラグインシステムを備えています。また、プラグインの機能は、ホワイトリストフィルターで制限出来ます。
- 内蔵プラグインを使用して、ローカルファイルへのアクセス・オープンダイアログ・子プロセス起動・ウインドウ操作が可能です。

### 環境

- CEF公式バイナリの対応アーキテクチャのうち、下記のアーキテクチャに対応:
  - Linux: amd64, armhf, arm64
  - Windows: i686, amd64
- ビルド環境
  - Node.js 20以降
  - Vite 5以降

---

## muonを始める

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

### muonパッケージをインストール

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

### muon Viteプラグインを構成

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

![Get started](./images/get-started.png)

ページのソースコードを変更して、ブラウザで表示させていた時と遜色なく、HMRが機能することを確認してみて下さい。
例えば、 `src/App.tsx` 内の `<h1>Get started</h1>` の行を `<h1>Get started with muon!</h1>` に書き換えて保存すれば、
再起動すること無く瞬時にmuonウインドウ側の表示も書き換わるはずです。

そして、このページの中央に配置されてるカウンタボタン "Count is 0" をクリックすると、カウント値が増加することが確認できるはずです。
これで、ViteテンプレートのReactが正しく動作していることが確認出来ます。

また、 "Explore Vite" のようなボタンをクリックした場合、新たなウインドウが開いて、以下のような「がっかり」するページが表示されます:

![Forbidden](./images/forbidden.png)

そして、これこそが、muonの特徴である、ネットワークアクセスのホワイトリストフィルタが機能している証拠です。
このボタンは、Viteの公式サイト(`https://vite.dev/`)を表示しようとしますが、muonの既定では「ローカルのアセットのみ」アクセスが許可されているので、
それ以外のサイトコンテンツへのアクセスが遮断されているのです。
このホワイトリストの指定方法は別章で詳しく示します。

また、`F12` キーでmuon DevToolsを起動出来ます:

![muon DevTools](./images/devtools.png)

CDP (Chrome DevTools Protocol) も有効化されているので、Playwrightで操作したりvscodeでデバッグが可能です（詳しくば別章を参照）。

`Ctrl+F12` キーでmuonをリサイクル再起動出来ます。リサイクル再起動は、 `muon.json` の変更など、HMRで反映できない更新を行った場合に使用出来ます。

### パッケージ化

muonアプリのパッケージ化を行うのも簡単です:

```bash
npx muon pack
```

これで、LinuxとWindowsの各パッケージ(deb, nsis形式)が生成され、 `artifacts/` ディレクトリに生成されます。より詳しくは、以下のドキュメントを参照してください。

---

## ドキュメント

muonをより詳しく知りたい場合は、以下のドキュメントを参照してください。

### ユーザーガイド

- [アプリケーションの配布](./docs/ja/deployment.md)
- [CEFのダウンロードと更新](./docs/ja/cef-download-and-update.md)
- [muon DevTools](./docs/ja/muon-devtools.md)
- [ローカルアセットの構成](./docs/ja/local-assets.md)
- [muonプラグインを使用する](./docs/ja/muon-plugins.md)
- [外部ネットワークにアクセスする](./docs/ja/external-network.md)

### Advanced topics

- [muon CLI](./docs/ja/muon-cli-advanced-topics.md)
- [パッケージのインストール先について](./docs/ja/package-install-location-advanced-topics.md)
- [CEFバージョンとCEF APIバージョン](./docs/ja/cef-version-and-cef-api-version-advanced-topics.md)
- [CEFバイナリ更新の詳細](./docs/ja/cef-binary-update-details-advanced-topics.md)
- [ウインドウ間連携の制約](./docs/ja/window-communication-limitations-advanced-topics.md)
- [ローカルアセットの権限](./docs/ja/local-asset-permissions-advanced-topics.md)
- [ローカルアセットのパッキング](./docs/ja/local-asset-packing-advanced-topics.md)
- [オリジンベースのアクセス許可](./docs/ja/origin-based-permissions-advanced-topics.md)

### リファレンス

- [muon.jsonリファレンス](./docs/ja/muon-json-reference.md)
- [muon Viteプラグインリファレンス](./docs/ja/muon-vite-plugin-reference.md)
- [muon内蔵プラグインリファレンス](./docs/ja/muon-built-in-plugin-reference.md)

### muonプラグインの実装

TODO:

### その他

- [muon-uiセルフビルド](./docs/ja/muon-ui-self-build-advanced-topic.md)
- [制約](./docs/ja/constraints.md)

---

## ライセンス

Under MIT.

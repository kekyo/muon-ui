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

つまり、GTK/Qt/Windowsのネイティブアプリケーションをウェブアプリケーションに移行する際に発生する、大きな問題点の一つをわかりやすく除外することで、
ウェブベース技術のエコシステムを使って現代的なローカルGUIアプリケーションを開発出来ます。

### 特徴

- すべてのネットワークアクセスをホワイトリストフィルターで制限することで、問題を起こすコンテンツを完全に除外出来ます。
- 扱いやすいNPMパッケージとして提供され、あなたのウェブアプリケーションプロジェクトを簡単にネイティブGUIアプリケーション化出来ます。複雑な構成や変更は不要です。
- レンダリングを担うブラウザはCEF (Chromium Embedded Framework)です。つまり、ウェブアプリケーションから見た場合は、ChromiumやChromeを使用しているのとほぼ同等です。
- Viteプラグインに対応しています。ViteのHMRに対応しているため、開発時にプレビューのリアルタイム更新を行えます。
- `muon run` で、HTTPサーバーを起動せずにローカルアセットを直接使った開発起動が出来ます。
- Linux desktop launcherとアイコンのmetadataを配布ビルド時に同梱出来ます。
- DevToolsを使用出来ます。更にCDP (Chrome DevTools Protocol)に対応しているため、外部からリモートデバッグを行うことが出来ます。
- 複数のブラウザウインドウを表示出来ます。ブラウザウインドウは親子関係をもたせることも出来ます。
- プラグインシステムを備えています。また、プラグインの機能は、ホワイトリストフィルターで制限出来ます。
- 内蔵プラグインを使用して、ローカルファイルへのアクセス・オープンダイアログ・子プロセス起動・ウインドウ操作が可能です。
- Linux (deb) とWindows (NSIS) のパッケージ生成、あるいはポータブル運用に対応しています。

### 環境

- CEF公式バイナリの対応アーキテクチャのうち、下記のアーキテクチャに対応:
  - Linux: amd64, armhf, arm64
  - Windows: i686, amd64
- ビルド環境
  - Node.js 20以降
  - Vite 5以降（オプション）

---

## ドキュメント

### ユーザーガイド

- [muonを始める](./docs/ja/getting-started-muon.md)
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

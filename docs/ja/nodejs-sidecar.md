# Node.js sidecarを使用する

muonのNode.js機能は、muon本体とは別プロセスでNode.jsを実行するオプション機能です。`muon.json`に`node.project`を指定したアプリだけがNodeプラグインを読み込みます。

```json
{
  "node": {
    "project": "./backend"
  },
  "plugin": {
    "mode": "simple"
  }
}
```

相対`project`は`muon.json`のディレクトリから解決され、絶対パスも指定できます。参照先は通常のNode.jsプロジェクトであり、`package.json`と実行に必要なソース、依存パッケージを配置します。MVPのdescriptor facadeは`window.muon.node`へ公開されるため、実際に使用する場合は`plugin.mode: "simple"`が必要です。

```text
my-app/
├── muon.json
├── package.json
├── src/
└── backend/
    ├── package.json
    ├── backend.mjs
    └── node_modules/
```

Nodeプロジェクトの`package.json`は変換されません。`type`、`main`、`exports`、`imports`はNode.jsのmodule loaderが解釈し、`dependencies`は開発者が選択したpackage managerが管理します。muon Node bridgeは`engines.node`を検証します。muon固有の設定は`package.json`ではなく、`muon.json`の`node`に配置します。

muonは`npm install`などのパッケージマネージャーを実行しません。配布物に必要な依存関係は、`muon build`または`muon pack`を実行する前にNodeプロジェクトへ用意してください。ビルド時にはNodeプロジェクト全体が`assets.zip`の外側にある`node-project/`へコピーされ、シンボリックリンクは実体として収録されます。staging境界は指定したプロジェクトディレクトリであるため、上位のworkspaceへhoistされた依存関係は、プロジェクト内からlinkされていない限りコピーされません。実行時依存関係がすべて`node.project`内に表現される構成にしてください。循環するシンボリックリンクはサポートされず、ビルドエラーになります。Nodeプロジェクトとasset source、Viteの`outDir`または`publicDir`、出力先またはpack作業ディレクトリが包含関係にある構成は、Viteがファイルをコピーまたは削除する前に拒否され、ソース消失や二重収録を防ぎます。

複数targetを一度にビルドする場合も、同じNodeプロジェクトが各targetへコピーされます。native addonやplatform別optional dependencyを使用する場合は、開発者が各targetに適合する依存関係を事前に構成するか、targetごとに別のビルドを行う必要があります。MVPは依存関係をtarget向けに再構築しません。

## モジュールを呼び出す

`importModule()`はNodeのモジュールオブジェクトそのものではなく、export descriptorから生成したrendererローカルの凍結facadeを返します。

```ts
const fs = await window.muon.node?.importModule("node:fs/promises");
if (fs !== undefined) {
  const text = await fs.readFile("foobar.txt", "utf8");
  await fs.$release();
}
```

Node組み込みモジュールは`node:` prefixを必須とします。例えば`fs/promises`は拒否され、`node:fs/promises`は受け入れられます。プロジェクト相対specifierと、プロジェクトから解決できるbare package specifierも使用できます。`importModule(".")`はプロジェクトの`package.json`にある`main`（省略時はNode.jsのdirectory entry規則）をロードします。

プロセス境界を通過できる値は次のものに限定されます。

- `undefined`、`null`、boolean、負のゼロを除く有限のnumber、string
- signed 64-bitまたはunsigned 64-bit範囲の`bigint`
- コピーされる`ArrayBuffer`とすべての`ArrayBufferView`
- Node関数へ直接渡す一時的なcallback関数

`undefined`は明示的な値と`void`の戻り値の両方を表します。binary入力はviewの範囲だけがコピーされ、rendererへ返るbinary値は`Uint8Array`に正規化されます。任意のobject、class instance、Nodeのhandleやpointerは渡せません。Node側のexport値が対応外のobjectである場合、そのexportはfacadeへ追加されません。関数が対応外の値を引数または戻り値に使用すると、その呼び出しはrejectされます。callbackはNode関数の呼び出しがsettleするまで有効です。`$release`はfacade制御用、`then`はPromiseのthenable同化を防ぐための予約名であり、いずれかをexportするmoduleはimportできません。

`$release()`が解放するのは、bridgeが保持するdescriptor、proxy、remote callbackの参照だけです。Nodeモジュール内で作成されたserver、timer、watcherなどのresourceを停止または破棄しません。

encode後のIPC frameは1件あたり16 MiBに制限されます。JSON framingとbase64展開分も上限に含まれるため、利用できるbinary payloadはこれより小さく、他の引数にも依存します。一つのsidecarが受け付けるpending requestは最大1,024件であり、上限を超えた呼び出しは先行requestがsettleするまでrejectされます。

## Node.js実行ファイル

MVPのmuon-uiパッケージとアプリ成果物にはNode.js実行ファイルを含めません。実行時には次の順序でシステムのNode.jsを選択します。

1. `MUON_NODE_EXECUTABLE`が設定されている場合、その絶対パス
2. 未設定の場合、`PATH`上の`node`（Windowsでは`node.exe`）

`MUON_NODE_EXECUTABLE`の相対パスは拒否され、`PATH`へfallbackしません。shellは使用しません。sidecar自体はNode.js `^20.19.0 || >=22.12.0`を必要とし、プロジェクト初期化前にこのrangeを検証します。`package.json`に`engines.node`がある場合はプロジェクト側のrangeも検証され、実行中のNode.jsがいずれかを満たさなければ最初の`importModule()`がrejectされます。

Node sidecarは最初の`importModule()`で遅延起動します。同時に複数のimportが発生しても、アプリごとに一つのsidecarだけを起動します。プロジェクトコードをロードする前に、sidecarの作業ディレクトリは指定されたプロジェクトrootへ変更されます。起動後にNodeプロセスが終了した場合、その失敗はアプリ終了まで保持され、自動再起動は行いません。

## validateと権限の境界

validate modeは、設定、プラグインmetadata、公開関数とallow policyの整合性を確認します。`window.muon.node`は公開されず、Node sidecarも起動せず、プロジェクトのコードも実行しません。MVPのAPIを呼び出すにはsimple modeを使用してください。システムNode.jsの有無、`engines.node`、モジュール解決、Nodeコードの実行時エラーは、simple modeへ切り替えた後の最初のimport時に報告されます。この制約により、検証だけで開発者コードが実行されることを防ぎます。

muonのallow policyはrendererからNodeプラグインへ入る境界に適用されます。sidecar内でNodeコードが行うファイル、ネットワーク、子プロセスなどの操作には適用されません。Nodeプロジェクトはアプリ開発者が信頼し、その権限と依存関係を管理する前提です。

## 配布と停止

`muon build`と`muon pack`は、対象アプリが`node.project`を指定した場合だけ次を成果物へ収録します。

- 対象プラットフォームの`node.so`または`node.dll`
- Node bridgeの`node-bridge.mjs`
- staging済みの`node-project/`

muon frameworkはNode.js実行ファイル、Node配布archive、download cacheを追加しません（Nodeプロジェクト内へ開発者自身が置いたファイルは通常のプロジェクト内容としてコピーされます）。アプリ終了時、muonはsidecarへshutdownを要求し、終了を非同期に待ってからプラグインをunloadします。bridgeとtransportを閉じた後、Node.jsにactive handleが残っていてもsidecarは明示的に終了します。muonはプロジェクト固有のcleanup APIを自動的に発見、呼び出し、awaitしません。resourceをgracefulにcleanupする必要がある場合は、アプリケーションコードが該当APIを呼び出してawaitしてください。応答しないsidecarは猶予時間後にterminateされ、最終的に強制終了されます。Linuxでは子プロセス終了の非同期監視に`pidfd_open`を使用するため、Linux kernel 5.3以降が必要です。POSIXでのterminate対象はsidecar process自体であり、プロジェクトコードが生成したchild processの停止は開発者が管理します。

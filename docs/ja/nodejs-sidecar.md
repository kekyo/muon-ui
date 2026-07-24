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

Nodeプロジェクトの`package.json`は変換されません。`type`、`main`、`exports`、`imports`はNode.jsのmodule loaderが解釈し、`dependencies`は開発者が選択したpackage managerが管理します。`engines.node`も通常のNode.jsプロジェクトと同じ場所へ指定します。muonはこのrangeとNode bridgeの互換rangeが重なることを検証し、その積集合をランタイム要件として正規化します。`engines.node`を省略した場合はNode bridgeの互換rangeだけが要件になります。muon固有の設定は`package.json`ではなく、`muon.json`の`node`に配置します。

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

## 管理されるNode.jsランタイム

muonはシステムにインストールされたNode.jsを探索せず、アプリごとに管理するNode.jsランタイムを使用します。Node.jsプロジェクトを構成して実効プラグインモードが`simple`となる場合、`vite dev`と`muon run`では起動前のruntime preparationが、配布物では`muon-launcher`が初回起動時などにランタイムを準備します。選択済みNode.jsアーカイブのSHA-256を含むready fingerprintが一致する間は準備済みランタイムを再利用し、catalog更新によって選択versionが変わった場合は再準備します。`muon build`と`muon pack`はNode.jsのdownloadやinstallを行わず、選択に必要な正規化済みランタイム要件を成果物へ埋め込みます。

ランタイムのversionは、Nodeプロジェクトの`package.json`にある`engines.node`とNode bridgeの互換rangeの積集合から選択されます。`engines.node`を省略した場合は、その積集合に一致する最新のLTS releaseを選択し、一致するLTSがなければ失敗して非LTSへはfallbackしません。指定した場合も、一致するLTS releaseのうち最大のversionを優先し、一致するLTSがない場合は一致する全releaseのうち最大のversionを選択します。

release情報はNode.js公式配布元の`https://nodejs.org/dist/index.json`から取得します。選択したversionの`SHASUMS256.txt`にあるartifact固有のdigestを取得し、downloadしたarchiveをSHA-256で検証してから展開します。インストールされるファイルは次の二つだけであり、公式archiveに含まれるnpmやCorepackなどは配置しません。

- `runtimes/node/LICENSE`
- `runtimes/node/bin/node`（Windowsでは`runtimes/node/bin/node.exe`）

Nodeプラグインは実行中の`muon-core` executableのディレクトリを基準に、このNode.js実行ファイルの絶対パスを受け取ります。このパスだけを直接起動し、環境変数によるoverrideや`PATH`へのfallbackは行いません。管理ランタイムがない場合、別のNode.jsを暗黙に使用せず、sidecarの起動に失敗します。

Node sidecarは最初の`importModule()`で遅延起動します。同時に複数のimportが発生しても、アプリごとに一つのsidecarだけを起動します。プロジェクトコードをロードする前に、sidecarの作業ディレクトリは指定されたプロジェクトrootへ変更されます。起動後にNodeプロセスが終了した場合、その失敗はアプリ終了まで保持され、自動再起動は行いません。

## validateと権限の境界

validate modeは、設定、プラグインmetadata、公開関数とallow policyの整合性を確認します。`window.muon.node`は公開されず、Node.jsランタイムのdownloadとinstall、Node sidecarの起動、プロジェクトコードの実行は行いません。MVPのAPIを呼び出すにはsimple modeを使用してください。Nodeプロジェクトの`package.json`と`engines.node`の構文、およびNode bridgeの互換rangeとの積集合は、設定解決またはビルド時に検証されます。一方、管理ランタイムの準備、モジュール解決、Nodeコードの実行時エラーはvalidate modeでは検証されません。この制約により、検証だけでdownloadや開発者コードの実行が行われることを防ぎます。

muonのallow policyはrendererからNodeプラグインへ入る境界に適用されます。sidecar内でNodeコードが行うファイル、ネットワーク、子プロセスなどの操作には適用されません。Nodeプロジェクトはアプリ開発者が信頼し、その権限と依存関係を管理する前提です。

## 配布と停止

`muon build`と`muon pack`は、対象アプリが`node.project`を指定した場合だけ次を成果物へ収録します。

- 対象プラットフォームの`node.so`または`node.dll`
- Node bridgeの`node-bridge.mjs`
- staging済みの`node-project/`
- 内部`launcher.nodeRuntime`に格納された正規化済みランタイム要件

`launcher.nodeRuntime`はmuonが生成する内部設定です。ユーザーが`muon.json`へ直接指定するとbuild errorになります。build/pack時点の成果物にはNode.js実行ファイル、Node配布archive、download cacheを含めません（Nodeプロジェクト内へ開発者自身が置いたファイルは通常のプロジェクト内容としてコピーされます）。必要なNode.js実行ファイルは、成果物を起動したlauncherが公式配布元から準備します。アプリ終了時、muonはsidecarへshutdownを要求し、終了を非同期に待ってからプラグインをunloadします。bridgeとtransportを閉じた後、Node.jsにactive handleが残っていてもsidecarは明示的に終了します。muonはプロジェクト固有のcleanup APIを自動的に発見、呼び出し、awaitしません。resourceをgracefulにcleanupする必要がある場合は、アプリケーションコードが該当APIを呼び出してawaitしてください。応答しないsidecarは猶予時間後にterminateされ、最終的に強制終了されます。Linuxでは子プロセス終了の非同期監視に`pidfd_open`を使用するため、Linux kernel 5.3以降が必要です。POSIXでのterminate対象はsidecar process自体であり、プロジェクトコードが生成したchild processの停止は開発者が管理します。

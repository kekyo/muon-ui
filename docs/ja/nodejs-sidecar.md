# Node.js sidecarを使用する

muonのNode.js機能は、muon本体とは別プロセスでNode.jsを実行するオプション機能です。`muon.json`に`node.project`を指定したアプリだけがNodeプラグインを読み込みます。

```json
{
  "node": {
    "project": "./backend"
  }
}
```

相対`project`は`muon.json`のディレクトリから解決され、絶対パスも指定できます。参照先は通常のNode.jsプロジェクトであり、`package.json`と実行に必要なソース、依存パッケージを配置します。Node.js機能の有効化は`node.project`だけで決まり、`plugin.mode`や`plugin.plugins[]`のallow設定には依存しません。

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

既定の`validate`モードでは、Viteが提供する`muon:node` virtual moduleから`createNode()`をimportします。`muon:node`の使用に`plugin.plugins[].imports`やallow設定を追加する必要はありません。

```ts
import { createNode } from "muon:node";

const node = await createNode();
try {
  const fs = await node.importModule("node:fs/promises");
  try {
    const text = await fs.readFile("foobar.txt", "utf8");
  } finally {
    await fs.$release();
  }
} finally {
  await node.release();
}
```

`simple`モードでは、同じ機能を`window.muon.node`から使用します。

```ts
const node = await window.muon.node.createNode();
try {
  const backend = await node.importModule("./backend.mjs");
  await backend.run();
} finally {
  await node.release();
}
```

`createNode()`はNode.jsプロセスを一つ起動し、bridgeの初期化が完了して使用可能になるまで待ってから、凍結されたinstance facadeを返します。複数回呼び出すと、module cache、global state、active handle、process IDを共有しない独立したsidecarが作成されます。一つのinstanceを解放しても、別のinstanceはそのまま使用できます。muonはinstanceのpoolingや自動再起動を行いません。

instanceの`importModule()`はNodeのモジュールオブジェクトそのものではなく、export descriptorから生成したrendererローカルの凍結facadeを返します。muonはmodule固有の型を生成しないため、既定のexport型は`Record<string, any>`です。既存または開発者が用意したTypeScript型を、`importModule<TExports>()`の型引数などで適用できます。

Node組み込みモジュールは`node:` prefixを必須とします。例えば`fs/promises`は拒否され、`node:fs/promises`は受け入れられます。プロジェクト相対specifierと、プロジェクトから解決できるbare package specifierも使用できます。`importModule(".")`はプロジェクトの`package.json`にある`main`（省略時はNode.jsのdirectory entry規則）をロードします。

プロセス境界を通過できる値は次のものに限定されます。

- `undefined`、`null`、boolean、負のゼロを除く有限のnumber、string
- plain object、密なarray、`null`、boolean、負のゼロを除く有限のnumber、stringから構成されるJSON data
- signed 64-bitまたはunsigned 64-bit範囲の`bigint`
- コピーされる`ArrayBuffer`とすべての`ArrayBufferView`
- Node関数へ直接渡す一時的なcallback関数

JSON objectとarrayは、独立した値のsnapshotとしてserializeされます。objectのprototypeは`Object.prototype`または`null`で、列挙可能なown string-keyed data propertyだけを含む必要があります。arrayのprototypeは`Array.prototype`で、lengthまでの全indexを持ち、追加propertyを持たない必要があります。循環参照、accessor、symbol property、array組み込みの`length`以外の非列挙property、custom prototype、class instance、`Date`、`Map`、`Set`、およびJSON container内の`undefined`、`bigint`、binary、function、非有限numberは拒否されます。非循環の共有参照は出現箇所ごとにコピーされ、prototype、property descriptor、参照同一性は保持されません。

`undefined`は明示的な値と`void`の戻り値の両方を表します。binary入力はviewの範囲だけがコピーされ、rendererへ返るbinary値は`Uint8Array`に正規化されます。Nodeのhandleやpointerは渡せません。moduleがexportするJSON objectまたはarrayの定数はfacadeへ追加されず、JSON objectとarrayを使用できるのは関数またはcallbackの引数と戻り値だけです。対応外の値を使用した呼び出しはrejectされます。callbackはNode関数の呼び出しがsettleするまで有効です。`$release`はfacade制御用、`then`はPromiseのthenable同化を防ぐための予約名であり、いずれかをexportするmoduleはimportできません。

module facadeの`$release()`が解放するのは、bridgeが保持するdescriptor、proxy、remote callbackの参照だけです。Nodeモジュール内で作成されたserver、timer、watcherなどのresourceを停止または破棄せず、sidecar processも終了しません。

instanceの`release()`は冪等であり、同時に複数回呼び出した場合も同じ停止処理の完了を待ちます。停止を開始すると、そのinstanceから得たすべてのmodule facadeが無効になり、処理中のrequestは`Node instance was released before the request completed`でrejectされます。停止開始後の新しい操作は`Node instance is being released or has been released`でrejectされます。`release()`はtransportを閉じてsidecar processとOS handleを回収してからresolveします。親instanceの停止後にmoduleの`$release()`を呼んだ場合は、完了済みまたは処理中のinstance停止へ合流します。

実行環境が`Symbol.asyncDispose`を提供する場合、instanceは`AsyncReleaseable`としても解放できます。

```ts
await using node = await createNode();
const backend = await node.importModule(".");
await backend.run();
```

encode後のIPC frameは1件あたり16 MiBに制限されます。JSON framingとbase64展開分も上限に含まれるため、利用できるJSONまたはbinary payloadはこれより小さく、他の引数にも依存します。JSON snapshotの検証、serialize、copyは、renderer由来の値ではrenderer main thread上、Node由来の値ではsidecarのNode.js event loop上で同期的に行われます。大きなdataにはbinary値または別のstreaming手段を使用してください。一つのsidecarが受け付けるpending requestは最大1,024件であり、上限を超えた呼び出しは先行requestがsettleするまでrejectされます。

## 管理されるNode.jsランタイム

muonはシステムにインストールされたNode.jsを探索せず、アプリごとに管理するNode.jsランタイムを使用します。`node.project`を構成した場合、`simple`と`validate`のどちらのモードでも、`vite dev`と`muon run`では起動前のruntime preparationが、配布物では`muon-launcher`が初回起動時などにランタイムを準備します。選択済みNode.jsアーカイブのSHA-256を含むready fingerprintが一致する間は準備済みランタイムを再利用し、catalog更新によって選択versionが変わった場合は再準備します。`muon build`と`muon pack`はNode.jsのdownloadやinstallを行わず、選択に必要な正規化済みランタイム要件を成果物へ埋め込みます。

ランタイムのversionは、Nodeプロジェクトの`package.json`にある`engines.node`とNode bridgeの互換rangeの積集合から選択されます。`engines.node`を省略した場合は、その積集合に一致する最新のLTS releaseを選択し、一致するLTSがなければ失敗して非LTSへはfallbackしません。指定した場合も、一致するLTS releaseのうち最大のversionを優先し、一致するLTSがない場合は一致する全releaseのうち最大のversionを選択します。

release情報はNode.js公式配布元の`https://nodejs.org/dist/index.json`から取得します。選択したversionの`SHASUMS256.txt`にあるartifact固有のdigestを取得し、downloadしたarchiveをSHA-256で検証してから展開します。インストールされるファイルは次の二つだけであり、公式archiveに含まれるnpmやCorepackなどは配置しません。

- `runtimes/node/LICENSE`
- `runtimes/node/bin/node`（Windowsでは`runtimes/node/bin/node.exe`）

Nodeプラグインは実行中の`muon-core` executableのディレクトリを基準に、このNode.js実行ファイルの絶対パスを受け取ります。このパスだけを直接起動し、環境変数によるoverrideや`PATH`へのfallbackは行いません。管理ランタイムがない場合、別のNode.jsを暗黙に使用せず、sidecarの起動に失敗します。

Node sidecarは`createNode()`ごとに起動します。プロジェクトコードをロードする前に、sidecarの作業ディレクトリは指定されたプロジェクトrootへ変更されます。起動後にNodeプロセスが予期せず終了した場合、そのinstanceの処理だけが失敗し、他のinstanceは影響を受けません。失敗したinstanceは自動再起動されませんが、別の`createNode()`で新しいinstanceを起動できます。

## validateと権限の境界

validate modeでは`window.muon`を公開せず、Viteが生成する`muon:node` virtual moduleからNode instanceを作成します。`muon:node`は`node.project`を指定した場合に使用でき、通常のmuon plugin capabilityとは別に扱われます。そのため、Node相互運用APIに`plugin.plugins[].imports`やallowによる関数単位のfilterは適用されません。simple modeでは`window.muon.node.createNode()`を使用します。アプリ全体の`plugin.pages`によるページURL境界は、どちらのmodeでも引き続き適用されます。

どちらのモードでも、sidecar内でNodeコードが行うファイル、ネットワーク、子プロセスなどの操作や、その結果と副作用をmuonは制限または検証しません。Nodeプロジェクトはアプリ開発者が信頼し、その権限、処理内容、依存関係に責任を持つ前提です。

Nodeプロジェクトの`package.json`と`engines.node`の構文、およびNode bridgeの互換rangeとの積集合は、設定解決またはビルド時に検証されます。module解決やNodeコードのruntime errorは、実際に該当instanceで操作した時点で報告されます。

## 配布と停止

`muon build`と`muon pack`は、対象アプリが`node.project`を指定した場合だけ次を成果物へ収録します。

- 対象プラットフォームの`node.so`または`node.dll`
- Node bridgeの`node-bridge.mjs`
- staging済みの`node-project/`
- 内部`launcher.nodeRuntime`に格納された正規化済みランタイム要件

`launcher.nodeRuntime`はmuonが生成する内部設定です。ユーザーが`muon.json`へ直接指定するとbuild errorになります。build/pack時点の成果物にはNode.js実行ファイル、Node配布archive、download cacheを含めません（Nodeプロジェクト内へ開発者自身が置いたファイルは通常のプロジェクト内容としてコピーされます）。必要なNode.js実行ファイルは、成果物を起動したlauncherが公式配布元から準備します。

rendererのV8 contextが解放されると、そのcontextが作成したすべてのNode instanceも自動的に解放されます。アプリ終了時には、muonが残っているすべてのinstanceへshutdownを要求し、終了を非同期に待ってからプラグインをunloadします。bridgeとtransportを閉じた後、Node.jsにactive handleが残っていても各sidecarは明示的に終了します。muonはプロジェクト固有のcleanup APIを自動的に発見、呼び出し、awaitしません。resourceをgracefulにcleanupする必要がある場合は、アプリケーションコードが該当APIを呼び出してawaitしてください。応答しないsidecarは猶予時間後にterminateされ、最終的に強制終了されます。

Windowsでは各sidecarを個別のJob Objectへ割り当て、`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`を設定します。通常のinstance解放に加え、muon processが予期せず終了した場合も、すべてのJob Object handleが閉じられて対応するsidecarが終了します。Linuxでは子プロセス終了の非同期監視に`pidfd_open`を使用するため、Linux kernel 5.3以降が必要です。POSIXでのterminate対象はsidecar process自体であり、プロジェクトコードが生成したchild processの停止は開発者が管理します。

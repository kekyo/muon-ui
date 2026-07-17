# muonプラグインの開発

muonプラグインの世界にようこそ。

muonプラグインは、CEF内のJavaScriptから、ネイティブ世界のあらゆる機能にアクセスするための玄関です。
このプラグインを開発すれば、文字通りOSのすべての操作を行えるようになります。
例えば、Linuxのシステムコールやサードパーティ製ライブラリを呼び出したり、Windows APIを呼び出したりすることで、
CEFやmuon内蔵プラグインでは実現出来ない強力な機能を、ページから操ることが出来るようになります。

しかし。

ここで立ち止まって再度考えてください。この先は茨の道でもあります。

- muonプラグインはCまたはC++で実装する必要があります。
- クラッシュすると、muonアプリのプロセスごと死ぬ可能性があるような、殺伐とした世界です。
- muon内蔵プラグインで実現出来る事であれば、わざわざmuonプラグインを自力で実装する必要性はありません。
  特に、この開発に興味がある方は、 `muon.executor` 名前空間の機能を再度調べてみるのが良いでしょう。

## 概要

muonプラグインは、以下のようなフェーズで実行されます:

### プラグインの初期化とワイヤリング

`muon.json` に従って、muonプラグインが実装されたso/dllライブラリがロードされ、muonアプリから参照できるように初期化されます。

- プラグインは、 `muon_init_plugin()` というエントリポイントを提供して、muonアプリがこれを呼び出すことで初期化の契機が与えられ、返却された情報を元にmuonアプリ側にJavaScript関数群が公開されます。
- この時、muonプラグインは、CEFの「ブラウザプロセス」と呼ばれるプロセスにロードされます。CEFは各ブラウザウインドウ毎に異なるプロセス「レンダープロセス」も保持していて、「ブラウザプロセス」はこれらとは切り離された管理プロセスです。
  したがって、muonプラグインは、各レンダープロセスと直接操作を行うわけではありません。
- プラグインが返した、プラグイン関数のエントリポイントメタデータを元に、muonがJavaScriptの世界とのIPC通信を実現させる「ワイヤリング」を実施します。
  これは、JavaScriptからの関数呼び出しを、ブラウザプロセスまでのIPCとして準備して、シームレスに通信が行われるようにします。
- 最後に、（必要であれば）プラグインからカスタムJavaScriptソースコードを挿入します。
  上記のワイヤリング手順には、引数や戻り値の表現に制約があるため、JavaScriptから見て扱いやすい定義となるように、JavaScript変換コードを挿入出来ます。
  これはオプションであるため、標準のワイヤリング規則だけで機能を実現できるなら、JavaScriptソースコードの挿入は不要です。

### muonアプリからプラグイン関数を呼び出す

muonアプリのJavaScriptが公開された関数を呼び出すと、レンダープロセス-->ブラウザプロセスへ要求を転送するためのIPCを行います。
IPCは一種のRPC（リモート関数呼び出し）として機能するため、JavaScript側から見れば、単純にプラグインの関数を呼び出しているように見えます。

ブラウザプロセスに転送された要求は、プラグインの具体的な関数（CまたはC++で実装）を呼び出し、そこであなたのネイティブコードが動作する機会を得ます。
但し、プラグインの関数は常に `Promise` を返すことを前提としているように、ネイティブコード側の関数も非同期処理を前提とする必要があります。

C/C++で実装する場合、`Promise` に代わる標準的な継続動作手順が存在しないため、以下のような規則で関数シグネチャを定義する必要があります。
たとえば、JavaScriptから見ると以下のような関数だったとします（説明のためにTypeScriptで記述します）:

```typescript
// プラグインが公開する関数のシグネチャ
function plugin_add(a: number, b: number) : Promise<number> { ... };
```

これに対応するC(C++)のコードは以下の通りです:

```cpp
#include "muon_plugin_api.h"

// プラグイン側のC++コード
extern "C" void plugin_add(muon_completion_func comp, int32_t a, int32_t b) {
  const auto result = a + b
  // 戻り値を返すのではなく、`comp` を呼び出す
  comp(&result, nullptr);
}
```

`comp` は完了継続関数を示していて、これを呼び出すことで戻り値を返却します。
この形式は一見奇妙に思えるかもしれませんが、すぐに結果を返す場合と、結果の返却が遅延する場合のどちらも、同じ方法で結果を返却出来ることがメリットです。

関数の処理が長時間に及ぶ場合、要するに非同期処理として扱われる必要がある場合、`comp` を保存しておいて、処理が完了する任意のタイミングで `comp` を呼び出すことで、非同期的に処理の完了を通知できます。
単純に結果を通知するコールバック関数だと考えても良いでしょう。

### 非同期処理の補助

このように、すべてのプラグイン関数は非同期処理にネイティブに対応しています。
逆に言えば、すべてのプラグイン関数は長時間処理をブロックしては「いけません」。
何故かというと、プラグイン関数が呼び出された時のスレッドは、CEFの重要なスレッドコンテキストを使用しているため、ここで処理をブロックするとCEFの動作に影響が出ます。

CEFはChromiumの母体となるエンジンを抜き出したもので、これを使用することでエンドユーザーはChromium/Chromeと「同じようなスムーズなブラウザ体験」を期待します。
しかし、プラグイン関数内でスレッドをブロックすると、CEFのブラウザウインドウの動作に直接的な影響が発生する可能性があります。

I/O操作によって遅延が発生する場合は、非同期I/O操作を使用する必要があるでしょう。
CPU依存の計算が長時間続くなら、ワーカースレッドでオフロードする必要があり、計算処理が完了した時点で `comp` を呼び出すことになります。

これらをすべて C言語で実装することも一応可能ではありますが、muonでは内部で [`cardio`](https://github.com/kekyo/cardio/) を使用しているため、
あなたのプラグインプロジェクトでも `cardio` を使用することで、大幅に実装を簡略化出来ます。
`cardio` ではC++20以上で使用できる `co_await` や `co_return` が使用できるため、コールバックを排除した読みやすいコードが実装できるはずです。
もちろん、他の非同期処理対応のライブラリを使用することも出来るでしょう。

muonプラグインの実装例は、 [`muon-core/test_plugins/`](../../muon-core/test_plugins/) ディレクトリに存在するので参考にしてください。
これらは `muon-core` のテストのための実装ですが、テストケースに対応した小規模な実装なので、最小限の実装が把握しやすいと思われます。

### シグネチャメタデータ

プラグイン関数のワイヤリングは、 [`tra-ffic`](https://github.com/kekyo/tra-ffic/) を使用して行われています。
現在の `tra-ffic` は、限定的な型定義（主にプリミティブ型・文字列型・関数型、及び関数型シグネチャのネスト型）のみ扱うことが出来ます。
これらはJavaScriptのランタイムでも、C/C++のネイティブコード内でも「ランタイム情報」としては提供されません。
従って、ワイヤリングを成功させ、IPCを実現出来るようにするには、これらのメタデータ情報を開発者がプラグイン側から提供する必要があります。

前節の `plugin_add()` のメタデータ定義の例を示します:

```cpp
// i32型 (int32_t) の定義
static const muon_type_descriptor type_i32 = {
  MUON_TYPE_I32,
  nullptr,
};

// 引数群の定義
static const muon_type_descriptor plugin_add_args[] = {
  type_i32,   // 引数aの型
  type_i32,   // 引数bの型
};

// 公開関数群の定義
static const muon_plugin_function_metadata plugin_function = {
  "plugin_add",  // 関数名
  reinterpret_cast<muon_native_function>(&plugin_add),  // 関数エントリポイント
  {2, plugin_add_args, &type_i32},  // 関数シグネチャ定義 (引数数・引数定義・戻り値型)
  nullptr
};
```

このように、プラグイン関数がどのような引数と戻り値の型を持つのかを、C/C++コードで定義する必要があります。
これらの定義は、更に名前空間内のすべての関数のリストとして定義して、最終的に `muon_init_plugin()` から返却することになります。

最終的な `muon_init_plugin()` の実装を示します:

```cpp
// プラグイン名前空間内の関数のリスト
static const muon_plugin_function_metadata* const plugin_functions_pointers[] = {
  &plugin_function,
  nullptr
};

// プラグイン名前空間の定義
static const muon_plugin_namespace plugin_namespace = {
  "foobar.baz",               // 名前空間名
  nullptr,
  plugin_functions_pointers   // このプラグイン名前空間内の関数群
};

// 名前空間のリスト（一つのプラグインは複数の名前空間をサポートできる）
static const muon_plugin_namespace* const plugin_namespaces_pointers[] = {
  &plugin_namespaces,
  nullptr,
};

// プラグインメタデータの定義
static const muon_plugin_metadata plugin_metadata = {
  plugin_namespaces_pointers,
};

// 初期化関数
extern "C" const muon_plugin_metadata* muon_init_plugin(
  const muon_plugin_init_context* context) {

  // 例: muon.jsonからconfigキーの値を参照できる
  const auto* value = muon_plugin_get_config_value(context, "foobar.startAt");

  // メタデータを返却
  return &plugin_metadata;
}
```

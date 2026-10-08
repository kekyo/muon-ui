# Androidランタイムの回帰テスト

Android上のWebView、RPC、ネイティブプラグイン、ファイル操作、fullscreenと資源解放を検証するテストホストです。利用者向けランタイムは[core/android](../android)から使用します。

リポジトリルートから`npm test --workspace muon-android-tester`を実行すると、単体テスト、両ABIのAPK・APK setのビルド、依存ライブラリと配布物の検査を行います。生成したテストプラグインの出力先は`.build/plugins/<abi>/`です。

端末テストでは`ANDROID_SERIAL`で対象を指定します。x86_64・16 KiBページのエミュレーターでは`npm run test:android --workspace muon-android-tester`、Pixel 6では`npm run test:android:pixel6 --workspace muon-android-tester`を実行します。

`observer`は独立した利用アプリを端末から操作するためのテストアプリです。QuickJSの試作と検証は[core/android-poc](../android-poc)にあります。

## Androidプラグインの同梱

同梱するプラグインは[android-plugins.json](./android-plugins.json)で指定します。ビルド時にAPK/AABへ組み込み、実行時のダウンロードや探索は行いません。各エントリーの形式は次のとおりです。

```json
{
  "name": "sample_plugin",
  "soname": "libsample_plugin.so",
  "source": "../path/inside-this-repository/sample_plugin.cpp",
  "artifacts": {
    "x86_64": "lib/x86_64/libsample_plugin.so",
    "arm64-v8a": "lib/arm64-v8a/libsample_plugin.so"
  },
  "allow": ["sample.namespace.*"],
  "config": {
    "sample.key": "sample-value"
  }
}
```

- `source`には、このディレクトリからの相対パスを指定します。対象は同じリポジトリ内のC++20ソースに限ります。
- プラグインはMuon plugin APIの`muon_init_plugin`をエクスポートする必要があります。
- `soname`は`lib<name>.so`形式にします。
- `x86_64`と`arm64-v8a`の両方を必ず宣言します。生成物は対応する`lib/<abi>/`へ配置されます。
- `allow`は空にできません。読み込んだメタデータへ許可規則を適用し、許可された関数だけをWebViewへ公開します。
- `config`のキーと値は文字列です。
- デスクトップ用の`path`、`signature`、`salt`はAndroidの登録情報では使用できません。

登録情報の生成スクリプトは入力を検証し、同じ正規化済みエントリーからC++の読込みテーブルとCMakeのプラグインターゲットを生成します。配布物の検査では、名前やsonameの重複、未対応ABI、成果物の欠落、ELF machineの不一致、16 KiBの境界整列違反、`muon_init_plugin`の欠落をエラーにします。

## ランタイムとライフサイクルの制約

プロセス内にはcardio 1.1.0の`dispatcher_host_android_auto`と共通`MuonPluginRuntime`を一組だけ作り、Androidのmain Looperへ接続します。Activity/WebViewごとにセッションは独立しています。最後の通常セッションが閉じると、同じLooper上でプラグインを非同期に停止し、読み込んだ順序の逆順で解放します。停止中に生成されたActivityは、停止完了後に新しいランタイムへ接続します。

[Androidがプロセスを強制終了した場合、Activityの`onDestroy()`は保証されません](https://developer.android.com/guide/components/activities/process-lifecycle)。プラグインの`Stop()`も実行される保証がないため、永続データや外部トランザクションの確定は、各操作の完了時に行ってください。

[libffi 3.8.0のx86_64静的トランポリン](https://github.com/libffi/libffi/blob/v3.8.0/src/x86/internal64.h)は4 KiBテーブルに固定されています。16 KiBのVM向けには、展開したビルド用コピーへ[muon所有のパッチ](../android/patches/libffi/0001-android-x86_64-16k-static-trampoline.patch)を適用します。libffiのサブモジュール自体は変更しません。端末テストでは、クロージャーの実行領域が実行可能かつ書込み不可であることと、確保・解放数の一致を実測します。

# 独立したプラグインとアプリの例

このディレクトリをmuon-uiのリポジトリ外へコピーして使用できます。`application`で`npm install`を実行し、`npx muon-plugin doctor --target linux-amd64`で必要な環境を確認してください。公開前のSDKとmuon-uiを試す場合は、両パッケージのtgzを`npm install -D /path/to/muon-plugin-sdk.tgz /path/to/muon-ui.tgz`で指定します。

| ディレクトリ    | 示す使い方                                                                             |
| --------------- | -------------------------------------------------------------------------------------- |
| `local`         | アプリ専用のC++20・cardioプラグイン。標準Makeとpkg-configを使用します。                |
| `vendor-plugin` | 別ディレクトリのライブラリをCMakeで取り込むC99プラグイン。                             |
| `vendor-source` | 外部ライブラリの代わりに使う、小さな独立Cライブラリの教材。muonやSDKに依存しません。   |
| `application`   | 上記二つをローカルソースとして参照し、ボタンを押すと両プラグインを呼び出すViteアプリ。 |
| `npm-package`   | `local`の成果物をNPM配布するpackage.json。                                             |

`application`で`npm run dev`を実行すると、開発用アプリを起動します。Linux向けの生成は`npm run build`、Windows向けは`npx muon build --target windows-amd64`、Android向けは`npx muon build --target android`です。各プラットフォームのアプリ用環境も用意してください。

実際のvendorやsubmoduleを使用する場合は、`vendor-plugin/muon-plugin.build.json`の`VENDOR_SOURCE_DIR`と、CMakeでリンクするターゲットを外部プロジェクトに合わせます。外部ソースは変更せず、接続コードを`vendor-plugin`へ置きます。ビルド出力はSDKが指定するディレクトリへ生成します。

NPM配布を試すには、`application`から次を実行します。必要な対象についてbuildを繰り返してから包装してください。

```bash
npx muon-plugin build ../local/muon-plugin.build.json --target linux-amd64 --out ../npm-package
npx muon-plugin verify ../npm-package/muon-plugin.json --target linux-amd64
cd ../npm-package
npm pack
```

このtgzをアプリへ`npm install`し、counterの参照を`"native": { "package": "muon-example-counter" }`へ変更すれば、ビルド済みNPMパッケージを利用する例になります。`vendor_counter`も同じ方法で包装できます。プラグインすべてをビルド済みパッケージから利用するアプリでは、作者用SDKを開発依存から外せます。

依存.so/.dllと公開型の配布、アクセス権限、独自ビルドの設定は[ガイド](https://github.com/kekyo/muon-ui/blob/main/docs/ja/muon-plugin-sdk.md)を参照してください。

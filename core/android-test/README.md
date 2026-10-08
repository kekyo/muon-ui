# Androidランタイムの回帰テスト

Android上のWebView、RPC、ネイティブプラグイン、ファイル操作、fullscreenと資源解放を検証するテストホストです。利用者向けランタイムは[core/android](../android)から使用します。

リポジトリルートから`npm test --workspace muon-android-tester`を実行すると、単体テスト、両ABIのAPK・APK setのビルド、依存ライブラリと配布物の検査を行います。生成したテストプラグインは`.build/plugins/<abi>/`に配置されます。

端末テストでは`ANDROID_SERIAL`で対象を指定します。x86_64・16 KiBページのエミュレーターでは`npm run test:android --workspace muon-android-tester`、Pixel 6では`npm run test:android:pixel6 --workspace muon-android-tester`を実行します。

`observer`は独立した利用アプリを端末から操作するためのテストアプリです。QuickJSの試作と検証は[core/android-poc](../android-poc)にあります。

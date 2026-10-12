# muon-plugin-sdk

muon-uiのCEF/Linux・WindowsとAndroid向けに、C/C++のネイティブプラグインを開発するSDKです。muon-ui本体のソースやビルド環境を取得せず、公開ヘッダーと対象別のリンク用ライブラリを使用できます。

```bash
npm install -D muon-plugin-sdk
npx muon-plugin init native/counter --name counter
npx muon-plugin doctor --config native/counter/muon-plugin.build.json --target linux-amd64
npx muon-plugin build native/counter/muon-plugin.build.json --target linux-amd64
```

既定はC++20とcardio、Makeとpkg-configです。C99は`--language c`、独自CMakeの雛形は`--build-system cmake`で選びます。任意の既存ビルドも、SDKの環境と成果物形式を使用できます。

`doctor`は必要なツール・NDK・対象用ライブラリ・SDKファイルを調べ、不足項目と導入方法をまとめて表示します。ビルド済みプラグインを利用するアプリには、作者用SDKやネイティブコンパイラは不要です。

[開発・導入・NPM配布のガイド](https://github.com/kekyo/muon-ui/blob/main/docs/ja/muon-plugin-sdk.md)と[独立サンプル](examples/README_ja.md)を参照してください。SDKとmuon-uiは同じリリースを使用します。対象はLinux amd64・armhf・arm64、Windows i686・amd64、Android arm64-v8a・x86_64です。

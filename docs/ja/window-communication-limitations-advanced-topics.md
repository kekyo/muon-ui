# ウインドウ間連携の制約 (Advanced topics)

`browser.allowUnsafeJavaScriptParentAccess` は、ページから別のページを（別のウインドウで）開いた場合などに、
子孫のページから親のページのオブジェクトにアクセス出来るかどうかをフィルタします。
既定では空のリストであり、すべてのページで親ページのオブジェクトアクセスが許可されません。

このフィルタは、開かれる子孫ページのURLに対して適用されます。
例えば、子ページは `asset://sub/` 以下に配置して `main` と分離し、以下のように子ページを許可します:

```json
{
  "browser": {
    "allowUnsafeJavaScriptParentAccess": [
      "asset://sub/**"  // 子ページでの親ページの参照を許可
    ]
  }
}
```

親ページ (`asset://main/index.html`):

```javascript
// 子ページから参照する情報をwindowに保持する例
window.foobarText = "ABC";

// 子ページ (`asset://sub/index.html`) を開く
window.open("asset://sub/index.html", "sub-window");
```

子ページ (`asset://sub/index.html`):

```javascript
// 子ページから親ページの情報にアクセスできる
const parentFoobarText = window.opener.foobarText;
```

この方法を使用することで、異なるオリジンでも親ページから子ページに情報を簡単に引き継ぐことが出来ます。
この設定は `window.opener` による親ページ参照を許可するかどうかだけを制御し、親ウインドウの操作可否や親子ウインドウのライフサイクルは変更しません。
ページ側で `noopener` または `noreferrer` を指定した場合は、この許可リストに一致していても `window.opener` は切断されます。

ところでこの機能がホワイトリスト形式なのは、その危険性のためです。
親ページのオブジェクトを介してmuonプラグインの関数に簡単にアクセス出来る危険性があります:

```javascript
// 親ページにmuonプラグインが露出していれば、
// 子ページから親ページ経由で呼び出せてしまう (!!)
await window.opener.muon.fs.writeTextFile(
  ".BABEL", "BABELBABELBABEL", "utf8");
```

従って、使用には十分注意が必要です。基本的にこの機能を使用することはお勧めしません:

- 一般的なウェブアプリケーション開発では、 `window.opener` による参照に頼るのではなく、 [ローカルストレージ](https://developer.mozilla.org/en/docs/Web/API/Window/localStorage) を用いたり、フレームワークの [ページルーティング機能](https://reactrouter.com/) を用いて情報の受け渡しを行うことを推奨します。
- また、いわゆる「モーダルダイアログ的なウインドウ管理」もふさわしくなく、設計時の自由度も劣ります。
  データハンドリングの複雑性もさることながら、URL履歴の管理や異常なページ遷移の回避に労力を要するなど、一見シンプルな解決方法に見えて、実際には別の様々な問題を引き起こします。
- モーダル画面遷移が必要な場合は、例えばReact MUIの [モーダルコンポーネント](https://mui.com/material-ui/react-modal/) を使用し、アプリケーション全体では [SPA](https://dev.to/seyedahmaddv/how-to-build-a-single-page-application-spa-with-react-285) で実装する手法があります。

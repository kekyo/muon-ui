# Window communication limitations (Advanced topics)

`browser.allowUnsafeJavaScriptParentAccess` filters whether descendant pages can access objects on the parent page when a page opens another page in another window.
By default, this is an empty list, and object access to parent pages is not allowed for any page.

This filter is applied to the URL of the descendant page being opened.
For example, place the child page under `asset://sub/` to separate it from `main`, and allow the child page as follows:

```json
{
  "browser": {
    "allowUnsafeJavaScriptParentAccess": [
      "asset://sub/**"  // Allow child pages to reference their parent page.
    ]
  }
}
```

Parent page (`asset://main/index.html`):

```javascript
// Example of keeping information referenced by the child page on window.
window.foobarText = "ABC";

// Open the child page (`asset://sub/index.html`).
window.open("asset://sub/index.html", "sub-window");
```

Child page (`asset://sub/index.html`):

```javascript
// The child page can access information on the parent page.
const parentFoobarText = window.opener.foobarText;
```

This method makes it easy to pass information from a parent page to a child page, even across different origins.
This setting only controls whether parent-page references through `window.opener` are allowed.
It does not change whether the parent window can be operated or the lifecycle of parent/child windows.
If the page specifies `noopener` or `noreferrer`, `window.opener` is disconnected even when the allow list matches.

This feature is whitelist-based because it is dangerous.
It can easily allow access to muon plugin functions through parent-page objects:

```javascript
// If muon plugins are exposed on the parent page,
// a child page can call them through the parent page (!!)
await window.opener.muon.fs.writeTextFile(
  ".BABEL", "BABELBABELBABEL", "utf8");
```

Therefore, use this feature with great care.
In general, using this feature is not recommended:

- In general web application development, instead of relying on references through `window.opener`, use [local storage](https://developer.mozilla.org/en/docs/Web/API/Window/localStorage) or framework [page routing features](https://reactrouter.com/) to pass information.
- So-called "modal-dialog-style window management" is also not suitable and reduces design flexibility.
  In addition to data-handling complexity, it requires effort to manage URL history and avoid abnormal page transitions. It may look simple at first, but in practice it causes various other problems.
- If modal screen transitions are necessary, one approach is to use a component such as React MUI's [Modal component](https://mui.com/material-ui/react-modal/) and implement the whole application as an [SPA](https://dev.to/seyedahmaddv/how-to-build-a-single-page-application-spa-with-react-285).

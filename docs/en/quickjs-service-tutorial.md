# Build a QuickJS note service

Save a note from a page to a file owned by a QuickJS service. After you reload the page, selecting Load retrieves the text you saved.

Use a Linux or Windows application configured with Vite 8 or later and the Muon Vite plugin. For a new application, follow [Getting started](getting-started.md) through opening the development window. All paths and commands below are relative to the application root.

## Register the service

```sh
npm install -D muon-plugin-sdk
```

The following `muon.json` is the complete configuration for the tutorial. It registers a service named `note`. The `io.fs.data` grant lets it read and write its private files, and `plugin` lets the page acquire a service. In an existing simple-mode application, add the service and its page permissions to your current configuration.

```json
{
  "services": {
    "note": {
      "runtime": "quickjs",
      "entry": "services/note/service.ts",
      "io": { "fs": { "data": "read-write" } }
    }
  },
  "plugin": {
    "mode": "simple",
    "plugins": [{ "name": "internal", "allow": ["muon.services.*"] }]
  }
}
```

## Implement the read and write methods

Create `services/note/interface.ts`. The page and the service share this declaration. `load` returns a string, and `save` takes one string. The declared return type of `save` is `void`. The page can await its `Promise<void>` to know when the write finishes.

```ts
import { defineServiceInterface } from 'muon-plugin-sdk/services';

export const noteInterface = defineServiceInterface({
  schemaVersion: 1,
  methods: {
    load: { parameters: [], returns: 'string', errors: [] },
    save: { parameters: ['string'], returns: 'void', errors: [] },
  },
  events: {},
});
```

Create `service.ts` in the same directory. When `note.txt` is missing, as it is on the first run, `load` returns an empty string. It propagates any other read error to the caller. `save` waits for the file write to finish.

```ts
import { defineService, isServiceIoError } from 'muon-plugin-sdk/services';
import { noteInterface } from './interface.ts';

export default defineService({
  interface: noteInterface,
  start: (context) => ({
    methods: {
      load: async (call): Promise<string> => {
        try {
          return await context.fs.readText('note.txt', { signal: call.signal });
        } catch (error) {
          if (isServiceIoError(error) && error.code === 'not-found') return '';
          throw error;
        }
      },
      save: async (call, text): Promise<void> => {
        await context.fs.writeText('note.txt', text, { signal: call.signal });
      },
    },
    stop: () => {},
  }),
});
```

## Save and load from the page

Use the following as `index.html` in the tutorial application.

```html
<!doctype html>
<html lang="en">
  <meta charset="utf-8">
  <title>Service notes</title>
  <fieldset>
    <legend>Notes</legend>
    <label>Note <textarea></textarea></label>
    <button type="button" value="save">Save</button>
    <button type="button" value="load">Load</button>
    <output></output>
  </fieldset>
  <script type="module" src="/src/note-page.ts"></script>
</html>
```

Create `src/note-page.ts`. Each button click acquires a service handle and releases it in `finally`. Passing the shared declaration to `bindServiceClient` gives `await note.call('load', [])` the TypeScript type `string`.

```ts
import type {} from 'muon-ui';
import { bindServiceClient } from 'muon-plugin-sdk/services';
import { noteInterface } from '../services/note/interface.ts';

const services = window.muon.services;
if (!services) throw new Error('Service access is not enabled');
const panel = document.querySelector('fieldset')!;
const text = document.querySelector('textarea')!;
const status = document.querySelector('output')!;

for (const button of panel.querySelectorAll('button')) {
  button.onclick = async () => {
    panel.disabled = true;
    try {
      const handle = await services.acquire('note');
      try {
        const note = bindServiceClient(noteInterface, handle);
        if (button.value === 'save') {
          await note.call('save', [text.value]);
          status.value = 'Saved';
        } else {
          text.value = await note.call('load', []);
          status.value = 'Loaded';
        }
      } finally {
        await handle.release();
      }
    } catch (error) {
      status.value = String(error);
    } finally {
      panel.disabled = false;
    }
  };
}
```

## Read back the saved note

```sh
npx vite dev
```

1. In the Muon window, select Load. On the first run the text field is empty and the page displays `Loaded`.
2. Enter `Shopping list`, select Save, and wait for `Saved`.
3. Leave the development command running and reload the page. Select Load to see `Shopping list` again.

Ending the development command removes that session's data. To try persistence across application restarts, launch a [distribution build](getting-started.md#distribution-builds), save a note, quit the application, launch it again, and select Load.

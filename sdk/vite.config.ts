import prettierMax from 'prettier-max';
import screwUp from 'screw-up';
import { defineConfig } from 'vitest/config';

export default defineConfig(({ mode }) => ({
  plugins: [
    { ...prettierMax(), apply: 'build' },
    { ...screwUp({ outputMetadataFile: true }), apply: 'build' },
  ],
  define:
    mode === 'cjs'
      ? {
          'import.meta.url':
            'require("node:url").pathToFileURL(__filename).href',
        }
      : {},
  build: {
    emptyOutDir: mode === 'cjs',
    lib: {
      entry: { index: 'src/index.ts' },
      formats: [mode === 'cjs' ? 'cjs' : 'es'],
      fileName: (format, name) => `${name}.${format === 'es' ? 'mjs' : 'cjs'}`,
    },
    target: 'node20',
    minify: false,
    rolldownOptions: { external: [/^node:/, 'funcity'] },
  },
  test: { environment: 'node', testTimeout: 120000, hookTimeout: 120000 },
}));

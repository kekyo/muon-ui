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
      entry:
        mode === 'cjs'
          ? { index: 'src/index.ts', artifact: 'src/artifact.ts' }
          : {
              index: 'src/index.ts',
              artifact: 'src/artifact.ts',
              cli: 'src/cli.ts',
            },
      formats: [mode === 'cjs' ? 'cjs' : 'es'],
      fileName: (format, name) => `${name}.${format === 'es' ? 'mjs' : 'cjs'}`,
    },
    target: 'node20',
    minify: false,
    rolldownOptions: {
      external: [/^node:/, 'funcity', 'commander'],
      output: {
        chunkFileNames:
          mode === 'cjs' ? '[name]-[hash].cjs' : '[name]-[hash].mjs',
      },
    },
  },
  test: { environment: 'node', testTimeout: 120000, hookTimeout: 120000 },
}));

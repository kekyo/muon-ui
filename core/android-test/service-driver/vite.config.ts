import { defineConfig } from 'vite';
import prettierMax from 'prettier-max';
import screwUp from 'screw-up';

export default defineConfig({
  root: new URL('./web', import.meta.url).pathname,
  base: './',
  plugins: [prettierMax(), screwUp()],
  build: { outDir: '../build/web', emptyOutDir: true, target: 'es2022' },
});

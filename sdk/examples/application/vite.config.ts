import { defineConfig } from 'vite';
import prettierMax from 'prettier-max';
import muon from 'muon-ui/vite';
export default defineConfig({
  plugins: [prettierMax(), muon()],
  build: { target: 'es2022' },
});

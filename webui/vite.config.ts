import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';
import { viteSingleFile } from 'vite-plugin-singlefile';
import { resolve } from 'node:path';

// The plugin's WebUiServer is WebSocket-only; it does NOT serve HTTP assets.
// The OBS dock opens resources/ui/index.html as a local file, so the whole app
// (JS + CSS) must inline into that single file — hence vite-plugin-singlefile.
export default defineConfig({
  plugins: [react(), viteSingleFile()],
  resolve: {
    alias: { '@': resolve(__dirname, 'src') },
  },
  build: {
    // Emit straight into the plugin's resource dir, replacing the old
    // hand-written mirror. Only index.html lives here, so emptying is safe.
    outDir: resolve(__dirname, '../resources/ui'),
    emptyOutDir: true,
    target: 'es2020',
    cssCodeSplit: false,
    assetsInlineLimit: 100_000_000,
  },
});

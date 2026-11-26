import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

// The built app goes to ../app (served by tools/preview.py at /app, or upload
// that folder to any https host - Bluetooth on a phone needs https).
export default defineConfig({
  plugins: [react()],
  base: "./",
  build: { outDir: "../app", emptyOutDir: true, assetsInlineLimit: 0 },
});

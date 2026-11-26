import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

// Locally the built app goes to ../app (served by tools/preview.py at /app).
// On Vercel (the hosted, https version for phones) it goes to dist/.
export default defineConfig({
  plugins: [react()],
  base: "./",
  build: { outDir: process.env.VERCEL ? "dist" : "../app", emptyOutDir: true, assetsInlineLimit: 0 },
});

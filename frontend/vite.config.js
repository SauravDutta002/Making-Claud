import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

// The built page goes straight into the bridge, which serves it at http://localhost:47801.
// `npm run dev` proxies the API to a running bridge, so you can work on the page live.
// On Vercel (the hosted website) the build goes to dist/ instead.
export default defineConfig({
  plugins: [react()],
  base: "./",
  build: {
    outDir: process.env.VERCEL ? "dist" : "../bridge/web",
    emptyOutDir: true,
    assetsInlineLimit: 0,
  },
  server: {
    port: 5173,
    proxy: { "/api": { target: "http://localhost:47801", changeOrigin: true } },
  },
});

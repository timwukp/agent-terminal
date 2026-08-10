import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

export default defineConfig({
  plugins: [react()],
  // Tauri expects a fixed dev port and a plain relative-path build.
  base: "./",
  server: { port: 1420, strictPort: true },
  build: { outDir: "dist" },
});

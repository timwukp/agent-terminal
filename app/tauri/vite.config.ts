import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

export default defineConfig({
  plugins: [react()],
  // Tauri expects a fixed dev port and a plain relative-path build.
  base: "./",
  server: { port: 1420, strictPort: true },
  build: { outDir: "dist" },
  // jsdom only where a test asks for it (`@vitest-environment jsdom`), so
  // the pure codec/hit-test suites keep running in plain node. Focus
  // behaviour is the reason a DOM environment exists here at all: "typed
  // and nothing happened" is a focus bug, and it is invisible to
  // logic-only tests.
  test: {
    environment: "node",
    // Node 25 shadows jsdom's localStorage with a broken one (testSetup.ts).
    setupFiles: ["./src/testSetup.ts"],
    // Vitest stubs CSS imports to "" by default, which silently turned the
    // theme.css-vs-theme.ts cross-check into a test that asserted nothing
    // (an empty stylesheet parses to zero tokens). Processing CSS costs a
    // few ms and makes `?raw` return the real file.
    css: true,
  },
});

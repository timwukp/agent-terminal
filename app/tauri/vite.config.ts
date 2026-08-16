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
    // Vitest's 5 s default is not a budget here, it is a coin flip. Measured
    // with --reporter=verbose on a 14-core machine: notifyWiring.test.tsx's
    // first case takes 469 ms run alone and 4699 ms in the full 28-file run
    // (focus.test.tsx's first case 3049 ms) — a 10x stretch from CPU
    // contention, since vitest runs ~13 jsdom environments at once and the
    // suite reports ~150 s of environment time inside a ~23 s wall clock.
    // Every later case in the same file lands at 344-890 ms. So the whole
    // margin was ~300 ms and a loaded machine ate it, failing the FIRST case
    // of whichever DOM file lost the race. None of these are timing
    // assertions, and CI runners have fewer cores than this machine, so the
    // ceiling is raised rather than the contention tuned down: a real hang
    // still fails, 3 s of scheduler noise no longer does.
    // Why 15 s and not 8: re-run with eight spinning processes against it —
    // roughly half the CPU, i.e. a weaker runner — the slowest single case
    // reached 6093 ms and 6454 ms, so >6 s is reachable in practice and the
    // 5 s default fails there by construction. 254/254 in both loaded runs and
    // in 8 consecutive unloaded ones.
    testTimeout: 15_000,
    // Vitest stubs CSS imports to "" by default, which silently turned the
    // theme.css-vs-theme.ts cross-check into a test that asserted nothing
    // (an empty stylesheet parses to zero tokens). Processing CSS costs a
    // few ms and makes `?raw` return the real file.
    css: true,
  },
});

// Test setup — repairs Web Storage under Node 25, and gives
// testing-library's async queries a budget that matches this suite's
// measured contention.
//
// Node 25 defines its own `globalThis.localStorage`, gated on
// `--localstorage-file`; without a path it is a bare `{}` that prints a
// warning and carries no getItem/setItem/clear. Vitest's jsdom environment
// only fills globals that are ABSENT, so Node's property shadows jsdom's
// real implementation and every storage-backed test fails on a missing
// method instead of on its subject — a failure that reads like a bug in the
// code under test. `sessionStorage` is unaffected, which is what makes the
// shadowing easy to misread.
//
// The shim is installed only when the running Node actually breaks it, so a
// version that behaves keeps using jsdom's own Storage.

export {}; // a module, so the dynamic import below may be awaited at top level

function memoryStorage(): Storage {
  const map = new Map<string, string>();
  return {
    get length() {
      return map.size;
    },
    key: (i: number) => [...map.keys()][i] ?? null,
    getItem: (k: string) => (map.has(k) ? (map.get(k) as string) : null),
    setItem: (k: string, v: string) => void map.set(String(k), String(v)),
    removeItem: (k: string) => void map.delete(String(k)),
    clear: () => map.clear(),
  } as Storage;
}

// Only in a DOM environment: the pure codec suites run in plain node, where
// a browser storage object has no business existing at all.
if (typeof document !== "undefined") {
  const current = (globalThis as { localStorage?: { getItem?: unknown } }).localStorage;
  if (typeof current?.getItem !== "function") {
    Object.defineProperty(globalThis, "localStorage", {
      value: memoryStorage(),
      configurable: true,
      writable: true,
    });
  }

  // vite.config.ts raised `testTimeout` to 15 s for this suite's measured
  // 10x contention stretch, but that is the budget for a whole CASE.
  // `findBy*`/`waitFor` carry their OWN timeout, defaulting to 1000 ms — so
  // that fix was incomplete for exactly the query shape that waits on a
  // React effect, and the two numbers were 15x apart.
  //
  // Measured on a loaded 14-core machine (the run's own ~13 concurrent jsdom
  // environments plus unrelated desktop apps; eight spinning processes were
  // added, so the load is not attributable to any single source):
  // `focus.test.tsx > moves focus off the sidebar button and into the
  // terminal` FAILED at 2587 ms inside its 15 s case, on a `findByRole`
  // holding 1000 ms, while ordinary PASSING cases in the same run reached
  // 8184 and 11905 ms. Whatever produced the contention, a query budget
  // 8x below the observed case durations cannot survive it — the query
  // budget was the binding one, not the case budget.
  //
  // 10 s: above the 8184 ms peak of the rendering DOM cases, and below
  // `testTimeout` so a genuinely missing element still fails with
  // testing-library's "Unable to find element" and its DOM dump — the
  // message that says what went wrong — rather than a bare case timeout
  // that says only that time passed. Asserted in focus.test.tsx, since
  // nothing else in this file would notice the setting being dropped.
  //
  // Imported from @testing-library/react (a declared dependency) rather
  // than @testing-library/dom, which is present only transitively.
  const { configure } = await import("@testing-library/react");
  configure({ asyncUtilTimeout: 10_000 });
}

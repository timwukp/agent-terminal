// Test setup — repairs Web Storage under Node 25.
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
}

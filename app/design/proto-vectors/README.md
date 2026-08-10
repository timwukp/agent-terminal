# Protocol wire vectors

The canonical golden file is
`app/tauri/src-tauri/crates/at-proto/tests/data/client-frames.json` —
generated and drift-checked by `at-proto`'s `tests/vectors.rs`
(`REGEN_VECTORS=1 cargo test -p at-proto --test vectors` regenerates;
commit the result, same contract as the C repo's `REGEN_GOLDEN`).

It lives inside the crate rather than here so tools that copy only the
Cargo workspace (cargo-mutants) can still run the drift check. Any other
codec (Route B's Swift implementation) validates against that same file:
every `frame_hex` must byte-for-byte match what its encoder produces and
parse cleanly through its decoder.

# Contributing

Thanks for your interest in agent-terminal.

## Ground rules

- **C17, no new runtime dependencies.** The zero-deps property is a
  feature; PRs adding libraries need a very strong case.
- **The VT engine stays isolated.** Nothing in `src/vt/` may perform I/O,
  call into the daemon, or allocate proportionally to untrusted input.
  All effects go through `vt_callbacks`.
- **Tests accompany changes.** VT changes need table-driven cases in
  `tests/unit/test_vt.c` (they are automatically chunking-property-tested);
  protocol/daemon changes need integration coverage under
  `tests/integration/`.
- **Run before pushing:**
  ```sh
  make test BUILD=asan
  make fuzz-regress BUILD=asan
  make BUILD=release all
  for t in tests/integration/test_*.sh; do BUILD=release bash "$t" || break; done
  python3 tools/check_svg.py docs/architecture.svg   # if you touched the diagram
  ```
  `BUILD` must match between building and testing: the integration scripts
  resolve `build/$BUILD` and abort with a `missing` message otherwise.

If you use a coding agent on this repo, [AGENTS.md](AGENTS.md) is the
machine-oriented version of these rules — keep the two in sync when either
changes.

## Reporting bugs

For VT rendering bugs, the most useful artifact is a raw byte capture:

```sh
script -r /tmp/capture   # record the misbehaving session, then exit
```

Attach `/tmp/capture` (or the relevant slice) to the issue. It becomes a
regression test / fuzz corpus entry directly.

For security issues, see [SECURITY.md](SECURITY.md) — do not open public
issues.

## License

By contributing, you agree that your contributions are licensed under the
MIT License.

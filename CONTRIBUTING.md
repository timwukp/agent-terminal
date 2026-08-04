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
  for t in tests/integration/test_*.sh; do bash "$t"; done
  ```

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

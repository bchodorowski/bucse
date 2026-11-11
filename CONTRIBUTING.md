# Contributing to bucse

Before changing the code, read [docs/design.md](docs/design.md). It
explains the core concepts and the invariants that every change must keep.

## Build, test, run

- Install the requirements and build: see [README.md](README.md#compilation).
- Build: `make`
- Tests: `tests/allTests.sh` (Python, `tests/test*.py`)
- Sample repositories for quickly trying out code changes:
  `testRepoAes/`, `testRepoUnencrypted/`
- Debugging: mount in the foreground with verbose logging, then work with
  the files in `mountpoint`:

  ```
  ./bucse-mount -f -v 4 -r testRepoAes mountpoint
  ```

## Coding conventions

- Language: C99.
- Naming: camelCase functions (e.g. `addToDynArray`).
- Error handling: return non-zero on error; failures must propagate to
  FUSE.
- Logging: use `log.h`; never log secrets or plaintext.
- Dependencies: libfuse3, json-c, libssh, libarchive — avoid adding more.
- Keep the Makefile simple.

## Rules for changes

- Discuss any change to the repository format or anything crypto-related
  before implementing it.
- Add or update a test in `tests/` for every behaviour change.
- Keep commits small, one topic each.

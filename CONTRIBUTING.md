# Contributing to bucse

Before changing the code, read [docs/design.md](docs/design.md). It
explains the core concepts and the invariants that every change must keep.

## Build, test, run

- Install the requirements and build: see [README.md](README.md#compilation).
- Build: `make`
- Tests: `tests/allTests.sh` runs every `tests/test*.py` (Python) for both
  encryption backends (`none`, `aes`) and the local destination, with
  bucse-mount under valgrind. Options: `--no-valgrind` (faster), or test file
  names to run only those. Set `BUCSE_TEST_SSH=ssh://host/~/bucseTests` to
  also test the ssh destination (needs key-based ssh login to the host).
  Outputs go to `tests/logs/`. Test requirements: python3, valgrind, openssl
  (the command, to encrypt test action files for `aes`).
- Keep the regular tests quick and light: the whole `allTests.sh` run should
  take minutes on a low-end machine, also under valgrind. Wait for bucse-mount
  with `waitForFiles()` / `waitForActionsHandled()` from `bucseTests.py`, not
  with fixed sleeps.
- Stress tests: `tests/stressTests.sh` runs `tests/stressTest*.py` (bigger
  versions of some regular tests, e.g. with multi-gigabyte files, or with a
  file downloaded from the internet). They need a lot of time, RAM and about
  16 GB of disk space, so run them from time to time, not for every commit.
- Action files in `tests/data/` are plain JSON; the tests encrypt them for the
  repository's backend when copying them into it.
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

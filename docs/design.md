# bucse design

This document describes how bucse works and why. For how to build, test
and contribute, see [CONTRIBUTING.md](../CONTRIBUTING.md).

## Philosophy and priorities

Ranked: when goals conflict, the higher one wins.

1. Never lose or corrupt user data.
2. Never leak plaintext, keys or metadata to the destination, or anywhere
   else (unless the `none` encryption backend is used).
3. Simplicity and auditability of code over features.
4. Proven technology (C99 for the code, Python for tests).
5. A small number of dependencies.
6. Performance.
7. A test suite that lets us verify that features work well.

## Core concepts (glossary)

- **Repository** — a directory containing encrypted data in the bucse
  format, mounted with `bucse-mount`.
- **Destination** — a storage backend that gives access to a repository in
  a specific way, e.g. local, ssh.
- **Action** — an operation the user performs on their files that needs to
  be saved in the repository, e.g. create file, edit file, delete file,
  create directory.
- **Action file** — a file in the `$repository/actions/` directory that
  describes one action.
- **actionsPending** — an in-memory dynamic array of actions, either just
  performed by the user or just read from action files. They are sorted by
  time, deduplicated, executed (handling out-of-order actions) and moved to
  the `actions` dynamic array.
- **Packing (tar) of actions** — an optimisation: many action files are
  packed into one tar file, so they can be handled as one big pack instead
  of one by one.
- **Encryption backend** — the encryption method of a repository: `aes` or
  `none`.
- **Cache** — a simple, generic mechanism that keeps data in memory to
  speed things up.

## Architecture in brief

- `bucse-init` — the program that initialises a repository.
- `bucse-mount` — the program that mounts a repository.
- `operations/` — FUSE callbacks.
- `destinations/` — storage backends.
- `encryption/` — encryption backends.

## Repository layout

A repository is a directory with the following contents:

- `actions/` — encrypted action files.
- `storage/` — encrypted data blocks.
- `repository.json` — basic information about the repository, such as its
  name, and the encryption backend it uses. This file is never encrypted.
- `repository` — an encrypted file with metadata about the whole
  repository. For now it only contains the repository creation time.

## Invariants and assumptions

These must ALWAYS hold. Breaking any of them is a bug.

- **Threat model:** only clients with the password can access the data.
  The server that stores the repository is not trusted.
- Nothing unencrypted ever leaves the client (unless the `none` backend is
  used).
- Action files are immutable once written: new actions only add new files.
  The only exception is dedicated maintenance tools, e.g. for packing action
  files or cleaning up redundant sequences of actions.
- **Concurrency:** several clients can mount the same repository at once.
- **Compatibility:** the repository format must always stay backwards
  compatible.

## Durability

A crash or a network failure must not leave the repository in an
inconsistent state. This rests on two rules:

- **A partially written action file is ignored.** An action file is a JSON
  document (encrypted, unless the `none` backend is used). A truncated file
  is never valid: an encrypted one fails decryption, and a plaintext one is
  not valid JSON. It is rejected as a whole, so a half-written action is
  never applied.
- **Blocks are written before the action that uses them.** When a file is
  flushed, all its new blocks are stored in `storage/` first. The action file
  is written only if every block was stored successfully, so a valid action
  never points to missing blocks. A failed flush can leave unused blocks in
  `storage/`, but they don't affect the repository's contents.

As a result, an interrupted operation is either fully applied or not
applied at all. The worst case is that the last action is lost.

## Non-goals

- bucse is not meant to be a general-purpose network filesystem. Still,
  using it like one is a good way to test how robust it is.

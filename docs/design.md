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
  time (actions with the same time, e.g. the two actions of a rename, by
  their other fields, so every client applies them in the same order and
  identical ones are adjacent), deduplicated, executed (handling
  out-of-order actions) and moved to the `actions` dynamic array.
- **Action time** — a new local action gets the current time, but always
  later than every action already applied (a Lamport clock), so it is
  ordered after everything the client has seen, even if the client's clock
  steps back or another client's clock is ahead. Actions of clients that
  haven't seen each other keep an arbitrary but consistent order. A client
  whose clock is far ahead pushes the times (and mtimes) of everyone's later
  actions after its own.
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
- `bucse-archive REPOSITORY` — a maintenance tool that packs the action
  files of a repository (a local path, `file://` or `ssh://`, as for
  `bucse-mount -r`) into one tar file and removes them. Only files last
  modified more than 5 minutes ago are packed, since a newer one may still be
  being uploaded (action files are written under their final name). The tar
  file is written under a name that starts with a dot (bucse-mount ignores
  such files) and renamed when complete. A client that has the repository
  mounted meanwhile already has the packed actions: when it reads the tar
  file, they are dropped as duplicates (found by their contents, not by the
  file name), and a file removed after it listed the directory is skipped.
  For `ssh://` it runs itself on the remote host through ssh.
- `operations/` — FUSE callbacks.
- `destinations/` — storage backends. Each destination decides on its own
  how it finds new action files (its `tick()` is called about once a second),
  so destinations can work very differently, e.g. list the actions directory,
  or wait for notifications. A full listing is still made at least every
  `check_interval` seconds (`-i`, default 10) as a fallback.
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
- Everything read from the destination is untrusted input: sizes are
  checked before data is copied or decrypted into a buffer. An action file
  (or a tar entry) larger than 1 MB (`MAX_ACTION_LEN`) or a block larger
  than its file's block size is rejected. So is an addFile or editFile
  action whose block size isn't 0 or a power of 2 from 512 B to 128 MB (the
  only ones bucse writes), whose size is negative, or whose content has fewer
  blocks than its size needs.
- Action files are immutable once written: new actions only add new files.
  The only exception is dedicated maintenance tools, e.g. for packing action
  files or cleaning up redundant sequences of actions.
- **Concurrency:** several clients can mount the same repository at once.
- **Compatibility:** the repository format must always stay backwards
  compatible.

## Passphrase

The passphrase is given to `bucse-init` and `bucse-mount` in a way that
other local users can't see:

- typed on the terminal, with echo off (the default when stdin and stdout
  are a terminal and no passphrase option is given; `bucse-init` asks twice,
  since a typo would make the repository unreadable), or
- read from a file with `-P FILE` (`--passphrase-file=FILE`, and
  `-o passphrase_file=FILE` for `bucse-mount`): its first line, without the
  newline; `-` is stdin. It's read with `read()`, not stdio, so no copy is
  left in a buffer that can't be wiped.

Stdin is read only when asked for with `-P -`, never just because it isn't a
terminal. `-p` (`-o passphrase=`) still works for compatibility, with a
warning: a command line can be read by every local user from
`/proc/<pid>/cmdline` and `ps`, and stays in the shell history. Right after
parsing, both programs zero the passphrase in their original argv, so it's
visible only until then (on Linux; other systems may keep their own copy).

## Durability

A crash or a network failure must not leave the repository in an
inconsistent state. This rests on two rules:

- **A partially written action file is ignored until it is complete.** An
  action file is a JSON document (encrypted, unless the `none` backend is
  used). A truncated file is never valid: the document is a single JSON
  array, and any prefix of it lacks the closing `]`, so it is not valid
  JSON. Decryption alone doesn't guarantee this: `aes` is AES-CBC, and a
  file cut off at a 16-byte block boundary can pass the padding check by
  chance, but it then decrypts to a prefix of the plaintext, which the JSON
  parser rejects (the same holds for the `repository` file, a single JSON
  object). This protects against accidental truncation only, not against a
  server that modifies files on purpose (`aes` has no integrity check). A
  truncated file is rejected as a whole, so a half-written action is never
  applied. A rejected file, or one that can't
  be read, is not marked as handled: bucse-mount reads it again on the next
  check, so an upload that was still in progress is picked up when it
  completes. A tar file of packed actions is rejected if it can't be
  unpacked or doesn't end with the end-of-archive marker (two 512-byte
  blocks of zeros, which tar files must have); the entries read before the error are applied, and are removed
  as duplicates on the retry. Once a tar file is unpacked, an entry inside it
  that is rejected is ignored.
- **Blocks are written before the action that uses them.** When a file is
  flushed, all its new blocks are stored in `storage/` first. The action file
  is written only if every block was stored successfully, so a valid action
  never points to missing blocks. A failed flush can leave unused blocks in
  `storage/`, but they don't affect the repository's contents. The blocks
  are also on disk before the action file is written, so this holds after a
  power loss too. They aren't synced one by one: the local destination calls
  `syncfs()` on `storage/` once before the action file is written, and the
  ssh destination fsyncs all the blocks written since the last action at
  that point (`fsync@openssh.com`).

As a result, an interrupted operation is either fully applied or not
applied at all. The worst case is that the last action is lost.

A file's local changes (writes, a truncate, a create) are written to the
repository when the file is closed, before `close()` returns, and a truncate
by path (`truncate()`, with no open file) before it returns. Every action
file is fsynced (and, for the local destination, so is `actions/`) before
the operation returns, so an operation that has returned survives a power
loss. The same goes for `repository.json` and `repository` in `bucse-init`.
SFTP can't sync a directory, so with the ssh destination the new files'
directory entries depend on the server's filesystem (ext4, for example,
commits them with the file's fsync). If the server doesn't support
`fsync@openssh.com`, bucse-mount warns and nothing is synced.

## Concurrent changes

Another client's action never drops local changes that haven't been flushed
yet (pending writes, a pending truncate, a file created but not flushed):

- **editFile:** the new content becomes the base, the local changes are
  applied on top of it when the file is flushed (a pending truncate sets its
  size: it cuts the new content, or extends it with zeroes).
- **addFile** of a file that was created locally but not flushed yet: if
  nothing was written to it, the added file is taken; otherwise the local one
  is kept and its flush writes an editFile.
- **removeFile:** the file is kept, and its flush adds it again.
- **removeDirectory** of a directory that isn't empty: the directory is kept.
  If it holds something that is in the repository (the other client hadn't
  seen it yet), every client keeps it. If it holds only local data, it is
  marked, and before the next action inside it an addDirectory is written
  for it (and for its marked parents).
- **addFile** of a path where a directory exists, or **addDirectory** where
  a file exists: if the existing entry is in the repository, the action is
  rejected (every client applies actions in the same order, so they all
  reject the same one). If it holds only local data (a file not flushed yet,
  or a directory marked as above), the local entry is renamed to
  `name.conflict` (`name.conflict2`, ... if that is taken), and its flush or
  next action writes it under the new name. FUSE operations are path based,
  so a file still open under the old name can't be written through that
  handle any more; its data is kept under the new name.
- An **addDirectory** of a directory that already exists keeps the existing
  one and its contents.

## Non-goals

- bucse is not meant to be a general-purpose network filesystem. Still,
  using it like one is a good way to test how robust it is.

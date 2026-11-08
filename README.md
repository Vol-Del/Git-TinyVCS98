# TinyVCS98 — Git-compatible client for Windows 98

TinyVCS98 builds an executable named **`git.exe`** and uses normal Git repository data. It is intended for small retro projects such as Turbo Pascal 7.0 games developed on a Windows 98 IBM ThinkPad.

This is just a small half-baked tries to make it work.

## What it supports

### Local repository operations

- Standard SHA-1 Git blob, tree, commit, and tag objects.
- Standard loose-object storage in `.git/objects`.
- Git index versions 2 and 3; writes index v2.
- Read loose objects and Git packfiles with pack index v1/v2, pack versions 2/3, `OFS_DELTA`, and `REF_DELTA`.
- Read symbolic/detached `HEAD`, loose refs, and `packed-refs`.
- `init`, `add`, `rm`, `commit`, `status`, `log`, and `restore`.

### Network operations

The client implements Git smart HTTP **protocol v0 over** **`http://`** **and** **`https://`**:

- `clone` downloads advertised branch and tag objects, initializes a normal `.git` repository, and checks out the advertised default branch.
- `fetch` downloads the remote's advertised branch/tag history and updates remote-tracking refs and local tag refs. Incoming packfiles are validated and unpacked into loose Git objects.
- `pull` performs `fetch` followed by a **fast-forward only** update. It refuses to overwrite staged, unstaged, or untracked files. Non-fast-forward updates require regular Git on another machine.

It works with normal SHA-1 Git repositories and has been tested in both interoperability directions against regular Git.

## HTTPS and Windows 98 networking

The network transport uses **WinSock 1.1**, available on Windows 98. `https://` remotes work **without any DLLs, Schannel, or Windows crypto support**: `git.c` contains its own TLS 1.2 client, so the single `git.exe` talks directly to HTTPS hosts.

What the built-in TLS client does:

- TLS 1.2 with `ECDHE-RSA-AES128-GCM-SHA256` / `ECDHE-ECDSA-AES128-GCM-SHA256` (key exchange on secp256r1 or secp384r1), SNI, and server-signature verification using RSA (PKCS#1 v1.5) or ECDSA (P-256/P-384) with SHA-256/384/512.
- Full certificate-chain verification against a PEM CA bundle, host-name checking (DNS and IP subjectAltName, wildcards), and validity-date checks.
- HTTP redirects (301/302/303/307/308, never https→http, credentials are not forwarded to a different host) and HTTP Basic authentication via `https://user:token@host/path.git`.

Setup:

1. Keep **`ca-bundle.crt`** in the same folder as `git.exe` (a Mozilla-derived root-certificate snapshot is included). Refresh it occasionally from [https://curl.se/ca/cacert.pem](https://curl.se/ca/cacert.pem). Use `set GIT_SSL_CAINFO=C:\path\to\bundle.pem` to point at a different file, for example one containing only your own server's certificate.
2. Make sure the **system clock and date are correct** — Windows 98 machines often lose their clock. Certificates are rejected when expired or not yet valid. `set GIT_SSL_NO_VERIFY_DATE=1` skips only the date check.
3. `set GIT_SSL_NO_VERIFY=1` disables chain and host-name verification (a warning is printed). This is insecure; use it only for testing.
4. For GitHub/GitLab-style tokens use `git clone https://USER:TOKEN@github.com/owner/repo.git`. The token is stored in `.git/config`, like regular Git does.

Limits and honest caveats:

- **TLS 1.3 is not implemented.** Hosts that still accept TLS 1.2 (including the major Git hosting services at the time of writing) work; a server that only speaks TLS 1.3 cannot be used. Only AES-128-GCM suites are offered, and there is no session resumption, client-certificate support, certificate revocation checking, or name-constraint/key-usage policy checking. Secret-dependent operations are not constant-time, which matters only against a local attacker who can measure timing on your machine.
- The cryptography was written for this project and checked against OpenSSL (RSA/P-256/P-384 chains, and the self-signatures of 115 real root certificates), but it has **not** had an independent security audit and has not been tested here against live public hosts. Treat it as best-effort protection suitable for a hobby/retro machine, and prefer short-lived, narrowly scoped access tokens.
- The random-number seed comes from the Windows CryptoAPI (`CryptGenRandom`, loaded dynamically; present on Windows 98) mixed with clock/cursor/timer data. A warning is printed if no system source is available.
- A handshake takes well under a second on a Pentium-class CPU; bulk decryption speed is a few MB/s at 200–400 MHz.
- Plain `http://` remains available but is unencrypted; use it only on a trusted LAN.

Other transport constraints: IPv4 only (no IPv6 literals), HTTP/1.0 requests, Git smart protocol v0 only, no proxy support. The server must enable smart HTTP upload-pack for clone/fetch and receive-pack for push.

## Build for Windows 98

Recommended toolchain: **Open Watcom C/C++ 2.x**, 32-bit Win32 target. The build uses WinSock 1.1 and links `wsock32.lib`:

```
build-win32.bat
```

The resulting executable is `git.exe`. Copy it to a directory on the ThinkPad's `PATH`, or run it from the project directory. If another Git is installed, make sure the TinyVCS98 directory comes first in `PATH` when using the ThinkPad.

The implementation is conservative C and includes its own SHA-1 and DEFLATE implementation; it does not require OpenSSL or zlib DLLs. **A native Win98 binary is not bundled or claimed to have been tested on physical Win98 hardware.** Open Watcom and a Windows 98 runtime were not available in the build environment. The build script is provided, but the actual Win98 compiler/linker build remains to be verified on the target machine.

## 

## Quick start on Windows 98

Open a command prompt in your Turbo Pascal project directory:

```
git init
git add GAME.PAS
git commit -m "Initial game version"
```

Stage all project files and commit a change:

```
git add -A
git commit -m "Add title screen"
git status
git log
```

Restore one tracked file from an earlier commit (use the full 40-character commit ID shown by `git log`):

```
git restore 0123456789abcdef0123456789abcdef01234567 GAME.PAS
git add GAME.PAS
git commit -m "Restore earlier game code"
```

## Network examples

Clone a repository. When the destination is omitted, the folder name is derived from the repository URL:

```
git clone http://192.168.1.10:8080/game.git
```

Or choose a directory explicitly:

```
git clone http://192.168.1.10:8080/game.git GAME
```

For an existing repository created with `git init`, configure a remote:

```
git remote add origin http://192.168.1.10:8080/game.git
git remote -v
```

Synchronize the current branch:

```
git fetch
git pull
git push
```

`git clone` creates an `origin` remote automatically. `git push` sends only the current local branch. `git pull` is fast-forward only; it will not create a merge commit or resolve conflicts.


## Unsupported Git features

- SHA-256 object-format repositories.
- Git protocol v2, SSH, native `git://`, TLS 1.3, HTTP proxies, and NTLM/Digest/Negotiate authentication (Basic only).
- Branch creation/switching, merge/rebase, reset, cherry-pick, tags commands, submodules, Git LFS, signed commits/tags, and reflog management.
- Index v4, split/sparse index, unmerged index entries, worktree pointer files, and some less common index extensions.
- `.gitignore` pattern matching, symlinks, executable-bit semantics, and full Git pathspec behavior.
- Push of multiple refs, ref deletion, force-push, and thin-pack negotiation.
- `pull` merge/rebase; only clean fast-forward updates are implemented.

Use regular Git on a modern computer for these operations. Back up repositories before using an experimental retro-targeted client.

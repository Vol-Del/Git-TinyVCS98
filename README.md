# TinyVCS98 — Git-compatible client for Windows 98

TinyVCS98 builds a single executable named **`git.exe`** that reads and writes normal Git repositories. It is meant for small retro projects, such as which developed on a Windows 98. In my case for developing small games using Turbo Pascal on my Thinkpad T42

It is a compact Git-compatible client, not a full reimplementation of Git. A regular Git installation can inspect, continue, branch, merge, and host repositories created or updated by this program. It’s just works for now with minimal usable tool,

## 

## Features

**Local repositories**

- Standard SHA-1 Git blob, tree, commit, and tag objects, stored loose in `.git/objects`.
- Git index versions 2 and 3 (index v2 is written).
- Reads loose objects and packfiles (pack index v1/v2, pack versions 2/3, `OFS_DELTA`, `REF_DELTA`).

**Network (smart HTTP protocol v0, over** **`http://`** **and** **`https://`)**

- `clone`, `fetch`, `push`, and fast-forward-only `pull`.
- `remote add` and `remote -v` (passwords in URLs are masked).
- Built-in TLS 1.2 client: **no DLLs, no Schannel, no OpenSSL** needed on Windows 98.

**Settings**

- `git config --global` for `user.name`, `user.email`, and `user.token`, so you can use plain repository URLs without typing your token each time. (It was a pain in the ass)

## Quick start

Open a command prompt in your project folder:

```
git init
git add -A
git commit -m "Initial version"
git status
git log
```

Connect to a remote, then synchronize:

```
git remote add origin https://github.com/YOUR-USER/YOUR-REPO.git
git push
```

Later:

```
git fetch
git pull
git push
```

`git clone` creates an `origin` remote automatically. `git push` sends only the current local branch. `git pull` is fast-forward only: it never creates a merge commit and never resolves conflicts, I have a problems with it’s realization.

## Setting up GitHub access

Because stupid GitHub no longer accepts account passwords for Git operations. Create a **personal access token** on GitHub (Settings → Developer settings → Personal access tokens) with permission to read and write the repository contents.

Then save your details once:

```
git config --global user.name "YOUR-USER"
git config --global user.email "you@example.com"
git config --global user.token "YOUR-TOKEN"
```

From now on, use plain URLs:

```
git clone https://github.com/YOUR-USER/YOUR-REPO.git
```

How it works:

- The settings are stored in a file named **`gitconfig`** **next to** **`git.exe`** (for example `C:\SOFTWARE\GIT\gitconfig`). Windows 98 has no per-user home folder, so this is the shared location. Set the `GIT_CONFIG_GLOBAL` environment variable to use a different file. **Keep this file private**: it contains your token.
- `user.name` and `user.email` become the commit author for repositories that still have the default identity.
- `user.name` and `user.token` are sent as HTTP Basic credentials to `https://` remotes whose URL contains no credentials. They are **never** sent over plain `http://` and **never** forwarded to a different host after a redirect. The token is not written into `.git/config`.
- GitHub ignores the user name when a token is used, so it need not match your account name exactly. Other hosts may be stricter.

To read a saved value, run the command without a value. To remove it, pass an empty value:

```
git config --global user.token
git config --global user.token ""
```

## Command reference

| Command | What it does |
| --- | --- |
| `git config --global user.name "NAME"` | Set the commit author name (and the user name sent with the token). |
| `git config --global user.email "EMAIL"` | Set the commit author e-mail. |
| `git config --global user.token "TOKEN"` | Set the access token for `https://` remotes. |
| `git config --global KEY` | Print a saved value. |
| `git init` | Create a repository in the current folder. |
| `git clone URL [DIRECTORY]` | Download a repository and check out its default branch. |
| `git add FILE...` / `git add -A` | Stage files, or everything under the current folder. |
| `git rm FILE...` | Remove files from the index and working tree. |
| `git commit -m "MESSAGE"` | Commit the staged changes. |
| `git status` | Show staged, unstaged, and untracked files. |
| `git log` | Show commit history. |
| `git restore COMMIT FILE` | Restore a file from a commit (use the full 40-character ID). |
| `git remote add NAME URL` / `git remote -v` | Add or list remotes. |
| `git fetch` | Download remote branches and tags. |
| `git pull` | Fetch, then fast-forward the current branch. |
| `git push` | Send the current branch to the remote. |

Restoring an older version of a file:

```
git restore 0123456789abcdef0123456789abcdef01234567 GAME.PAS
git add GAME.PAS
git commit -m "Restore earlier game code"
```

## HTTPS and networking

The transport uses **WinSock 1.1**, available on Windows 98. The TLS 1.2 client is part of `git.c`, so the single `git.exe` talks directly to HTTPS hosts.

What the built-in TLS client does:

- TLS 1.2 with `ECDHE-RSA-AES128-GCM-SHA256` and `ECDHE-ECDSA-AES128-GCM-SHA256` (secp256r1 or secp384r1), SNI, and server-signature verification with RSA (PKCS#1 v1.5) or ECDSA (P-256/P-384) using SHA-256/384/512.
- Full certificate-chain verification against a PEM CA bundle, host-name checking (DNS and IP subjectAltName, wildcards), and validity-date checks.
- HTTP redirects (301/302/303/307/308), never from https to http, and credentials are not forwarded to a different host.

Setup:

1. Keep **`ca-bundle.crt`** in the same folder as `git.exe`. A Mozilla-derived root-certificate snapshot is included; refresh it occasionally from [https://curl.se/ca/cacert.pem](https://curl.se/ca/cacert.pem). To use a different file, run `set GIT_SSL_CAINFO=C:\path\to\bundle.pem`.
2. Make sure the **system date and clock are correct**. Windows 98 machines often lose their clock, and certificates are rejected when expired or not yet valid. `set GIT_SSL_NO_VERIFY_DATE=1` skips only the date check.
3. `set GIT_SSL_NO_VERIFY=1` disables chain and host-name verification (a warning is printed). This is insecure; use it only for testing.

## Building

Recommended toolchain: **Open Watcom C/C++ 2.x**, 32-bit Win32 target. From an 

Open Watcom command prompt: `wcl386 -q -bt=nt -l=nt -ox -k524288 -fe=git.exe git.c wsock32.lib`

and produces `git.exe`. Copy it, together with `ca-bundle.crt`, into a folder on your `PATH` (for example `C:\SOFTWARE\GIT`)

The code is conservative C89 and includes its own SHA-1, DEFLATE, and TLS implementations. Only `wsock32.lib` is linked.

## Troubleshooting

| Problem | What to try |
| --- | --- |
| `unsupported config key` | Use exactly `user.name`, `user.email`, or `user.token`, and put a space before the quoted value: `git config --global user.name "Name"`. |
| `cannot write ...\gitconfig` | Make sure the folder containing `git.exe` exists and is writable (not a CD or write-protected disk), or set `GIT_CONFIG_GLOBAL` to a writable file. |
| `authentication is required` | Set `user.token` (and `user.name`), or use a `https://USER:TOKEN@host/path.git` URL. Check that the token has not expired and has access to the repository. |
| Certificate rejected / "not yet valid" / "expired" | Check the system date and clock. Refresh `ca-bundle.crt`. |
| Connection or handshake fails on one host only | The host may require TLS 1.3, which is not supported. |
| `user.token is not sent over plain http://` | The token is deliberately sent only over `https://`. Use an `https://` URL. |
| `pull` refuses to continue | You have staged, unstaged, or untracked files, or the update is not a fast-forward. Commit or move them, or merge using regular Git on another machine. |

## Unsupported Git features

- Git protocol v2, SSH, native `git://`, TLS 1.3, HTTP proxies, and NTLM/Digest/Negotiate authentication (Basic only).
- Branch creation and switching, merge, rebase, reset, cherry-pick, tag commands, submodules, Git LFS, signed commits and tags, and reflog management.
- Index v4, split or sparse index, unmerged index entries, worktree pointer files, and some less common index extensions.
- `.gitignore` matching, symlinks, executable-bit semantics, and full pathspec behavior.
- Pushing multiple refs, deleting refs, force-push, and thin-pack negotiation.
- Other `git config` forms: only `--global` with `user.name`, `user.email`, and `user.token` is supported.

Use regular Git on a modern computer for these operations. Back up your repositories before using this experimental git client.

## Project files

- `git.c` — the complete implementation, including the WinSock smart-HTTP transport and the built-in TLS 1.2 client.
- `ca-bundle.crt` — root certificates used to verify HTTPS servers. (In the future maybe update is needed)

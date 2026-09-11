# GitHub authentication and PAT migration

## Recommended path

Do not put a GitHub PAT in this repository, PlatformIO build flags, `LocalConfig.h`, a Makefile, or a GitHub Actions secret for this build. The current Actions workflow only needs `contents: read` and `actions/checkout` with `persist-credentials: false`.

For local Git operations, prefer GitHub CLI browser authentication or SSH. GitHub recommends fine-grained PATs instead of classic PATs when a token is actually required. A classic PAT that has expired is not renewed in place: create a replacement credential, verify it, then revoke the old credential if it is still present.

### GitHub CLI

Install GitHub CLI, then authenticate interactively:

```sh
gh auth login
gh auth status
git remote -v
```

Choose GitHub.com, your normal Git protocol (SSH is preferred for a long-lived local setup), and browser authentication. Do not paste a token into the shell command line. This keeps credentials outside the repository and avoids shell history leakage.

### If a token is unavoidable

Create a fine-grained PAT restricted to only the repository that needs it and grant the minimum repository permissions. For ordinary Git push/pull, use the smallest repository permissions offered by GitHub for that operation. Give it a short expiration and store it in the OS credential manager or GitHub CLI, not in a file tracked by Git.

Do not use a classic PAT merely because an old script used one. Fine-grained PATs can be repository-scoped, while classic PATs can reach every repository the user can access.

### When the old classic PAT is already expired

1. Stop using the old PAT. An expired token cannot be used as a safe migration credential.
2. Check GitHub Settings → Developer settings → Personal access tokens and revoke any old token that is still active.
3. Use `gh auth login` for local Git access, or create a fine-grained PAT only if a token is genuinely required.
4. Run `make preflight` before the first push.
5. If the old PAT was ever committed, treat it as compromised even if it is now expired: rotate/revoke it and clean the Git history before publishing that history.

### GitHub Actions

This repository does not need a PAT to compile firmware. `actions/checkout` obtains the workflow's ephemeral `GITHUB_TOKEN`; `persist-credentials: false` prevents checkout from leaving that credential in the local Git configuration. The workflow does not pass a token to PlatformIO or the compiler.

If a future deployment needs a cloud credential, prefer GitHub Actions OIDC and short-lived cloud credentials rather than adding a long-lived PAT to the workflow.

### Local verification

Run:

```sh
make preflight
make build
```

The preflight checks tracked content, staged changes, untracked files, reachable push history, credential-like paths, and Git remotes for embedded credentials. It deliberately suppresses matching secret content.

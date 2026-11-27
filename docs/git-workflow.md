# Git workflow

How a change gets from a working copy to `dev`, and from `dev` to `main`. It is the same for maintainers, contributors
and AI agents. Contributors without write access fork the repository first and open their pull requests from the fork
([CONTRIBUTING.md](../CONTRIBUTING.md)). Everything else below applies to them too.

## Branches

| Branch | Holds | Changed by |
|---|---|---|
| `main` | the released versions, each tagged `v<version>` | a pull request from `dev`, at a release |
| `dev` | the next version, always working | pull requests from topic branches |
| `fix/<topic>`, `feature/<topic>`, `docs/<topic>`, `test/<topic>` | one topic in progress | its author, pushed as often as useful |

`main` and `dev` are protected on GitHub:

- a change reaches them only through a pull request;
- the `checks` workflow must pass;
- every review conversation must be resolved first;
- force pushes are refused, and neither branch can be deleted.

No approval is required, since the project has a single maintainer for now. Administrators *can* push past the
protection, but nobody does: a direct push skips the CI run on GitHub's English Windows, which has caught failures
that this Chinese-Windows machine never shows.

The repository is public. History is never rewritten on `main` or `dev`. That means:

- no `git push --force`;
- no orphan branches or resets of a published branch;
- no `git fetch --prune-tags`.

The local branch `old-dev-private` holds the history from before the repository went public, and it is never pushed.

## Making a change

1. Start from an up-to-date `dev`:

   ```bat
   git switch dev
   git pull --ff-only
   git switch -c fix/<topic>
   ```

   Suppose the commits were already made on local `dev`, a round of fixes for instance. Make the branch from
   there instead: `git switch -c fix/<topic>`. The commits are then on both, and local `dev` catches up when the
   pull request is merged.

2. Commit with the rules in [AGENTS.md](../AGENTS.md#git):
   - an English first line that says what changes for the user;
   - details below it when they help;
   - the SPDX line in new source files;
   - no `Co-Authored-By` trailer or any other AI-agent attribution.

   Each commit is one fix or one feature with its test. Keep a pull request to one topic. A round of related fixes
   (one testing session) is one topic.

3. Run the tests of what you touched while you work. Before the pull request, run `scripts\check_all.cmd` (or
   `/quick`). An AI agent runs `check_all` only when the person it works for asks for it, and says in the pull
   request which checks it ran.

## Opening the pull request

AI agents push and open pull requests only when the person they work for asks for it.

```bat
git push -u origin fix/<topic>
gh pr create --base dev --head fix/<topic> --title "<what changes for the user>" --body-file <file>
```

- The body follows [.github/pull_request_template.md](../.github/pull_request_template.md):
  - **What changes**: what a user or a Python developer sees differently; link issues with "Fixes #123";
  - **How it is tested**: the tests that fail without the change, and the checks that were run;
  - **Checklist**: tick what holds, and say why an item doesn't apply.
- It is English, like everything in the repository, and carries no AI-agent attribution line.
- Write the body to a file and pass `--body-file`, rather than quoting it on the command line. Shells mangle
  backslashes and `$`.

## Waiting for CI

The `checks` workflow ([.github/workflows/checks.yml](../.github/workflows/checks.yml)) runs on the pull request. It
runs the translation check, UI command coverage, the boundary check over the history, and the Python tests that need
no running YCGrid.

```bat
gh pr checks <number> --watch
```

When it fails, read the log (`gh run view <run-id> --log-failed`). Fix the cause on the same branch with a new commit
and push again. The pull request picks the commit up, and CI runs again. Never amend or force-push a branch that has
an open pull request.

## Merging

- Merge only when `checks` is green and every conversation is resolved. An AI agent merges only when the person asks
  for it, and otherwise reports that the pull request is ready.
- Use a merge commit, which keeps every commit of the branch with its own message and test:

  ```bat
  gh pr merge <number> --merge --delete-branch
  git switch dev
  git pull --ff-only
  ```

  Don't squash: a squashed round of fixes loses which test belongs to which fix.

## Releasing

A release goes from `dev` to `main` through a pull request too, then gets its tag. The full steps are in
[releasing.md](releasing.md): the version in five files, the changelog, the files and the tag.

```bat
gh pr create --base main --head dev --title "YCGrid <version>" --body-file <notes>
gh pr checks <number> --watch
gh pr merge <number> --merge
git switch main
git pull --ff-only
git tag -a v<version> -m "YCGrid <version>"
git push origin v<version>
git switch dev
```

The tag is pushed on its own. `main` itself only moves through the pull request.

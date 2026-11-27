# Releasing YCGrid

How a version goes from `dev` to the [Releases](https://github.com/ycgrid/ycgrid/releases) page. Each
release ships two things, built from the same `ycgrid.exe`:

| File | For | Contents |
|---|---|---|
| `YCGrid-<version>.zip` | people without a Python of their own (the main distribution) | `ycgrid.exe`, its own portable Python 3.12, uv, the `yc` package, the skill, the MCP tool list, the `examples` folder (the demo with `open_demo.cmd`, `txo_market_maker` and the research and trading screens), `install.cmd` / `uninstall.cmd`, `licenses\` |
| `ycgrid-<version>-py3-none-win_amd64.whl` | people who use their own Python (`pip install` the downloaded file) | the `yc` package with `ycgrid.exe` in `yc/_bin`, the skill and the licenses; no Python ([ADR 0009](adr/0009-wheel-and-the-installing-python.md)) |

Each file gets a `.sha256` file next to it. The release is not code-signed yet (see [Code signing](#code-signing)), so
the checksums are how a user checks that a download is the one published.

## 1. Prepare the version on `dev`

1. Give the release its number ([Semantic Versioning](https://semver.org/); while it starts with 0, a minor version
   may change the Python API and the notes say how). Write it in all five places; `tests.test_core.VersionTest`
   fails while they differ:

   | File | Where |
   |---|---|
   | `VERSION` | the only line |
   | `python/yc/__init__.py` | `__version__` |
   | `vcpkg.json` | `version-string` |
   | `mcp/tools.json` | `server.version` |
   | `apps/ycgrid/ycgrid.manifest` | `assemblyIdentity version`, with a fourth part `.0` |

2. In [CHANGELOG.md](../CHANGELOG.md), rename "Unreleased" to `## <version> (<date>)` and start a new empty
   "Unreleased" above it. Write each entry for the people who use YCGrid: what they see change.
3. Run the full checks, then the boundary check over the history of the published branches:

   ```bat
   scripts\check_all.cmd
   dist_release\runtime\python\python.exe -E -s scripts\check_boundary.py --history
   ```

4. Commit as `Version <version>` (only the version and the changelog) on a branch, bring it into `dev` through a
   pull request, then open a pull request from `dev` to `main` and merge it once `checks` passes
   ([git-workflow.md](git-workflow.md#releasing)). `main` is never pushed to directly.

## 2. Build the files

On a developer machine:

```bat
scripts\package.cmd --with-python /silent   :: dist\YCGrid-<version>.zip and .zip.sha256
scripts\build_wheel.cmd release             :: build\wheel\ycgrid-<version>-py3-none-win_amd64.whl and .whl.sha256
```

Or push the tag (step 3) and take the files from the **release** artifact of the [build workflow](../.github/workflows/build.yml),
which builds them from a clean clone. Before publishing, unzip the zip into a folder whose path has spaces and
non-ASCII characters, start `ycgrid.exe` and open the demo; install the wheel into a new virtual environment and run
`ycgrid`.

The wheel refuses a `licenses\` folder that holds a file [THIRD-PARTY-NOTICES.md](../THIRD-PARTY-NOTICES.md) doesn't
list, so a license left by an older build can't ship. A new component goes into that table first.

## 3. Tag and publish the release

```bat
git switch main
git pull --ff-only
git tag -a v<version> -m "YCGrid <version>"
git push origin v<version>
gh release create v<version> --title "YCGrid <version>" --notes-file notes.md ^
  dist\YCGrid-<version>.zip dist\YCGrid-<version>.zip.sha256 ^
  build\wheel\ycgrid-<version>-py3-none-win_amd64.whl build\wheel\ycgrid-<version>-py3-none-win_amd64.whl.sha256
```

`notes.md` is the version's CHANGELOG section, with the install steps from the README and the SHA-256 of both files.
It starts, under its first line, with which file to download, since most people need only the zip:

```markdown
> **Which file?** Download **`YCGrid-<version>.zip`**: it has everything, its own Python included. The `.whl` file is
> only for developers who want YCGrid to run their code in a Python environment of their own (venv or conda).
```

## 4. PyPI (not used for now)

Releases are published on GitHub only: people with their own Python download the wheel from the release page and
`pip install` that file. Uploading it to PyPI as well would let them run `pip install ycgrid`, but a version on
PyPI is final: it can be deleted, never replaced, its number can't be used again, and mirrors may keep a copy. This
section is kept for the day the project decides to publish there; until then, don't upload anything.

When it does:

- Upload only the wheel, never a source distribution (sdist). The wheel's `win_amd64` tag keeps pip on macOS, Linux
  and Windows ARM from installing it ("No matching distribution found"); with an sdist on PyPI, pip there would install
  `yc` from it and fail only when YCGrid is started.
- Change the README's "Install with pip" section to `pip install ycgrid`, and say there that YCGrid runs only on
  64-bit Windows 10 and 11.

The first time:

1. Create an account on [pypi.org](https://pypi.org) with two-factor authentication, and an API token (Account
   settings → API tokens, scope "Entire account" for the first upload).
2. Check and upload the wheel. uv runs twine without installing anything into a Python of yours:

   ```bat
   set UV_CACHE_DIR=%TEMP%\yc_uv_cache
   dist_release\runtime\uv.exe tool run --python dist_release\runtime\python\python.exe --from twine twine check --strict build\wheel\ycgrid-<version>-py3-none-win_amd64.whl
   dist_release\runtime\uv.exe tool run --python dist_release\runtime\python\python.exe --from twine twine upload build\wheel\ycgrid-<version>-py3-none-win_amd64.whl
   ```

   twine asks for the user name `__token__` and the token as the password. The first upload registers the name
   `ycgrid`.
3. Replace the account-wide token with one scoped to the `ycgrid` project, or set up
   [trusted publishing](https://docs.pypi.org/trusted-publishers/) for this repository's build workflow so no token is
   kept at all.

Every later release: step 2 with the project token. PyPI never accepts the same version twice: a broken upload is
fixed by a new version, not by replacing the file.

## Code signing

`ycgrid.exe`, `install.cmd` and the zip are not signed. Windows SmartScreen may warn the first time a downloaded
`ycgrid.exe` runs ("Windows protected your PC" → **More info** → **Run anyway**), and the README says so. To remove
the warning, sign `ycgrid.exe` with a code-signing certificate in `package.cmd` before it is zipped and in the wheel;
open-source projects can get one at no cost (SignPath's program for open-source projects signs builds made by a CI
workflow such as `build.yml`).

## Going public (once)

What GitHub needs when the repository is made public, or when it moves to a new repository:

- [ ] A new repository starts from the current tree as its first commit: run `check_boundary.py --history` on it
      before the first push.
- [ ] Settings → General: visibility **Public**; a description and a homepage (the README or a site); a social
      preview image (Settings → General → Social preview), for example `docs/images/en/quote-board.png`.
- [ ] Settings → Branches: protect `main` and `dev` (pull requests only, the `checks` workflow must pass, no force
      pushes). Free accounts get branch protection only on public repositories.
- [ ] Settings → Code security: turn on **Private vulnerability reporting**; [SECURITY.md](../SECURITY.md) and the
      issue form's "Report a security problem" link rely on it. Turn on Dependabot alerts; `.github/dependabot.yml`
      keeps the workflows' actions current.
- [ ] Settings → General → Features: Discussions, if questions should not go to issues.
- [ ] Actions: run the build workflow once by hand and check that it passes.
- [ ] The copyright holder in [NOTICE](../NOTICE) and `apps/ycgrid/resources.rc.in` is the name the project wants
      on record.

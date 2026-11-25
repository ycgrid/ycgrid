## What changes

<!-- What a user (or a Python developer) sees differently. Link the issue: "Fixes #123". -->

## How it is tested

<!-- The test that fails without this change, and the checks you ran. -->

- [ ] A test that fails without this change (Catch2 in `tests/unit/`, Python in `python/tests/`, or a check in `scripts/feature_sections.py`)
- [ ] `scripts\check_all.cmd` passes (or `/quick`, saying which parts you couldn't run)

## Checklist

- [ ] Code, comments, docs and commit messages are in English; UI text uses `TR(...)` and `i18n/zh-TW.json` is updated
- [ ] A new UI command has its Python entry in `schema/commands.yaml`; new Python members are in `object-model.md`
- [ ] `.ycg` files written before this change still open (new state in new fields, with a round-trip test)
- [ ] Not on the "Not planned" list in `docs/excel-parity-backlog.md`

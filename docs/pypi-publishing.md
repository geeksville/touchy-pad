# PyPI Publishing Guide

This document describes the PyPI publishing setup for the `touchy-pad` Python package.

## Overview

The project uses **trusted publishing** (OpenID Connect) for secure, token-free publishing to PyPI. Releases are automated via GitHub Actions.

## Setup (one-time)

### 1. Configure PyPI trusted publisher

Visit [pypi.org/manage/account/publishing/](https://pypi.org/manage/account/publishing/) and add:

- **PyPI Project Name**: `touchy-pad`
- **Owner**: `geeksville`
- **Repository name**: `touchy-pad`
- **Workflow name**: `release.yml`
- **Environment name**: `pypi`

> **The workflow name must be `release.yml`** — the file that contains the
> `publish-pypi` job. PyPI matches the OIDC token against the *workflow
> filename* (plus owner and repository), and it cannot use a reusable workflow as
> the publishing workflow ([PyPI troubleshooting](https://docs.pypi.org/trusted-publishers/troubleshooting/)
> → "Reusable workflows on GitHub", warehouse#11096). That is why the publish job
> lives inline in `release.yml` instead of in its own reusable
> `.github/workflows/publish-pypi.yml`, and why both the release trigger and the
> manual `workflow_dispatch` run present the same identity. If the PyPI entry
> says anything else, the upload fails with `invalid-publisher`.

For testing, repeat at [test.pypi.org/manage/account/publishing/](https://test.pypi.org/manage/account/publishing/) with environment name `testpypi` and workflow name `testpypi.yml`.

### 2. Create GitHub environments

In the GitHub repository settings → Environments, create:

1. **`pypi`** environment (for production)
   - Add a required reviewer if you want manual approval before publishing

2. **`testpypi`** environment (for dry runs)

## Publishing workflow

### Production release

1. Update version in `VERSION` file (line 1):
   ```bash
   just bump-version 0.2.0
   ```

2. Commit and push:
   ```bash
   git add VERSION app/pyproject.toml
   git commit -m "chore: bump version to 0.2.0"
   git push
   ```

3. Create a GitHub release:
   - Go to Releases → Draft a new release
   - Create a new tag: `v0.2.0`
   - Title: `v0.2.0`
   - Auto-generate release notes or write your own
   - Publish release

4. The workflow will:
   - Build the wheel and sdist
   - Generate cryptographic attestations
   - Publish to PyPI via trusted publishing
   - Upload artifacts to the GitHub release

### Manual publish (production)

`release.yml` also carries a `workflow_dispatch` trigger, so the PyPI publish can
be run on its own, without cutting a release:

1. Go to Actions → **Release** → **Run workflow**
2. Pick the branch or tag to publish from (e.g. `main`, or the release tag), then
   click **Run workflow**

A manual run is deliberately reduced to **PyPI only**: `test-app` (ruff + pytest +
`poetry build`) followed by `publish-pypi`. The release-only jobs — the Rust gate,
the firmware and OpenDeck plugin builds, the crates.io publish and the
release-asset upload — are gated on `github.event_name == 'release'` and are
skipped, so nothing is attached to a (nonexistent) GitHub release. The `pypi`
environment still applies, so a required reviewer must approve if you configured
one, and nothing is uploaded unless the tests pass.

The version that gets published is whatever is in `app/pyproject.toml` (kept in
sync with the root `VERSION`) on the ref you selected. Re-publishing a version
that is already on PyPI fails with `File already exists` — bump the version
instead of re-running.

### Test release (dry run)

To test the build and publish to TestPyPI without affecting production:

1. Go to Actions → **Publish to TestPyPI (dry run)** → **Run workflow**
2. Click **Run workflow**

The package will be published to [test.pypi.org/project/touchy-pad](https://test.pypi.org/project/touchy-pad/).

## Verification

After publishing, verify the package:

```bash
# Install from PyPI
pip install touchy-pad

# Verify version
touchy --version

# Check package metadata
pip show touchy-pad
```

## Attestations

Starting with recent PyPI releases, the workflow generates **build provenance attestations** for all distributions. These are cryptographically signed proof that:

- The artifacts were built by the specific GitHub Actions workflow
- The source matches the tagged commit
- No tampering occurred between build and publish

Users can verify attestations with:

```bash
pip install touchy-pad --use-feature=truststore
```

PyPI displays attestations on the project's file page.

## Troubleshooting

**Error: "403 Forbidden" during publish**
- Verify trusted publisher is configured correctly on PyPI
- Ensure the environment name in the workflow matches PyPI configuration
- Check the repository/owner/workflow names match exactly

**Error: "Package already exists" / "File already exists"**
- You cannot re-upload the same version. Bump the version number and create a new
  release — or, for a manual (`workflow_dispatch`) publish, make sure the ref you
  selected carries the version you intend to publish, and that it is not already
  on PyPI.

**Build fails on "Generate proto bindings"**
- Ensure `just`, `grpcio-tools`, and `nanopb` are installed in the workflow
- The proto bindings are regenerated before each build to ensure consistency

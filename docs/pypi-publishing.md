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
- **Workflow name**: `publish-pypi.yml`
- **Environment name**: `pypi`

For testing, repeat at [test.pypi.org/manage/account/publishing/](https://test.pypi.org/manage/account/publishing/) with environment name `testpypi`.

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

### Test release (dry run)

To test the build and publish to TestPyPI without affecting production:

1. Go to Actions → "Publish to PyPI" → "Run workflow"
2. Select `dry_run: true`
3. Click "Run workflow"

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

**Error: "Package already exists"**
- You cannot re-upload the same version
- Bump the version number and create a new release

**Build fails on "Generate proto bindings"**
- Ensure `just`, `grpcio-tools`, and `nanopb` are installed in the workflow
- The proto bindings are regenerated before each build to ensure consistency

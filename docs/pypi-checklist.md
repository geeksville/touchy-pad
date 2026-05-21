# PyPI Publishing Checklist

Use this checklist before your first PyPI release.

## Pre-release checklist

- [ ] **PyPI account**: Create account at [pypi.org/account/register](https://pypi.org/account/register/)
- [ ] **TestPyPI account**: Create account at [test.pypi.org/account/register](https://test.pypi.org/account/register/)
- [ ] **Verify package name**: Confirm `touchy-pad` is available on PyPI
- [ ] **Configure trusted publisher on PyPI**:
  - Go to [pypi.org/manage/account/publishing/](https://pypi.org/manage/account/publishing/)
  - Add pending publisher:
    - PyPI Project Name: `touchy-pad`
    - Owner: `geeksville`
    - Repository: `touchy-pad`
    - Workflow: `publish-pypi.yml`
    - Environment: `pypi`
- [ ] **Configure trusted publisher on TestPyPI**:
  - Go to [test.pypi.org/manage/account/publishing/](https://test.pypi.org/manage/account/publishing/)
  - Same settings but environment: `testpypi`
- [ ] **Create GitHub environments**:
  - Settings → Environments → New environment → `pypi`
  - Settings → Environments → New environment → `testpypi`
- [ ] **Test build locally**:
  ```bash
  cd app
  poetry build
  # Check dist/ folder contains .whl and .tar.gz
  ```
- [ ] **Verify proto files are included**:
  ```bash
  unzip -l app/dist/touchy_pad-*.whl | grep _proto
  # Should show: preferences_pb2.py, touchy_pb2.py, widgets_pb2.py
  ```
- [ ] **Update documentation**:
  - Ensure app/README.md is current
  - Check all links work
  - Update version in examples if needed

## First test release

- [ ] **Test on TestPyPI**:
  - Go to Actions → "Publish to PyPI" → "Run workflow"
  - Select branch: `main`
  - Dry run: `true`
  - Click "Run workflow"
  - Wait for completion
  - Check [test.pypi.org/project/touchy-pad](https://test.pypi.org/project/touchy-pad/)
- [ ] **Install from TestPyPI and verify**:
  ```bash
  pip install --index-url https://test.pypi.org/simple/ \
              --extra-index-url https://pypi.org/simple/ \
              touchy-pad
  touchy --version
  ```

## Production release

- [ ] **Bump version**: `just bump-version X.Y.Z`
- [ ] **Commit and push**: 
  ```bash
  git add VERSION app/pyproject.toml
  git commit -m "chore: bump version to X.Y.Z"
  git push
  ```
- [ ] **Create GitHub release**:
  - Tag: `vX.Y.Z`
  - Target: `main`
  - Title: `vX.Y.Z`
  - Description: Summarize changes
  - Publish release
- [ ] **Monitor workflow**: Watch Actions → "Publish to PyPI"
- [ ] **Verify on PyPI**: [pypi.org/project/touchy-pad](https://pypi.org/project/touchy-pad/)
- [ ] **Test installation**:
  ```bash
  pip install --upgrade touchy-pad
  touchy --version
  ```
- [ ] **Announce release**: Update README, docs, or community channels as needed

## Troubleshooting

**Workflow fails with "403 Forbidden"**
→ Trusted publisher not configured correctly. Double-check PyPI settings match workflow exactly.

**"Package already exists"**
→ Cannot reupload same version. Bump version and retry.

**Proto files missing in installed package**
→ Run `just build-proto-py` before `poetry build`. The CI workflow does this automatically.

**TestPyPI install fails with dependency errors**
→ Normal. TestPyPI doesn't have all dependencies. Use `--extra-index-url https://pypi.org/simple/`.

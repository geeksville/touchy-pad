Yes. You are looking for **`git-crypt`**.

Since you're a kernel guy, think of it as a transparent block device for your git index. It hooks into git's `clean` and `smudge` filters to encrypt files when you commit and seamlessly decrypt them when you checkout. The plaintext never hits the remote repository.

Here is how to isolate it to a single subdirectory and wire it to GitHub Actions without wanting to tear your hair out.

### 1. Setup and Target the Subdir

Initialize it and tell it exactly what to hijack using a standard `.gitattributes` file.

```bash
git-crypt init

# Target your specific directory
echo "my_secret_dir/** filter=git-crypt diff=git-crypt" > .gitattributes
git add .gitattributes

```

### 2. The Symmetric Key (For GitHub Actions)

While `git-crypt` supports GPG, managing GPG rings in CI pipelines is a special kind of hell. Export a symmetric key instead.

```bash
git-crypt export-key secret.key
base64 -w 0 secret.key > secret.key.b64

```

Copy the contents of `secret.key.b64` and paste it into a GitHub Repository Secret named `GIT_CRYPT_KEY`. (Obviously, do not commit the key itself).

### 3. Inflate in GitHub Actions

In your workflow YAML, you just decode the secret and unlock the repo before doing your build steps.

```yaml
steps:
  - uses: actions/checkout@v4
  
  - name: Install git-crypt
    run: sudo dnf install -y git-crypt # Or apt-get, depending on your runner OS

  - name: Decrypt secrets
    env:
      GIT_CRYPT_KEY: ${{ secrets.GIT_CRYPT_KEY }}
    run: |
      echo "$GIT_CRYPT_KEY" | base64 -d > repo.key
      git-crypt unlock repo.key
      rm repo.key

```

Locally, once unlocked, it stays unlocked. The files just look like plaintext on your filesystem, but `git diff` and `git push` will handle the ciphertext under the hood.
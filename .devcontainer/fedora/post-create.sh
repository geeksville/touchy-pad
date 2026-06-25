#!/usr/bin/env bash
set -e

echo "source $DEVCONTAINER_DIR/on-shell-start.sh" >> ~/.bashrc
echo "source $DEVCONTAINER_DIR/on-shell-start.sh" >> ~/.zshrc

# Enable VS Code shell integration
echo '[[ "$TERM_PROGRAM" == "vscode" ]] && . "$(code --locate-shell-integration-path bash)"' >> ~/.bashrc
echo '[[ "$TERM_PROGRAM" == "vscode" ]] && . "$(code --locate-shell-integration-path zsh)"' >> ~/.zshrc

# Sometimes ownership of critical homedir directories gets messed up - fix that
sudo chown "$(whoami):$(whoami)" $HOME/.*

# Fix git credential helper to use container's gh path instead of host's homebrew path (we might not be allowed to write to ~/.gitconfig though)
if command -v git &> /dev/null; then
    git config --global --unset-all credential.'https://github.com'.helper 2>/dev/null || true
    git config --global --add credential.'https://github.com'.helper '!/usr/bin/gh auth git-credential' || true
    git config --global --unset-all credential.'https://gist.github.com'.helper 2>/dev/null || true
    git config --global --add credential.'https://gist.github.com'.helper '!/usr/bin/gh auth git-credential' || true
fi

# Setup poetry if available
if command -v poetry &> /dev/null && [ -f "pyproject.toml" ]; then
    # Setup initial poetry venv (we store it in project so we can add the sb/starbash scripts to the path)
    poetry config virtualenvs.in-project true --local
    poetry install || true  # allow failure if dependencies can't be installed right now

    # Setup poetry build env
    poetry completions bash >> ~/.bash_completion

    # zsh completions - write to zsh function directory
    mkdir -p ~/.zfunc
    poetry completions zsh > ~/.zfunc/_poetry

    # install git hooks (if installed)
    poetry run pre-commit install | true
fi

# just completions
if command -v just &> /dev/null; then
    just --completions bash >> ~/.bash_completion
    mkdir -p ~/.zfunc
    just --completions zsh > ~/.zfunc/_just
fi

# for zsh completions: Add to fpath and enable completions if not already present
if ! grep -q "fpath+=~/.zfunc" ~/.zshrc; then
    echo 'fpath+=~/.zfunc' >> ~/.zshrc
    echo 'autoload -Uz compinit && compinit' >> ~/.zshrc
fi

if command -v atuin &> /dev/null; then
    echo "Logging into Atuin (on first run you'll be prompted for your credentials)..."
    atuin status || atuin login -u geeksville
fi
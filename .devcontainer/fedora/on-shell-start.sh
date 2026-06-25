#!/bin/bash

# --- Add any commands you want to run for each new shell ---

# Set Ptyxis terminal tab title to "starbash-dev" (if under that terminal)
if [[ -n "$PTYXIS_VERSION" ]]; then
    # For zsh, disable auto-title and use precmd hook to persist the title
    if [[ -n "$ZSH_VERSION" ]]; then
        DISABLE_AUTO_TITLE="true"
        precmd() { printf '\033]0;%s\007' "geeksville-dev"; }
    else
        # For bash, just set it once
        printf '\033]0;%s\007' "geeksville-dev"
    fi
fi

echo "🚀 Geeksville dev shell started!"

cd /workspaces/$PROJECT_NAME

# To find siril and other flatpaks
export PATH="$PATH:$HOME/.local/share/flatpak/exports/bin/"

# to reach our sb command
export PATH="$PWD/.venv/bin:$PATH"

# Limit OpenBLAS threads to prevent resource warnings when running tests
# GraXpert's numpy/scipy dependencies use OpenBLAS which tries to create too many threads
export OPENBLAS_NUM_THREADS=4


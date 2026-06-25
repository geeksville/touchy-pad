# Run inside a recent fedora based image with python3, pip3, git and sudo installed
FROM fedora:latest

# We use 'user' as the name to match what the zhephyr-build image already created
ARG USERNAME=vscode

## Setup a user with sudo support
USER root

# Add core python packages
# libxcb is needed for Qt6
RUN dnf update -y && dnf install -y \
        python3 python3-pip python3-tkinter sudo git flatpak libxcb curl just git protobuf-compiler \
        gcc make cairo-devel cairo-gobject-devel pkg-config python3-devel gobject-introspection-devel

# Install the github CLI
RUN dnf install dnf5-plugins
RUN dnf config-manager addrepo --from-repofile=https://cli.github.com/packages/rpm/gh-cli.repo
RUN dnf -y install gh --repo gh-cli

# Installing podman-remote to communicate with host's podman via DOCKER_HOST socket
RUN dnf install -y podman-remote && ln -sf /usr/bin/podman-remote /usr/bin/podman

# Create a non-root user if one doesn't exist, and give it sudo rights
RUN if ! id -u ${USERNAME} > /dev/null 2>&1; then \
        groupadd --gid 1000 ${USERNAME} && \
        useradd --uid 1000 --gid 1000 --create-home --shell /bin/bash ${USERNAME} && \
        echo "${USERNAME} ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/${USERNAME}-nopasswd; \
        fi

USER ${USERNAME}

# Some containers might not have a .local directory at all, don't fail in that case
RUN mkdir -p /home/${USERNAME}/.local/bin

# Add the local bin directory to the PATH. For pipx and flatpacks
ENV PATH="/home/${USERNAME}/.local/bin:$HOME/.local/share/flatpak/exports/bin/:${PATH}"

# pipx
RUN pip install --user --no-cache-dir --break-system-packages pipx 

# Python tooling for the host-side protobuf workflow (touchy.proto) and the
# embedded-side nanopb generator. Installed system-wide via --break-system-packages
# because Ubuntu Noble marks the system Python as externally-managed (PEP 668)
# and the devcontainer has no separate venv for these dev tools.
RUN sudo pip install --break-system-packages \
      protobuf \
      grpcio-tools \
      nanopb

# install poetry for the user
RUN pipx install --pip-args=--no-cache-dir poetry

# Install Atuin
RUN curl --proto '=https' --tlsv1.2 -sSf https://setup.atuin.sh | sh

USER root

# needed for vhs to make movies

# alas chromium-browser requires snap which is a whole other kettle of fish
# RUN apt-get install -y --no-install-recommends chromium-browser chromium-codecs-ffmpeg

# instead we install chrome - you might need to run the GUI once before using it from VHS
#RUN cd /tmp && \
#        wget https://dl.google.com/linux/direct/google-chrome-stable_current_amd64.deb && \
#        apt install -y ./google-chrome-stable_current_amd64.deb

# We need the docker CLI for build ghcr.io containers
# needed because i'm using old docker commands above - someday move this docker install to top and the update can leave
#RUN apt-get update
#RUN dnf install -y docker

# Make container smaller.  Do this last, so that if we change the container it is fast to use existing dnfs
# RUN dnf clean all && rm -rf /var/cache/dnf/*

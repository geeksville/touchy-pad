# SPDX-License-Identifier: GPL-3.0-or-later
"""Canonical on-device filesystem paths (Stage 68).

Single source of truth for the drive-prefixed directories the host uses
when talking to the device, shared by ``api/``, ``sim/``, ``client.py``
and ``cli.py`` so the literal ``"F:host/s/"`` etc. never appears inline.

Layout
------
* ``F:host/s/`` — screens. The boot/default chrome lives at
  ``F:host/s/default.pb`` (:data:`DEFAULT_SCREEN_PATH`); it carries the
  persistent prev/next chrome and a ``widget_ref(id="page")`` body that
  pages through :data:`USER_SCREENS_DIR`.
* ``F:host/uscr/`` — "user screens": one widget-layout ``.pb`` per
  top-level page that fills the body of the default chrome screen. Most
  users only ever touch files here.
* ``F:host/w/`` — generic widget-refs (TouchyDeck keys,
  :meth:`Touchy.widget_save`). Unchanged from earlier stages.
* ``F:host/images/`` — image assets.

The on-disk directory moved from ``host/screens/`` → ``host/s/`` in
Stage 68; there is no backward-compatible read of the old path.
"""

from __future__ import annotations

# Drive-prefixed directories (always trailing-slash terminated).
SCREENS_DIR = "F:host/s/"
USER_SCREENS_DIR = "F:host/uscr/"
WIDGETS_DIR = "F:host/w/"
IMAGES_DIR = "F:host/images/"

# Stage 87 — the logical "T:" ("temp") transient drive. The device
# resolves it to a PSRAM ramdisk where available, else a flash scratch
# area (see ``SysBoardInfoResponse.temp_is_flash``). Host writers of
# throwaway assets address it as ``T:...`` and never branch on the board.
TEMP_DRIVE = "T:"
#: Per-``ImageSource`` dynamic image assets (rewritten in place to
#: repaint a live widget). See :class:`touchy_pad.api.images_dynamic`.
DYNAMIC_IMAGE_DIR = "T:dyn/"
#: Host image cache (content-addressed). Mirrors the Rust
#: ``ImageCache``'s ``IMAGE_CACHE_ROOT``.
IMAGE_CACHE_DIR = "T:host/icache/"

# Filename of the boot/default chrome screen inside SCREENS_DIR.
DEFAULT_SCREEN_FILE = "default.pb"
DEFAULT_SCREEN_PATH = SCREENS_DIR + DEFAULT_SCREEN_FILE

# Stage lb9 — mutual-TLS certificate material for the HTTPS network API.
# Provisioned over USB via the normal FileWrite API (``touchy pref
# provision-mtls``) and read by the firmware at network bring-up. When all
# three are present the device serves HTTPS-with-mTLS; otherwise plaintext
# HTTP. Kept on persistent flash (F:).
TLS_DIR = "F:tls/"
#: Device's own server certificate (PEM).
TLS_SERVER_CERT_PATH = TLS_DIR + "server.crt"
#: Device's server private key (PEM).
TLS_SERVER_KEY_PATH = TLS_DIR + "server.key"
#: CA certificate the device verifies client certs against (PEM).
TLS_CLIENT_CA_PATH = TLS_DIR + "client_ca.crt"


def screen_path(name: str) -> str:
    """Drive-prefixed path of a screen file, e.g. ``F:host/s/home.pb``."""
    return f"{SCREENS_DIR}{name}.pb"


def user_screen_path(name: str) -> str:
    """Drive-prefixed path of a user-screen page, e.g. ``F:host/uscr/trackpad.pb``."""
    return f"{USER_SCREENS_DIR}{name}.pb"


def widget_path(name: str) -> str:
    """Drive-prefixed path of a generic widget-ref, e.g. ``F:host/w/key0.pb``."""
    return f"{WIDGETS_DIR}{name}.pb"


def dynamic_image_path(n: int) -> str:
    """Drive-prefixed path of a dynamic image asset, e.g. ``T:dyn/1.bin``.

    *n* is a process-global monotonic counter allocated per
    :class:`~touchy_pad.api.images_dynamic.ImageSource`; the path is
    stable for the life of the source so a rewrite repaints the widget
    in place.
    """
    return f"{DYNAMIC_IMAGE_DIR}{n}.bin"


__all__ = [
    "SCREENS_DIR",
    "USER_SCREENS_DIR",
    "WIDGETS_DIR",
    "IMAGES_DIR",
    "TEMP_DRIVE",
    "DYNAMIC_IMAGE_DIR",
    "IMAGE_CACHE_DIR",
    "DEFAULT_SCREEN_FILE",
    "DEFAULT_SCREEN_PATH",
    "screen_path",
    "user_screen_path",
    "widget_path",
    "dynamic_image_path",
]

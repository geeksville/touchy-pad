"""Shared exception types for the :mod:`touchy_pad.api` package.

These live in a dedicated, dependency-free module so that both the
low-level :mod:`touchy_pad.api._transport` layer and the high-level
:mod:`touchy_pad.api.client` layer can import them without creating a
circular dependency (``client`` imports from ``_transport``, so a class
both need must not be owned by either).
"""

from __future__ import annotations


class TouchyError(RuntimeError):
    """Raised on device/host interaction failures.

    Most commonly this means the device reported a non-OK ``ResultCode``;
    in that case ``code`` / ``code_name`` carry the wire values (e.g.
    ``code=INVALID_ARG``). Subclasses representing host-side failures —
    such as :class:`touchy_pad.api._transport.TransportPermissionError` — may
    pass an explicit ``message`` instead and leave the code fields at
    their defaults.
    """

    def __init__(
        self,
        code: int = 0,
        code_name: str = "",
        *,
        message: str | None = None,
    ) -> None:
        if message is None:
            message = f"device returned {code_name} ({code})"
        super().__init__(message)
        self.code = code
        self.code_name = code_name

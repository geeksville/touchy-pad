"""Qt renderers for proto :class:`touchy.Widget` messages.

A single :func:`build_widget` recursively turns a ``Widget`` tree into
a :class:`QWidget` tree:

* leaf widget kinds (``button`` / ``label`` / …) map to ordinary Qt
  controls;
* layout-widget kinds (``layout_absolute`` / ``layout_flex`` /
  ``layout_grid``) build a container and recurse into ``Layout.children``.

This is intentionally a pixel-rough approximation, not a faithful LVGL
reproduction — the sim exists to make UI iteration possible without
hardware, not to preview exact device rendering. See Stage 30 in
``docs/design.md``.

Click handling: every leaf widget that the firmware can generate
``ActionHost`` / ``ActionSwitchScreen`` / ``ActionMacro`` events for is
wired to call the supplied ``on_click(widget_proto)`` when activated.
The actual action dispatch lives in :mod:`touchy_pad.sim.window` so
this module stays pure-rendering.
"""

from __future__ import annotations

import logging
from collections.abc import Callable

from PySide6 import QtCore, QtGui, QtWidgets

from .. import _proto
from .fs import SimFs

_log = logging.getLogger("touchy_pad.sim")

#: Callback fired when a leaf widget is activated.
#:
#: Signature: ``on_event(widget_proto, event_kind, state)`` where:
#:
#: * ``event_kind`` is ``"click"`` for buttons / image buttons or
#:   ``"change"`` for sliders / checkboxes / toggles;
#: * ``state`` is an empty dict for clicks, ``{"value": int}`` for
#:   sliders, or ``{"checked": bool}`` for checkboxes / toggles.
EventHandler = Callable[["_proto.Widget", str, dict], None]


# ---------------------------------------------------------------------------
# Public entry point
# ---------------------------------------------------------------------------


def build_widget(
    w: _proto.Widget,
    fs: SimFs,
    on_event: EventHandler | None = None,
    widget_ref_overrides: dict[str, str] | None = None,
) -> QtWidgets.QWidget:
    """Recursively turn a proto Widget into a Qt widget tree.

    Parameters
    ----------
    w:
        The widget proto to render.
    fs:
        Pseudo-fs used to resolve :class:`Image` ``asset`` paths and
        :class:`WidgetRef` ``path`` indirections.
    on_event:
        Optional callback invoked when a leaf widget is activated.
        See :data:`EventHandler` for the signature.
    widget_ref_overrides:
        Optional ``{outer_widget_id: path}`` map applied to
        :class:`WidgetRef` widgets whose outer :attr:`Widget.id`
        matches a key — used by Stage-57 ``ActionChangeWidgetRef`` to
        rebind refs in RAM without rewriting the screen file.
    """
    kind = w.WhichOneof("kind")
    if kind in ("layout_absolute", "layout_flex", "layout_grid"):
        return _build_layout(w, kind, fs, on_event, widget_ref_overrides)
    if kind == "widget_ref":
        return _build_widget_ref(w, fs, on_event, widget_ref_overrides)
    return _build_leaf(w, kind, fs, on_event)


# ---------------------------------------------------------------------------
# Layout containers
# ---------------------------------------------------------------------------


def _build_layout(
    w: _proto.Widget,
    kind: str,
    fs: SimFs,
    on_event: EventHandler | None,
    overrides: dict[str, str] | None = None,
) -> QtWidgets.QWidget:
    container = QtWidgets.QWidget()
    container.setObjectName(f"layout_{w.id or kind}")

    if kind == "layout_grid":
        g = w.layout_grid
        lay = QtWidgets.QGridLayout(container)
        lay.setSpacing(int(g.gap))
        lay.setContentsMargins(0, 0, 0, 0)
        # Equal-weight rows/cols so all cells get a fair share, matching
        # LVGL's LV_GRID_FR(1) default.
        for c in range(max(1, int(g.cols))):
            lay.setColumnStretch(c, 1)
        for r in range(max(1, int(g.rows) or 1)):
            lay.setRowStretch(r, 1)
        for child in g.layout.children:
            cw = build_widget(child, fs, on_event, overrides)
            cell = child.cell if child.WhichOneof("placement") == "cell" else _proto.GridCell()
            col_span = int(cell.col_span) if cell.HasField("col_span") else 1
            row_span = int(cell.row_span) if cell.HasField("row_span") else 1
            lay.addWidget(cw, int(cell.row), int(cell.col), row_span, col_span)
        return container

    if kind == "layout_flex":
        f = w.layout_flex
        # ROW family → horizontal; COLUMN family → vertical. Everything
        # else (wrap / reverse) collapses onto the same primary axis;
        # we don't honour wrap or reverse in the sim.
        is_column = f.flow in (
            _proto.LayoutFlex.COLUMN,
            _proto.LayoutFlex.COLUMN_WRAP,
            _proto.LayoutFlex.COLUMN_REVERSE,
            _proto.LayoutFlex.COLUMN_WRAP_REVERSE,
        )
        lay = QtWidgets.QVBoxLayout(container) if is_column else QtWidgets.QHBoxLayout(container)
        lay.setSpacing(int(f.gap))
        lay.setContentsMargins(0, 0, 0, 0)
        for child in f.layout.children:
            cw = build_widget(child, fs, on_event, overrides)
            lay.addWidget(cw)
        return container

    # layout_absolute: no Qt layout — children are placed via setGeometry
    # from their Rect placements. The container itself reports a size
    # hint that bounds every child so parent layouts have something to
    # work with.
    abs_layout = w.layout_absolute
    max_x = max_y = 0
    for child in abs_layout.layout.children:
        cw = build_widget(child, fs, on_event, overrides)
        cw.setParent(container)
        r = child.rect if child.WhichOneof("placement") == "rect" else _proto.Rect()
        sh = cw.sizeHint()
        cw_w = int(r.w) if r.w else max(sh.width(), 1)
        cw_h = int(r.h) if r.h else max(sh.height(), 1)
        cw.setGeometry(int(r.x), int(r.y), cw_w, cw_h)
        cw.show()
        max_x = max(max_x, int(r.x) + cw_w)
        max_y = max(max_y, int(r.y) + cw_h)
    container.setMinimumSize(max_x, max_y)
    return container


# ---------------------------------------------------------------------------
# Leaf widgets
# ---------------------------------------------------------------------------


def _build_leaf(
    w: _proto.Widget,
    kind: str | None,
    fs: SimFs,
    on_event: EventHandler | None,
) -> QtWidgets.QWidget:
    if kind == "button":
        btn = QtWidgets.QPushButton(w.button.text or "")
        _wire_click(btn, w, on_event)
        return btn

    if kind == "label":
        lbl = QtWidgets.QLabel(w.label.text or "")
        lbl.setAlignment(_qt_align(w.label.text_align))
        if w.label.font_size:
            f = lbl.font()
            f.setPointSize(int(w.label.font_size))
            lbl.setFont(f)
        return lbl

    if kind == "slider":
        s = w.slider
        sl = QtWidgets.QSlider(QtCore.Qt.Horizontal)
        sl.setMinimum(int(s.min))
        sl.setMaximum(int(s.max) if s.max > s.min else int(s.min) + 1)
        sl.setValue(int(s.value))
        if on_event is not None:
            sl.valueChanged.connect(lambda v, _w=w: on_event(_w, "change", {"value": int(v)}))
        return sl

    if kind == "checkbox":
        cb = QtWidgets.QCheckBox(w.checkbox.text or "")
        cb.setChecked(bool(w.checkbox.checked))
        if on_event is not None:
            cb.toggled.connect(
                lambda checked, _w=w: on_event(_w, "change", {"checked": bool(checked)})
            )
        return cb

    if kind == "toggle":
        cb = QtWidgets.QCheckBox("")
        cb.setChecked(bool(w.toggle.on))
        if on_event is not None:
            cb.toggled.connect(
                lambda checked, _w=w: on_event(_w, "change", {"checked": bool(checked)})
            )
        return cb

    if kind == "image":
        return _build_image(w.image, fs)

    if kind == "image_button":
        btn = QtWidgets.QPushButton()
        pix = _load_pixmap(w.image_button.released.path, fs)
        if pix is not None:
            btn.setIcon(QtGui.QIcon(pix))
            btn.setIconSize(pix.size())
        else:
            btn.setText(w.image_button.released.path or "(img)")
        _wire_click(btn, w, on_event)
        return btn

    if kind == "arc":
        # Approximate as a vertical progress bar — close enough for layout
        # validation without dragging in a custom paintEvent.
        a = w.arc
        bar = QtWidgets.QProgressBar()
        bar.setMinimum(int(a.min))
        bar.setMaximum(int(a.max) if a.max > a.min else int(a.min) + 1)
        bar.setValue(int(a.value))
        return bar

    if kind == "trackpad":
        # Per Stage 30 step 4: render a placeholder grey box; the sim
        # does not synthesise USB HID from sim-window touches.
        frame = QtWidgets.QLabel("Sim Trackpad")
        frame.setAlignment(QtCore.Qt.AlignCenter)
        frame.setStyleSheet("QLabel { background: #555; color: #ddd; border: 1px solid #777; }")
        return frame

    if kind == "fps":
        # Static placeholder — real FPS counter would need a render loop.
        lbl = QtWidgets.QLabel("60 fps")
        lbl.setAlignment(QtCore.Qt.AlignCenter)
        lbl.setStyleSheet("QLabel { color: #8c8; }")
        return lbl

    if kind == "log":
        lbl = QtWidgets.QLabel("(log)")
        lbl.setObjectName("log_line")
        lbl.setAlignment(QtCore.Qt.AlignLeft | QtCore.Qt.AlignVCenter)
        lbl.setStyleSheet(
            "QLabel { background: #222; color: #ccc; padding: 4px; border: 1px solid #444; }"
        )
        lbl.setWordWrap(True)
        return lbl

    if kind == "force_render":
        cb = QtWidgets.QCheckBox("Force")
        return cb

    if kind == "spacer":
        sp = QtWidgets.QWidget()
        sp.setSizePolicy(QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Expanding)
        return sp

    _log.info("sim: unrenderable widget kind %r — falling back to placeholder", kind)
    return QtWidgets.QLabel(f"?{kind}?")


def _build_widget_ref(
    w: _proto.Widget,
    fs: SimFs,
    on_event: EventHandler | None,
    overrides: dict[str, str] | None,
) -> QtWidgets.QWidget:
    """Expand a Stage-54 ``WidgetRef`` inline.

    Resolves the path through :paramref:`overrides` (Stage 57 RAM
    rebinding by outer ``Widget.id``) then falls back to the path
    encoded in the proto. Empty / missing files render as a placeholder
    label so authoring mistakes don't crash the sim.
    """
    declared = w.widget_ref.path
    path = (overrides or {}).get(w.id, declared) if w.id else declared
    if not path:
        return QtWidgets.QLabel("(empty widget_ref)")
    try:
        blob = fs.read(path)
    except (FileNotFoundError, ValueError):
        _log.warning("sim: widget_ref %r — file not found", path)
        return QtWidgets.QLabel(f"(missing: {path})")
    inner = _proto.Widget()
    inner.ParseFromString(blob)
    return build_widget(inner, fs, on_event, overrides)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _qt_align(text_align: int) -> QtCore.Qt.AlignmentFlag:
    if text_align == _proto.TEXT_ALIGN_CENTER:
        return QtCore.Qt.AlignHCenter | QtCore.Qt.AlignVCenter
    if text_align == _proto.TEXT_ALIGN_RIGHT:
        return QtCore.Qt.AlignRight | QtCore.Qt.AlignVCenter
    return QtCore.Qt.AlignLeft | QtCore.Qt.AlignVCenter


def _wire_click(
    btn: QtWidgets.QAbstractButton,
    widget_proto: _proto.Widget,
    on_event: EventHandler | None,
) -> None:
    """Wire press/release/click signals on a Qt button to ``on_event``.

    Mirrors the firmware's three-edge dispatch (PRESSED, RELEASED, plus
    CLICKED for the full press+release lifecycle). The ``on_press`` /
    ``on_release`` action slots in the widget protobuf are only seen by
    listeners if we forward those signals; doing it unconditionally
    keeps the sim faithful regardless of which slots the screen
    actually populated.
    """
    if on_event is None:
        return
    btn.pressed.connect(lambda _w=widget_proto: on_event(_w, "press", {}))
    btn.released.connect(lambda _w=widget_proto: on_event(_w, "release", {}))
    btn.clicked.connect(lambda *_args, _w=widget_proto: on_event(_w, "click", {}))


def _build_image(img: _proto.Image, fs: SimFs) -> QtWidgets.QWidget:
    lbl = QtWidgets.QLabel()
    lbl.setAlignment(QtCore.Qt.AlignCenter)
    pix = _load_pixmap(img.path, fs)
    if pix is None:
        lbl.setText(f"(missing: {img.path})")
        lbl.setStyleSheet("QLabel { color: #aaa; font-style: italic; }")
        return lbl
    if img.HasField("scale") and img.scale and img.scale != 256:
        sf = float(img.scale) / 256.0
        pix = pix.scaled(
            int(pix.width() * sf),
            int(pix.height() * sf),
            QtCore.Qt.KeepAspectRatio,
            QtCore.Qt.SmoothTransformation,
        )
    if img.HasField("rotation") and img.rotation:
        transform = QtGui.QTransform().rotate(img.rotation / 10.0)
        pix = pix.transformed(transform, QtCore.Qt.SmoothTransformation)
    lbl.setPixmap(pix)
    return lbl


def _load_pixmap(asset: str, fs: SimFs) -> QtGui.QPixmap | None:
    if not asset:
        return None
    candidates = [asset]
    # The host DSL pre-rewrites image_button assets to ``.bin`` at
    # authoring time (see ``api/screens.py::image_button``) because the
    # firmware only ships LVGL's native ``.bin`` decoder. The sim
    # transport skips that conversion (needs_image_conversion = False),
    # so the actual file on disk is the original ``.png`` / ``.jpg`` /
    # etc. — fall through and try the common source extensions when the
    # exact path is missing.
    if asset.lower().endswith(".bin"):
        stem = asset[: -len(".bin")]
        candidates.extend(stem + ext for ext in (".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp"))
    for path in candidates:
        try:
            data = fs.read(path)
        except (OSError, ValueError):
            continue
        pix = QtGui.QPixmap()
        if pix.loadFromData(data):
            return pix
    return None


__all__ = ["build_widget", "EventHandler"]

"""Stage LB17 — the sim's inline-recolour → Qt rich-text helper.

PySide6 is an optional dependency (``pip install 'touchy-pad[sim]'``), so the
whole module is skipped when it isn't available.
"""

from __future__ import annotations

import os

import pytest

pytest.importorskip("PySide6")

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from PySide6 import QtCore, QtWidgets  # noqa: E402

from touchy_pad.sim.widgets import _recolor_to_html  # noqa: E402


def test_plain_text_is_escaped():
    assert _recolor_to_html("a & b < c") == "a &amp; b &lt; c"


def test_a_coloured_run_becomes_a_span():
    assert _recolor_to_html("#FF0000 Danger!# watch out") == (
        '<span style="color:#FF0000">Danger!</span> watch out'
    )


def test_several_runs():
    assert _recolor_to_html("#FF0000 one# #00FF00 two#") == (
        '<span style="color:#FF0000">one</span> <span style="color:#00FF00">two</span>'
    )


def test_an_unmatched_hash_is_kept_literally():
    assert _recolor_to_html("#nope") == "#nope"


# ---- stage lb24: the same parser, and span-group rich text -------------------


def test_recolor_to_html_uses_the_shared_markup_parser():
    """The sim's label preview must agree with the device's parser."""
    from touchy_pad.rich_text import parse_recolor_markup
    from touchy_pad.sim.widgets import spans_to_html

    for text in (
        "plain",
        "#FF0000 one#",
        "#A020F0 cocks head#I bet he's in a cave",
        "#FF0000 one# #00FF00 two#",
        "#nope",
        "#FF0000 #",
    ):
        assert _recolor_to_html(text) == spans_to_html(parse_recolor_markup(text))
    assert _recolor_to_html("#A020F0 cocks head#I bet") == (
        '<span style="color:#A020F0">cocks head</span>I bet'
    )


def test_spans_to_html_carries_font_size():
    from touchy_pad import _proto
    from touchy_pad.sim.widgets import spans_to_html

    html = spans_to_html(
        [
            _proto.Span(text="big", text_color=0xFF0000, font_size=30),
            _proto.Span(text=" plain"),
        ]
    )
    assert html == '<span style="color:#FF0000; font-size:30pt">big</span> plain'


def test_span_group_widget_renders_rich_text():
    """A `span_group` leaf renders its runs as Qt rich text."""
    import pathlib
    import tempfile

    from touchy_pad import _proto
    from touchy_pad.sim.fs import SimFs
    from touchy_pad.sim.widgets import build_widget

    app = QtWidgets.QApplication.instance() or QtWidgets.QApplication([])
    with tempfile.TemporaryDirectory() as d:
        fs = SimFs(pathlib.Path(d), "sim")
        w = _proto.Widget(id="q")
        w.span_group.spans.add(text="one", text_color=0xFF0000)
        w.span_group.spans.add(text=" two")
        qw = build_widget(w, fs)
        assert qw.textFormat() == QtCore.Qt.RichText
        assert qw.text() == '<span style="color:#FF0000">one</span> two'
        assert qw.wordWrap() is True
    assert app is not None

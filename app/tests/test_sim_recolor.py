"""Stage LB17 — the sim's inline-recolour → Qt rich-text helper.

PySide6 is an optional dependency (``pip install 'touchy-pad[sim]'``), so the
whole module is skipped when it isn't available.
"""

from __future__ import annotations

import os

import pytest

pytest.importorskip("PySide6")

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

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

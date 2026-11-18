"""Stage lb24 — the ``#RRGGBB text#`` markup parser.

``touchy_pad.rich_text.parse_recolor_markup`` is the Python mirror of the
firmware's parser (``firmware/main/widgets/widget_span.cpp``), which is what the
device uses to render a ``recolor`` label *and* to apply a runtime ``text``
override to any span group. The table below is the shared specification: the C++
implementation is written to produce the same runs, so a change here must be
mirrored there (and vice versa).
"""

from __future__ import annotations

import pytest

from touchy_pad.rich_text import parse_recolor_markup


def _runs(text: str) -> list[tuple[str, int | None]]:
    return [
        (s.text, s.text_color if s.HasField("text_color") else None)
        for s in parse_recolor_markup(text)
    ]


def test_plain_text_is_one_uncoloured_run():
    assert _runs("hello") == [("hello", None)]


def test_a_single_run():
    assert _runs("#FF0000 one#") == [("one", 0xFF0000)]


def test_text_after_a_run_inherits():
    assert _runs("#FF0000 Danger!# watch out") == [
        ("Danger!", 0xFF0000),
        (" watch out", None),
    ]


def test_two_runs_keep_the_text_between_them():
    assert _runs("#FF0000 one# #00FF00 two#") == [
        ("one", 0xFF0000),
        (" ", None),
        ("two", 0x00FF00),
    ]


def test_voicebot_quip_closing_hash_hugs_the_next_word():
    """The reported case: a closing ``#`` immediately followed by text."""
    assert _runs("#A020F0 cocks head#I bet he's in a cave") == [
        ("cocks head", 0xA020F0),
        ("I bet he's in a cave", None),
    ]


def test_voicebot_full_string_segments_are_exact():
    text = (
        '#FF0000 peeks from behind curtains# Oh, Mike\'s been "hiding" somewhere, '
        "avoiding his problems, as usual. #A020F0 cocks head#I bet he's in a cave, "
        "living off grubs and studying the stars."
    )
    runs = _runs(text)
    assert runs[0] == ("peeks from behind curtains", 0xFF0000)
    assert runs[1][0].startswith(" Oh, Mike's")
    assert runs[2] == ("cocks head", 0xA020F0)
    assert runs[3][0].startswith("I bet he's")
    # The delimiters are consumed (each opener is ``#RRGGBB `` = 8 chars, each
    # closer is ``#``) but no visible character is lost or moved: the runs
    # concatenate back to the original string minus exactly those four markers.
    expected = (
        text.replace("#FF0000 ", "", 1)
        .replace("curtains#", "curtains", 1)
        .replace("#A020F0 ", "", 1)
        .replace("head#", "head", 1)
    )
    assert "".join(r[0] for r in runs) == expected


def test_an_unmatched_hash_is_literal():
    assert _runs("#nope") == [("#nope", None)]
    assert _runs("trailing #") == [("trailing #", None)]
    assert _runs("costs $5 #yolo") == [("costs $5 #yolo", None)]


def test_a_hex_run_without_the_trailing_space_is_literal():
    assert _runs("#FF0000no-space") == [("#FF0000no-space", None)]


def test_an_empty_run_contributes_nothing():
    assert _runs("#FF0000 #") == []
    assert _runs("#FF0000 #after") == [("after", None)]


def test_an_unterminated_run_keeps_its_colour():
    assert _runs("#FF0000 to the end") == [("to the end", 0xFF0000)]


def test_a_hash_inside_a_run_closes_it():
    # The `#` that follows the run closes it; the *next* `#RRGGBB ` would need
    # its own `#`, which this string does not have. Matches LVGL's parser.
    assert _runs("#FF0000 a #00FF00 b") == [
        ("a ", 0xFF0000),
        ("00FF00 b", None),
    ]


def test_lowercase_hex_is_accepted():
    assert _runs("#00ff00 live#") == [("live", 0x00FF00)]


def test_a_seven_digit_run_is_not_an_opener():
    # Seven hex digits then a space: the opener test needs *exactly* six.
    s = _runs("#FF00000 x#")
    assert s == [("#FF00000 x#", None)]


@pytest.mark.parametrize("text", ["", "#", "##", "####"])
def test_degenerate_inputs_never_raise(text):
    parse_recolor_markup(text)


def test_spans_are_plain_proto_messages():
    """The parser hands back ``touchy.Span`` messages, ready to append."""
    from touchy_pad import _proto

    spans = parse_recolor_markup("#FF0000 one# two")
    assert all(isinstance(s, _proto.Span) for s in spans)
    group = _proto.SpanGroup()
    group.spans.extend(spans)
    assert len(group.spans) == 2
    assert group.spans[1].HasField("text_color") is False

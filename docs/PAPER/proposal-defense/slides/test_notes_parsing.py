"""Regression checks for percentage-bearing scientific presenter notes."""
import unittest

from add_speaker_notes_to_pptx import _plain_text_from_latex


class NotesParsingTests(unittest.TestCase):
    def test_escaped_percentages_are_content(self):
        self.assertEqual(_plain_text_from_latex(r"54.57\% versus 53.80\%"),
                         "54.57% versus 53.80%")

    def test_unescaped_percent_starts_a_comment(self):
        self.assertEqual(_plain_text_from_latex("value % comment\nnext"),
                         "value \nnext")


if __name__ == "__main__":
    unittest.main()

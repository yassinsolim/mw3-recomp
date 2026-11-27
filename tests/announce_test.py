"""The release announcement for Discord (tools/release/announce.py): the message from notes, the dry run, the refusals."""
import contextlib
import importlib.util
import io
import os
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("announce", ROOT / "tools/release/announce.py")
announce = importlib.util.module_from_spec(spec)
spec.loader.exec_module(announce)

WEBHOOK = "https://discord.com/api/webhooks/123456789/not-a-real-token"


def release(tag="v0.1.10", **extra):
    return {"tag_name": tag, "name": f"Modern Warfare 3 for Android {tag}", "draft": False, "prerelease": False,
            "html_url": f"https://github.com/yassinsolim/mw3-recomp/releases/tag/{tag}",
            "body": (ROOT / "docs/releases" / f"mw3-{tag}.txt").read_text(encoding="utf-8"), **extra}


class AnnounceTest(unittest.TestCase):
    def test_message_of_a_real_release(self):
        item = release()
        text = announce.message(announce.game_title(item), item["tag_name"], item["body"], item["html_url"])
        lines = text.splitlines()
        self.assertEqual(lines[0], "**Modern Warfare 3 v0.1.10** is out.")
        self.assertTrue(lines[1].startswith("Nothing to rebuild"))
        self.assertEqual(sum(line.startswith("- ") for line in lines), 5)  # six bullets in New: five are kept
        self.assertIn(f"Get it: {item['html_url']}", lines)
        self.assertIn(announce.DISCORD, text)
        self.assertNotIn("relay server", text)  # Known issues are left out
        self.assertNotIn("Official downloads are only", text)
        self.assertLess(len(text), 2000)

    def test_three_headlines_are_enough(self):
        notes = "Game for Android v1\n\nNew\n- One.\n- Two.\n- Three.\n\nKnown issues\n- Bad.\n"
        self.assertEqual(announce.headlines(notes), ["One.", "Two.", "Three."])

    def test_a_long_bullet_is_cut_at_a_sentence(self):
        sentence = "It is fixed. " + "word " * 80
        short = announce.shortened(sentence)
        self.assertLessEqual(len(short), announce.HEADLINE_LIMIT + 4)
        self.assertTrue(short.endswith("..."))

    def test_a_long_bullet_ends_at_a_whole_sentence_when_one_fits(self):
        first = "The first sentence is long enough to be most of the cut, " + "and goes on " * 10 + "to its end."
        short = announce.shortened(first + " " + "More follows. " * 30)
        self.assertTrue(short.startswith(first))
        self.assertTrue(short.endswith("follows."))
        self.assertLessEqual(len(short), announce.HEADLINE_LIMIT)

    def test_notes_without_bullets_still_make_a_message(self):
        text = announce.message("Game", "v1.0.0", "Game for Android v1.0.0\n\nA small release.", "https://example.org/r")
        self.assertNotIn("What's new", text)
        self.assertIn("A small release.", text)

    def test_dry_run_prints_and_sends_nothing(self):
        out = io.StringIO()
        with mock.patch.object(announce, "github", return_value=release()), \
                mock.patch.object(announce, "send") as send, \
                mock.patch.dict(os.environ, {announce.SECRET: ""}), contextlib.redirect_stdout(out):
            announce.announce("yassinsolim/mw3-recomp", "v0.1.10", dry_run=True)
        send.assert_not_called()
        self.assertIn("dry run", out.getvalue())
        self.assertIn("**Modern Warfare 3 v0.1.10** is out.", out.getvalue())

    def test_sending_posts_the_message_without_mentions(self):
        with mock.patch.object(announce, "github", return_value=release()), \
                mock.patch.object(announce, "send") as send, \
                mock.patch.dict(os.environ, {announce.SECRET: WEBHOOK}), contextlib.redirect_stdout(io.StringIO()) as out:
            announce.announce("yassinsolim/mw3-recomp", "v0.1.10", dry_run=False)
        address, text = send.call_args.args
        self.assertEqual(address, WEBHOOK)
        self.assertIn("is out.", text)
        self.assertNotIn(WEBHOOK, out.getvalue())

    def test_drafts_and_pre_releases_are_refused(self):
        for key in ("draft", "prerelease"):
            with mock.patch.object(announce, "github", return_value=release(**{key: True})), \
                    mock.patch.object(announce, "send") as send:
                with self.assertRaises(announce.Refused):
                    announce.announce("yassinsolim/mw3-recomp", "v0.1.10", dry_run=False)
            send.assert_not_called()

    def test_no_webhook_is_refused_and_a_wrong_one_too(self):
        with mock.patch.dict(os.environ, {announce.SECRET: ""}):
            with self.assertRaises(announce.Refused):
                announce.webhook(None)
        with mock.patch.dict(os.environ, {announce.SECRET: "https://example.org/hook"}):
            with self.assertRaises(announce.Refused):
                announce.webhook(None)


if __name__ == "__main__":
    unittest.main()

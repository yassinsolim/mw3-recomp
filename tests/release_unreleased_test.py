"""A game that is not released stays out of the mirrors (tools/release/release.py: public_text, public_message,
filter_private, unreleased_problems; docs/android-releases.md, Games that are not released).

The marks are tried on made-up text. Then, where tools/release/unreleased.json names such a game (no mirror has
the file, so there this part is passed over), the checkout's HEAD is filtered as `sync` filters the private
branch, into a repository made for it, and the whole filtered history is looked through: no path of the game,
no commit message and no text file that names it, no mark left. No GitHub is reached."""
import importlib.util
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools/release"))
spec = importlib.util.spec_from_file_location("release", ROOT / "tools/release/release.py")
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)

AT = b"@"  # the marks are put together here, so that this file has none


def marked(text):
    return text.replace(b"{begin}", AT + b"private-begin").replace(b"{end}", AT + b"private-end").replace(
        b"{line}", AT + b"private-line").replace(b"{public}", AT + b"public:")


class MarksTest(unittest.TestCase):
    def test_a_region_is_taken_out_with_its_marks(self):
        text = marked(b"one\n    // {begin}\n    hidden();\n    // {end}\ntwo\n")
        self.assertEqual(release.public_text(text), b"one\ntwo\n")

    def test_every_comment_style(self):
        for begin, end in ((b"# {begin}", b"# {end}"), (b"<!-- {begin} -->", b"<!-- {end} -->"), (b"//{begin}", b"//{end}")):
            text = marked(b"a\n" + begin + b"\nx\n" + end + b"\nb\n")
            self.assertEqual(release.public_text(text), b"a\nb\n", begin)

    def test_a_marked_line_is_taken_out(self):
        text = marked(b"keep\nhidden = 1  # {line}\nvalue = 2 // {line}\r\nkeep too\n")
        self.assertEqual(release.public_text(text), b"keep\nkeep too\n")

    def test_the_public_line_stands_in_place_with_its_own_indentation_and_line_end(self):
        text = marked(b"if (a) {\n    // {begin}\n    b || c\n    // {end}\n    // {public}     b\r\n# {public} \n}\n")
        self.assertEqual(release.public_text(text), b"if (a) {\n    b\r\n\n}\n")

    def test_a_mark_inside_a_sentence_is_text(self):
        text = marked(b"# Lines between {begin} and {end} are private; {public} is not at the start here.\n")
        self.assertEqual(release.public_text(text), text)

    def test_marks_out_of_place_are_refused(self):
        for text in (b"// {begin}\nx\n", b"// {end}\n", b"// {begin}\n// {begin}\n// {end}\n// {end}\n"):
            with self.assertRaises(release.Refused):
                release.public_text(marked(text))

    def test_a_binary_file_and_a_file_without_marks_are_left(self):
        binary = marked(b"\0\1// {begin}\n")
        self.assertIs(release.public_text(binary), binary)
        self.assertEqual(release.public_text(b"plain\ntext"), b"plain\ntext")

    def test_the_release_tool_and_its_notes_have_no_mark_of_their_own(self):
        for name in ("tools/release/release.py", "docs/android-releases.md", "tests/release_unreleased_test.py"):
            data = (ROOT / name).read_bytes()
            self.assertEqual(release.public_text(data), data, name)


class MessageTest(unittest.TestCase):
    MENTION = "(?i)secret game"

    def test_a_message_that_names_no_such_game_is_left(self):
        self.assertEqual(release.public_message(b"Fix the menu\n\n- one\n", self.MENTION), b"Fix the menu\n\n- one\n")
        self.assertEqual(release.public_message(b"Secret Game\n", None), b"Secret Game\n")

    def test_one_that_does_is_replaced_by_its_public_part(self):
        message = b"secret game: bots\n\n- details of the Secret Game\n\nPublic: The timer fix\n\n- for every title\n"
        self.assertEqual(release.public_message(message, self.MENTION), b"The timer fix\n\n- for every title\n")

    def test_or_by_a_line_that_says_so(self):
        self.assertEqual(release.public_message(b"Secret Game runs\n", self.MENTION), release.NOT_RELEASED)

    def test_a_public_part_that_names_the_game_is_refused(self):
        with self.assertRaises(release.Refused):
            release.public_message(b"x\n\nPublic: the Secret Game's timer\n", self.MENTION)


class FilteredHistoryTest(unittest.TestCase):
    def test_a_dry_run_sync_of_this_checkout_has_nothing_of_a_game_that_is_not_released(self):
        if not (ROOT / release.UNRELEASED).exists():
            self.skipTest("no tools/release/unreleased.json: a mirror, or no game is held back")
        if subprocess.run(["git", "filter-repo", "--version"], capture_output=True).returncode:
            self.skipTest("git filter-repo is not installed")
        hidden = json.loads((ROOT / release.UNRELEASED).read_text())
        folder = tempfile.mkdtemp(prefix="unreleased-")
        self.addCleanup(shutil.rmtree, folder, ignore_errors=True)
        release.run("git", "init", "-q", folder)
        head = release.run("git", "rev-parse", "HEAD", cwd=ROOT)
        self.assertEqual(release.filter_private(folder, head, ROOT), hidden)

        commits = int(release.run("git", "rev-list", "--count", "refilter", cwd=folder))
        files = release.run("git", "ls-tree", "-r", "--name-only", "refilter", cwd=folder).splitlines()
        self.assertGreater(commits, 900)
        self.assertGreater(len(files), 1000)
        self.assertNotIn(release.UNRELEASED, files)
        for path in hidden["paths"]:
            self.assertFalse([name for name in files if name == path or name.startswith(path.rstrip("/") + "/")], path)
        self.assertEqual(release.unreleased_problems(folder, hidden), [])
        # The files that had marks are there still, as text that passes the project's own check of its tree.
        self.assertIn("runtime/title.h", files)
        print(f"filtered {head[:9]}: {commits} commits, {len(files)} files, nothing of {len(hidden['paths'])} paths "
              f"and no mention", file=sys.stderr)

    def test_the_check_finds_what_a_filter_would_leave(self):
        """unreleased_problems on a small history of its own: a name, a message, a text and a mark."""
        folder = tempfile.mkdtemp(prefix="unreleased-")
        self.addCleanup(shutil.rmtree, folder, ignore_errors=True)
        git = lambda *args: release.run("git", "-c", "user.name=t", "-c", "user.email=t@t", *args, cwd=folder)  # noqa: E731
        git("init", "-q", "-b", "refilter")
        Path(folder, "a.txt").write_text("fine\n")
        git("add", "."), git("commit", "-q", "-m", "first")
        hidden = {"mention": "(?i)secret game", "published": ["old.txt"]}
        self.assertEqual(release.unreleased_problems(folder, hidden), [])
        first = git("rev-parse", "HEAD")
        Path(folder, "old.txt").write_text("the Secret Game, said before\n")
        Path(folder, "b.txt").write_text("one\nthe secret game\n")
        Path(folder, "c.txt").write_bytes(marked(b"# {begin}\n# {end}\n"))
        Path(folder, "secret game.txt").write_text("x\n")
        git("add", "."), git("commit", "-q", "-m", "Secret Game")
        bad = release.unreleased_problems(folder, hidden, first)
        self.assertEqual(len(bad), 4, bad)
        self.assertTrue(any("its message" in line for line in bad), bad)
        self.assertTrue(any(line.startswith("secret game.txt: its name") for line in bad), bad)
        self.assertTrue(any(line.startswith("b.txt:2") for line in bad), bad)
        self.assertTrue(any(line.startswith("c.txt") and "mark" in line for line in bad), bad)
        self.assertEqual(release.unreleased_problems(folder, {}), [])


if __name__ == "__main__":
    unittest.main()

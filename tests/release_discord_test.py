"""The release's 'fixed in' replies (tools/release/release.py, tell_discord): the evidence handed to the Discord bot,
the dry run, and a bot that hangs, is missing or fails. No bot, no GitHub and no Discord are reached."""
import contextlib
import importlib.util
import io
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools/release"))
spec = importlib.util.spec_from_file_location("release", ROOT / "tools/release/release.py")
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)

COMMITS = {"mw3": "a" * 40, "aw": "b" * 40}


class TellDiscordTest(unittest.TestCase):
    def setUp(self):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        self.work = Path(folder.name)
        bot = self.work / "botctl.cmd"
        bot.write_text("")
        patch = mock.patch.object(release, "BOTCTL", bot)
        patch.start()
        self.addCleanup(patch.stop)
        self.release = release.Release.__new__(release.Release)
        self.release.work = self.work
        self.release.games = ["mw3", "aw"]
        self.release.state = {"tags": {"mw3": "v0.1.12", "aw": "v0.1.5"}}
        self.calls = []
        self.bot = lambda game, command: (0, f"{game}: 0 thread(s) told")

    def bounded(self, command, seconds, cwd=None):
        """Stands in for the two commands: git in the mirror's clone, then the bot."""
        command = [str(part) for part in command]
        if command[0] == "git":
            game = next(game for game in COMMITS if Path(cwd) == self.work / release.GAMES[game][0])
            self.assertEqual(command[-1], f"refs/tags/{self.release.state['tags'][game]}^{{commit}}")
            return 0, COMMITS[game]
        self.calls.append((command, seconds))
        return self.bot(command[command.index("--name") + 1], command)

    def tell(self, **arguments):
        out = io.StringIO()
        with mock.patch.object(release, "bounded", self.bounded), contextlib.redirect_stdout(out):
            self.release.tell_discord(**arguments)
        return out.getvalue()

    def test_the_bot_gets_the_repository_the_tag_and_the_tag_s_commit(self):
        out = self.tell()
        self.assertEqual([call[0][1:] for call in self.calls], [
            ["fixed-in", "--repo", "yassinsolim/mw3-recomp", "--name", "MW3", "--version", "v0.1.12", "--commit", "a" * 40],
            ["fixed-in", "--repo", "yassinsolim/aw-recomp", "--name", "AW", "--version", "v0.1.5", "--commit", "b" * 40]])
        self.assertTrue(all(seconds == release.BOT_SECONDS and 0 < seconds < 3600 for _, seconds in self.calls))
        self.assertIn("discord: MW3: 0 thread(s) told", out)
        self.assertNotIn("WARNING", out)

    def test_a_dry_run_is_passed_on_to_the_bot(self):
        self.tell(dry_run=True)
        self.assertTrue(self.calls and all(call[0][-1] == "--dry-run" for call in self.calls))

    def test_a_bot_that_hangs_is_a_warning_and_the_next_game_is_still_told(self):
        def bot(game, command):
            if game == "MW3":
                raise subprocess.TimeoutExpired(command, release.BOT_SECONDS)
            return 0, "AW: 1 thread(s) told"
        self.bot = bot
        out = self.tell()
        self.assertIn(f"discord: WARNING: mw3: the bot was stopped after {release.BOT_SECONDS} s", out)
        self.assertIn("discord: AW: 1 thread(s) told", out)
        self.assertEqual(len(self.calls), 2)

    def test_a_bot_that_does_not_start_is_a_warning(self):
        def bot(game, command):
            raise PermissionError(13, "Access is denied", "C:\\Users\\someone\\secret-path\\botctl.cmd")
        self.bot = bot
        out = self.tell()
        self.assertEqual(out.count("the bot did not start (PermissionError)"), 2)
        self.assertNotIn("secret-path", out)  # the warning names the kind of failure, nothing of the PC

    def test_a_bot_that_fails_is_a_warning_with_what_it_did_say(self):
        self.bot = lambda game, command: (1, "C0003: posted in https://discord.com/channels/1/30")
        out = self.tell()
        self.assertIn("discord: C0003: posted in", out)
        self.assertIn("discord: WARNING: mw3: the bot ended with 1", out)

    def test_no_tag_in_the_clone_no_bot(self):
        with mock.patch.dict(COMMITS, {"mw3": "", "aw": "not a commit"}):
            out = self.tell()
        self.assertEqual(self.calls, [])
        self.assertEqual(out.count("is not a tag in the mirror's clone"), 2)

    def test_a_pc_without_the_bot_skips_it(self):
        release.BOTCTL.unlink()
        self.assertIn("no bot at", self.tell())
        self.assertEqual(self.calls, [])

    def test_publish_as_a_dry_run_publishes_nothing_and_tells_the_bot_so(self):
        told = []
        self.release.gates = lambda wait: ({}, ("yassinsolim/toolchain", "t1", None))
        self.release.device_gates = lambda path, gates: None
        self.release.tell_discord = lambda dry_run=False: told.append(dry_run)
        self.release.verify_public = lambda: self.fail("a dry run has nothing public to verify")
        with mock.patch.object(release, "run", side_effect=AssertionError("a dry run runs no gh release edit")), \
                contextlib.redirect_stdout(io.StringIO()) as out:
            self.release.publish("report.json", dry_run=True)
        self.assertEqual(told, [True])
        self.assertIn("yassinsolim/mw3-recomp v0.1.12 passes the gates and is not published", out.getvalue())

    def test_a_step_without_a_dry_run_refuses_one(self):
        for step in ("tag", "sync", "withdraw"):
            with mock.patch.object(sys, "argv", ["release.py", step, "--dry-run"]), \
                    mock.patch.object(release, "Release", side_effect=AssertionError("refused before anything is done")), \
                    contextlib.redirect_stderr(io.StringIO()) as err:
                self.assertEqual(release.main(), 1)
            self.assertIn(f"REFUSED: {step} has no dry run", err.getvalue())


class BoundedTest(unittest.TestCase):
    def test_the_output_and_the_exit_code(self):
        self.assertEqual(release.bounded((sys.executable, "-c", "print('told'); raise SystemExit(3)"), 60), (3, "told"))

    def test_a_command_over_its_time_is_killed(self):
        started = time.monotonic()
        with self.assertRaises(subprocess.TimeoutExpired):
            # The child outlives its parent's kill unless the whole tree goes; neither may keep this waiting.
            release.bounded((sys.executable, "-c", "import subprocess, sys; "
                             "subprocess.run([sys.executable, '-c', 'import time; time.sleep(120)'])"), 2)
        self.assertLess(time.monotonic() - started, 60)

    def test_a_command_that_is_not_there(self):
        with self.assertRaises(OSError):
            release.bounded((str(ROOT / "tests" / "no-such-botctl.cmd"),), 60)


if __name__ == "__main__":
    unittest.main()

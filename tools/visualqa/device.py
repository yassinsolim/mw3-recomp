"""A run on a phone: the device's lock, the test app beside the owner's, the recording.

The test app is the debug build under another package name (`-PappSuffix=.vq`),
installed fresh for a round and removed after it. It plays from the disc
image in the phone's Download folder and takes the executables from the
owner's installed game, which is read and never written, or for a game the
owner's app is not read for from a folder of test files on the phone
(scenarios.TEST_FILES). Switches go in through `options.txt`, which only a
debug build reads.
"""
from contextlib import contextmanager
from datetime import datetime, timedelta
from pathlib import Path
import os
import re
import shutil
import subprocess
import threading
import time

# The longest a slot is held (device-locks/README.txt allows 30 minutes).
SLOT_SECONDS = 28 * 60
LOCK_RETRY_SECONDS = 120
# `screenrecord` stops at three minutes whatever it is asked; a segment ends
# before that, and the next starts this long before it does so that no frame
# falls between two.
SEGMENT_SECONDS = 170
SEGMENT_OVERLAP = 3
# 1080p at this rate keeps a 32 pixel block's edges and a 13 level flash
# (measured: a flash of 13.4 levels reads 13.4 at 4 Mbit/s and at 20).
BIT_RATE = 12_000_000
ACTIVITY = "com.mw2.recomp.MainActivity"
# In a test app's `dlc` folder while it has the packages of a folder of test
# files: the folder they go back to (Device.provision, Device.remove).
LENT = "lent-from.txt"


def find_adb():
    named = os.environ.get("ADB")
    if named:
        return named
    found = shutil.which("adb")
    if not found:
        raise SystemExit("adb is not on the PATH: set ADB to platform-tools/adb")
    return found


class DeviceLock:
    """One slot of a lock folder (`mkdir <name>.lock`), held for no longer than a slot at a time."""

    def __init__(self, folder, name, owner, purpose):
        self.path = Path(folder) / f"{name}.lock" if folder else None
        self.owner = owner
        self.purpose = purpose
        self.taken = None

    def reserved_for(self):
        """Who the device is kept for next (`<name>.next`, which only the parent writes), or None."""
        try:
            return self.path.with_suffix(".next").read_text(errors="replace").strip() or None
        except OSError:
            return None

    def acquire(self, say=print):
        if self.path is None:
            return
        while True:
            # A device kept for somebody else is not taken even when it is free.
            named = self.reserved_for()
            if named and named != self.owner:
                say(f"waiting for {self.path.name}: reserved next for {named}")
                time.sleep(LOCK_RETRY_SECONDS)
                continue
            try:
                # mkdir either makes the folder or fails: two agents cannot both take it.
                self.path.mkdir()
                break
            except FileExistsError:
                holder = ""
                try:
                    holder = (self.path / "owner.txt").read_text(errors="replace").strip()
                except OSError:
                    pass
                say(f"waiting for {self.path.name}: {holder or 'held'}")
                time.sleep(LOCK_RETRY_SECONDS)
        self.taken = time.monotonic()
        until = datetime.now() + timedelta(seconds=SLOT_SECONDS)
        (self.path / "owner.txt").write_text(f"{self.owner} until {until:%H:%M} - {self.purpose}\n")
        if named == self.owner:
            # The one it was kept for removes the reservation once it holds the device.
            self.path.with_suffix(".next").unlink(missing_ok=True)
        say(f"took {self.path.name} until {until:%H:%M}")

    def release(self):
        if self.path is None or self.taken is None:
            return
        shutil.rmtree(self.path, ignore_errors=True)
        self.taken = None

    def room_for(self, seconds, before_release=None, say=print):
        """Gives the slot up and queues again when the next run would overrun it."""
        if self.path is None or self.taken is None:
            return
        if time.monotonic() - self.taken + seconds > SLOT_SECONDS:
            if before_release:
                before_release()
            self.release()
            say(f"released {self.path.name} at the end of the slot; queueing again")
            # Whoever waited goes first: their retry comes within two minutes.
            time.sleep(LOCK_RETRY_SECONDS + 10)
            self.acquire(say)

    @contextmanager
    def held(self, say=print):
        self.acquire(say)
        try:
            yield self
        finally:
            self.release()


_CLOCKED = re.compile(r"^\s*(\d+\.\d+)\s.*\b(?:at|since) (\d+\.\d+) s\b")


class RunClock:
    """When, by the phone's time, the run's own clock was at nought.

    The log counts seconds from the game's start and the recording from its
    own; the phone's system log has every line of the game's with the time
    it was written. A line that names its second of the run ("in play at
    30.339 s", "queued ... at 25.00 s", every STUTTER) gives the phone's time
    of the run's nought, and the middle one of all of them is used.
    """

    def __init__(self, device):
        # From now on only: the phone's log before this is other runs'.
        self.process = subprocess.Popen(
            [device.adb_path, "-s", device.serial, "logcat", "-v", "epoch", "-T", "1", "-s", "mw2"],
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL, text=True, errors="replace")
        self.noughts = []
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def _read(self):
        for line in self.process.stdout:
            found = _CLOCKED.match(line)
            if found:
                self.noughts.append(float(found.group(1)) - float(found.group(2)))

    def close(self):
        self.process.terminate()
        self.reader.join(timeout=10)

    def nought(self):
        """The phone's time at the run's second nought; None when no line of the log said."""
        if not self.noughts:
            return None
        ordered = sorted(self.noughts)
        return ordered[len(ordered) // 2]


class Device:
    def __init__(self, serial, suffix=".vq", adb=None):
        self.serial = serial
        self.suffix = suffix
        self.adb_path = adb or find_adb()

    def adb(self, *arguments, timeout=900, check=False):
        done = subprocess.run([self.adb_path, "-s", self.serial, *arguments], capture_output=True, text=True,
                              errors="replace", timeout=timeout, stdin=subprocess.DEVNULL)
        if check and done.returncode:
            raise RuntimeError(f"adb {' '.join(arguments[:3])}: {(done.stdout + done.stderr).strip()[:400]}")
        return (done.stdout + done.stderr).strip()

    def shell(self, command, timeout=900):
        return self.adb("shell", command, timeout=timeout)

    def package(self, game):
        return f"com.{game}.recomp{self.suffix}"

    def external(self, game):
        return f"/sdcard/Android/data/{self.package(game)}/files"

    def owner_game(self, game):
        return f"/sdcard/Android/data/com.{game}.recomp/files/game"

    def has(self, path):
        return self.shell(f"[ -e '{path}' ] && echo yes").strip() == "yes"

    def disc_image(self, game, names):
        """The first of a game's disc images that is in the phone's Download folder."""
        for name in names:
            path = f"/sdcard/Download/{name}"
            if self.has(path):
                return path
        return None

    def install(self, game, apks):
        """Installs the test app and its game code, fresh."""
        self.remove(game)
        for name in (f"{game}{self.suffix}.apk", f"{game}-game{self.suffix}.apk"):
            apk = Path(apks) / name
            if not apk.is_file():
                raise RuntimeError(f"{apk} is not built (docs/visualqa.md, Building the test apps)")
            said = self.adb("install", "-r", str(apk), timeout=1200)
            if "Success" not in said:
                raise RuntimeError(f"installing {name}: {said[-300:]}")

    def provision(self, game, image, files, packages=False, more_prefs=None):
        """The executables out of `files` and the preferences of a set-up app.

        `files` is only read, but for its content packages when `packages` says it has some (Advanced Warfare's
        install disc's two): they are lent to the app, moved into the `dlc` folder beside its game folder,
        where the launcher counts the game as set up, and `remove` moves them back. A copy would be four
        gigabytes a run.
        """
        package, external = self.package(game), self.external(game)
        if not self.has(f"{files}/default.xex"):
            raise RuntimeError(f"{files} has no game")
        self.shell(f"mkdir -p {external}/game")
        self.shell(f"cd {files} && for f in default.xex default_mp.xex nxeart; do "
                   f"[ -e $f ] && cp -r $f {external}/game/; done")
        if packages:
            self.shell(f"mkdir -p {external}/dlc && echo {files} > {external}/dlc/{LENT} && "
                       f"for f in {files}/*; do case ${{f##*/}} in default*|nxeart) ;; *) mv $f {external}/dlc/;; esac; done")
        # The app reads the image where it lies, which needs all-files access.
        self.shell(f"appops set {package} MANAGE_EXTERNAL_STORAGE allow")
        values = {"gameDirectory": f"{external}/game", "discImage": image or "", "seenWelcome": True,
                  "fpsCapFollowsScreen": True, "seenPadWarning": True, "playerNameAsked": True,
                  # The help over the game at a run's start dims the picture until it is closed.
                  "seenGameHelp": True,
                  # Left false, the driver the build carries becomes the default, as on a player's phone.
                  "driverChoiceMade": False,
                  # The touch controls and the statistics line cover the picture and are not the game's.
                  "touchControls": False, "showStats": False, "autoLaunch": True}
        values.update(more_prefs or {})
        rows = []
        for name, value in values.items():
            if isinstance(value, bool):
                rows.append(f'    <boolean name="{name}" value="{str(value).lower()}" />')
            elif isinstance(value, int):
                rows.append(f'    <int name="{name}" value="{value}" />')
            elif isinstance(value, float):
                rows.append(f'    <float name="{name}" value="{value}" />')
            else:
                rows.append(f'    <string name="{name}">{value}</string>')
        xml = "<?xml version='1.0' encoding='utf-8' standalone='yes' ?>\n<map>\n" + "\n".join(rows) + "\n</map>\n"
        self._push_text(xml, "/data/local/tmp/vq_prefs.xml")
        # files/ must be there before a run request is written, or the app starts its first title.
        self.shell(f"run-as {package} sh -c 'mkdir -p shared_prefs files; "
                   f"cp /data/local/tmp/vq_prefs.xml shared_prefs/mw2.xml'")
        self.shell("rm -f /data/local/tmp/vq_prefs.xml")

    def _push_text(self, text, remote):
        local = Path(os.environ.get("TEMP", "/tmp")) / f"vq_{os.getpid()}_{Path(remote).name}"
        local.write_text(text, newline="\n")
        try:
            self.adb("push", str(local), remote, check=True)
        finally:
            local.unlink()

    def start(self, game, title, options):
        """Starts a run with these switches; returns the clock that places the phone's time in the run."""
        package, external = self.package(game), self.external(game)
        self.shell(f"am force-stop {package}")
        self._push_text("".join(f"{name}={value}\n" for name, value in options.items()), f"{external}/options.txt")
        self.shell(f"run-as {package} sh -c 'printf \"{title}\\n\\n\" > files/run_request.txt'")
        self.shell(f"rm -f {external}/game.log; run-as {package} rm -f files/game.log; rm -rf {external}/vqframes; "
                   f"mkdir -p {external}/vqframes")
        clock = RunClock(self)
        self.shell(f"input keyevent KEYCODE_WAKEUP; svc power stayon true; am start -n {package}/{ACTIVITY}")
        deadline = time.monotonic() + 40
        while time.monotonic() < deadline:
            if self.running(game):
                return clock
            time.sleep(1)
        clock.close()
        raise RuntimeError(f"{package} did not start a game (is its game code current, and the game folder whole?)")

    def running(self, game):
        return bool(self.shell(f"pidof {self.package(game)}:game").strip())

    def stop(self, game):
        self.shell(f"am force-stop {self.package(game)}; svc power stayon false")

    def remove(self, game):
        """Takes the test app, its game code and everything they wrote off the phone."""
        package = self.package(game)
        self.shell(f"am force-stop {package}")
        # Before the app goes, and its folder with it: the packages it was lent (provision).
        lent = f"/sdcard/Android/data/{package}/files/dlc"
        self.shell(f"[ -f {lent}/{LENT} ] && home=$(cat {lent}/{LENT}) && rm {lent}/{LENT} && mv {lent}/* $home/")
        for name in (package, f"{package}.game"):
            self.adb("uninstall", name)
        self.shell(f"rm -rf /sdcard/Android/data/{package} /sdcard/vq_*.mp4")

    def battery(self):
        said = self.shell("dumpsys battery")
        values = {}
        for line in said.splitlines():
            name, _, value = line.strip().partition(": ")
            if name in ("level", "temperature") and value.isdigit():
                values[name] = int(value)
        return values.get("level", 0), values.get("temperature", 0) / 10.0

    def record(self, seconds, folder, stem):
        """Records the screen for this long, in segments; returns [(file, the phone's time at its start)].

        Each segment is its own `screenrecord`, started while the last one
        still runs. The phone's time is read in the same shell just before
        it starts, which places the segment in the run (RunClock) to a few
        tenths of a second.
        """
        segments = []
        threads = []
        errors = []

        def one(index, length):
            remote = f"/sdcard/vq_{stem}_{index}.mp4"
            try:
                said = self.shell(f"date +%s.%N; screenrecord --bit-rate {BIT_RATE} "
                                  f"--time-limit {length} {remote}", timeout=length + 60)
                segments.append((index, remote, float(said.split()[0])))
            except (RuntimeError, ValueError, IndexError, subprocess.TimeoutExpired) as error:
                errors.append(f"segment {index}: {error}")

        started = time.monotonic()
        begin = 0.0
        index = 0
        while begin < seconds:
            time.sleep(max(0.0, started + begin - time.monotonic()))
            length = max(2, int(min(SEGMENT_SECONDS, seconds - begin)))
            thread = threading.Thread(target=one, args=(index, length))
            thread.start()
            threads.append(thread)
            begin += SEGMENT_SECONDS - SEGMENT_OVERLAP
            index += 1
        for thread in threads:
            thread.join()
        if errors:
            raise RuntimeError("; ".join(errors))
        # The file is whole a moment after screenrecord returns.
        time.sleep(1)
        pulled = []
        for index, remote, uptime in sorted(segments):
            local = Path(folder) / f"{stem}_{index}.mp4"
            self.adb("pull", remote, str(local), timeout=600, check=True)
            self.shell(f"rm -f {remote}")
            pulled.append((local, uptime))
        return pulled

    def pull_log(self, game, local):
        self.adb("pull", f"{self.external(game)}/game.log", str(local), check=True)
        return local

    def pull_dumps(self, game, folder):
        """The frames the game wrote itself, when it was asked to; the folder, or None."""
        remote = f"{self.external(game)}/vqframes"
        if not self.shell(f"ls {remote} | head -1").strip():
            return None
        self.adb("pull", remote, str(folder), timeout=1200, check=True)
        self.shell(f"rm -rf {remote}")
        return Path(folder)

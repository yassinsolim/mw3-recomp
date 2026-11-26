"""What a run's own log says of its frames (MW2_REPORT, MW2_REPORT_SECONDS, MW2_STUTTERS).

The recording shows what the display showed; at 120 Hz `screenrecord` drops
frames of its own, and it cannot tell a game that stood still from a view
that did. The log has every frame's time from the game's side, so the
pacing checks and the stalls come from here, and the picture checks ask it
which seconds were play and which a loading screen.
"""
from dataclasses import dataclass, field
import re

_SECOND = re.compile(r"report: second (\d+): (\d+) frames, (\d+) on time, the longest (\d+) ms")
_IN_PLAY = re.compile(r"stutters: in play at ([\d.]+) s")
_STUTTER = re.compile(r"STUTTER \d+: (\d+) blanks? lost at ([\d.]+) s -- ([\d.]+) ms between world frames(.*)")
_STALL = re.compile(r"STALL: no world frame since ([\d.]+) s")
_STALL_OVER = re.compile(r"STALL: over at ([\d.]+) s, ([\d.]+) s after the last world frame")
_TOTAL = re.compile(r"report: (\d+) frames in (\d+) s, (\d+) of them of the world")
_PERIOD = re.compile(r"the period is ([\d.]+) ms")
_DISPLAY = re.compile(r"vulkan: the display runs at ([\d.]+) Hz")
_LIMIT = re.compile(r"MW2_FPS_LIMIT=(\d+)")
_COMPILED = re.compile(r"report: compiled at a draw: (\d+) shaders in \d+ ms \(the longest \d+ ms\), (\d+) pipelines in (\d+) ms")
_NEW_RUN = "===== new run, pid"
_DRIVER = re.compile(r"\] driver: (the system's (?:Vulkan|libvulkan\.so)|\S+ from \S+)")
_GRAPHICS = re.compile(r"report: graphics (.*)")
_DUMP = re.compile(r"renderer: wrote \S*frame_(\d+)\.pnm")
_LINE_TIME = re.compile(r"\s*(\d+\.\d+) \[")


@dataclass
class Second:
    second: int
    frames: int
    on_time: int
    longest_ms: int


@dataclass
class Stutter:
    seconds: float
    blanks: int
    gap_ms: float
    textures: bool  # the renderer spent the gap reading textures it had not seen: a first visit


@dataclass
class Stall:
    start: float
    length: float  # seconds; None when the log ends inside it
    over: bool


@dataclass
class GameLog:
    seconds: list = field(default_factory=list)
    stutters: list = field(default_factory=list)
    stalls: list = field(default_factory=list)
    in_play_at: float = None
    finished: bool = False
    run_seconds: float = None
    frames: int = 0
    world_frames: int = 0
    period_ms: float = None
    display_hz: float = None
    fps_limit: int = None
    pipelines_compiled: int = 0
    lines: int = 0
    # "system" for the phone's own Vulkan driver, else the library the app
    # loaded in its place (the Turnip the build carries); None when the log
    # does not say (a Windows run).
    driver: str = None
    graphics: str = None
    # (frame number, second of the run) of the pictures the game wrote itself that the log has a
    # line for. It has one for the first of a row of them and counts the rest ("(×64)").
    dumps: list = field(default_factory=list)

    def dump_seconds(self, number):
        """The second of the run a picture with this frame number was written in, between the lines the log has."""
        marks = sorted(self.dumps)
        if self.finished and marks and self.frames > marks[-1][0]:
            marks.append((self.frames, float(self.run_seconds)))
        if not marks:
            return number / 60.0
        for (frame, seconds), (next_frame, next_seconds) in zip(marks, marks[1:]):
            if frame <= number <= next_frame:
                return seconds + (next_seconds - seconds) * (number - frame) / max(1, next_frame - frame)
        frame, seconds = marks[0] if number < marks[0][0] else marks[-1]
        return seconds + (number - frame) / 60.0

    def play_seconds(self):
        """The seconds of the run that were play: those the log has a line for."""
        return {row.second for row in self.seconds}

    def in_play(self, seconds, margin=1.0):
        """Whether the run was in play at this time, and a margin either side of it.

        The margin keeps the frames at a loading screen's two ends, which a
        recording's clock cannot place to better than a second, out of play.
        """
        play = self.play_seconds()
        return all(int(seconds + offset) in play for offset in (-margin, 0.0, margin))


def parse(text):
    """Reads the last run in a log's text."""
    start = text.rfind(_NEW_RUN)
    if start > 0:
        text = text[start:]
    log = GameLog()
    open_stall = None
    for line in text.splitlines():
        log.lines += 1
        if "report: second" in line:
            found = _SECOND.search(line)
            if found:
                log.seconds.append(Second(*(int(value) for value in found.groups())))
            continue
        if "STUTTER" in line:
            found = _STUTTER.search(line)
            if found:
                log.stutters.append(Stutter(float(found.group(2)), int(found.group(1)), float(found.group(3)),
                                            "reading new textures" in found.group(4)))
            continue
        if "STALL" in line:
            found = _STALL_OVER.search(line)
            if found:
                length = float(found.group(2))
                # The line that opens a stall may be missing from a log cut at its head.
                begin = open_stall if open_stall is not None else float(found.group(1)) - length
                log.stalls.append(Stall(begin, length, True))
                open_stall = None
                continue
            found = _STALL.search(line)
            if found:
                open_stall = float(found.group(1))
            continue
        for pattern, name, cast in ((_IN_PLAY, "in_play_at", float), (_PERIOD, "period_ms", float),
                                    (_DISPLAY, "display_hz", float), (_LIMIT, "fps_limit", int)):
            found = pattern.search(line)
            if found and (name != "in_play_at" or log.in_play_at is None):
                setattr(log, name, cast(found.group(1)))
        found = _TOTAL.search(line)
        if found:
            log.frames, log.run_seconds, log.world_frames = (int(value) for value in found.groups())
            log.finished = True
        found = _COMPILED.search(line)
        if found:
            log.pipelines_compiled = int(found.group(2))
        found = _DRIVER.search(line)
        if found:
            # A driver the app chose and could not load is followed by the
            # line that the system's was opened after all: that line decides.
            if found.group(1).startswith("the system"):
                log.driver = "system"
            elif log.driver is None:
                log.driver = found.group(1).split(" from ")[0]
        found = _GRAPHICS.search(line)
        if found:
            log.graphics = found.group(1).strip()
        found = _DUMP.search(line)
        if found:
            # With MW2_LOG_TIME the line says when; without, lines are in
            # order and it is the second after the last one reported.
            said = _LINE_TIME.match(line)
            at = float(said.group(1)) if said else float(log.seconds[-1].second + 1 if log.seconds else 0)
            log.dumps.append((int(found.group(1)), at))
    if open_stall is not None:
        log.stalls.append(Stall(open_stall, None, False))
    return log


def read(path):
    with open(path, encoding="utf-8", errors="replace") as stream:
        return parse(stream.read())

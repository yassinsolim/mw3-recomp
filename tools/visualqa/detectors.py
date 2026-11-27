"""The checks: what in a run's measurements and log is a fault.

Each check has one or two numbers it decides by, kept in `Thresholds` with
what they were measured against. docs/visualqa.md has the labelled
recordings and the precision and recall the numbers below gave on them
(`visualqa.py calibrate` measures them again).
"""
from dataclasses import dataclass, asdict

import cv2
import numpy as np

from frames import CELLS

FAIL = "FAIL"
WARN = "warn"
NOTE = "note"


@dataclass
class Thresholds:
    # Whole-frame flicker: the mean brightness of one frame against both its
    # neighbours, in levels of 255. Advanced Warfare's gamma fault was 13.4
    # on every flash (33 flashes in six recordings); with Brightness four
    # steps down, where nobody saw it, 0.4. The largest in a clean recording
    # was 2.4, in the fastest turn of a match.
    whole_flash_levels: float = 5.0
    # Every region has to step with it: the tenth of them that stepped least
    # by at least this share of the middle one's step. The gamma fault moved
    # the least 0.56 to 0.79 of the middle; a weapon's flash, sparks and an
    # explosion, which light the frame by 6 to 40 levels for a frame, 0.00
    # to 0.25, the regions far from them not at all.
    whole_flash_even: float = 0.4
    whole_flash_events: int = 1
    # Local flicker: the largest patch that differed for one frame alone, as
    # a share of the frame. The sun shadow's wedge is 0.03 to 0.29 of it and
    # the lit patches seen through a doorway 0.008 to 0.009 (83 frames in
    # twelve seconds); clean menus and a clean match standing still 0.0014
    # at most. A fast turn makes patches of its own, up to 0.037 with 0.83
    # of the picture changing, so the share asked for grows with the turn.
    # The turn is measured two ways and the larger taken: by how the two
    # neighbours differ, and by how the frame differs from the one before
    # beyond the patch itself. A ribbed wall sliding past by one rib a frame
    # leaves the neighbours alike and the frame between them different
    # (Rust at 120 Hz: patches of 0.061 with 0.6 of the picture changing and
    # 0.025 with 0.2); Dome's sun shadow lost for a frame in a turn was 0.080
    # with 0.43.
    local_flash_share: float = 0.004
    local_flash_per_moving: float = 0.12
    # A patch that is in a neighbour at another place moved and is not
    # counted at all (frames.py, FLASH_REACH). What is left in calm play is
    # a fault: no frame of the labelled set's clean recordings has one, the
    # empty matches on both phones among them, and the two faults a suite found were
    # one frame each in forty seconds (Rust with half its draws missing,
    # Dome's sun shadow lost). So one such frame fails a run, where the
    # scenario is calm. In a fight the weapons' flashes, sparks and the
    # jolt of a hit are such frames by design (Context.combat_from).
    local_flash_events: int = 1
    # Blocks: rectangles of one size on one lattice in a frame. The recorded
    # lattices have 9 to 59 with a regularity of 0.94 to 1.00 across and
    # down (0.8 and more in 82 of 89 frames of Dome's); clean play has at
    # most 5 rectangles of a size, and they are not in rows and columns.
    # Eight positions at random reach 0.85 both ways one time in sixty, six
    # one time in four: under eight blocks the regularity says nothing.
    block_count: int = 8
    # The renderer's blocks are 32 pixels of a frame 1920 wide (30 to 36 as
    # measured, in all three recordings); Solar's honeycomb wall is a lattice
    # of 40 to 46, and the holes of a perforated screen on Riot (16 to 18),
    # Rust's wire fence (22) and Dome's window grilles (18) are smaller.
    block_side_least: float = 26.0
    block_side_most: float = 38.0
    block_regular: float = 0.85
    block_frames: int = 3
    # Black, white and one-colour frames, in play: how much of the frame, and
    # for how long. A fade between scenes is black for under two seconds; the
    # world missing behind the HUD (Black Ops III) stays.
    black_share: float = 0.97
    white_share: float = 0.60
    # A washed frame has nothing dark in it at all: the level 5% of it is under.
    washed_darkest: float = 150.0
    uniform_spread: float = 3.0
    # A level's own black (Black Tuesday opens on eight seconds of it, and
    # fades for four) is before the scenario's play; in play five seconds
    # of nothing is a fault.
    blank_seconds: float = 5.0
    # Missing geometry: regions that had texture (a spread of 6 levels or
    # more inside the region) and from one frame to the next are one flat
    # colour (under 0.8: the encoder leaves 0.2 to 0.5 on a flat field),
    # four or more together, while no more than this share of the picture
    # changed outside them. The sun shadow's wedge changes 0.10 of the rest;
    # a walk up to a dark wall, which goes flat as well, 0.45 to 0.58.
    textured: float = 6.0
    flat: float = 0.8
    # Four together: three went flat twice in a run on Solar with nothing
    # missing (a dark doorway and a bare wall filling them in a turn).
    missing_regions: int = 4
    # A region that went flat is the flat surface beside it slid in when a
    # region one or two across from it had no texture the frame before and was within this many levels
    # of its colour (Solar's bare walls in a turn: 1 to 4 levels apart).
    flat_slid_levels: float = 8.0
    missing_elsewhere: float = 0.2
    missing_events: int = 2
    # Frozen: no new picture for this long while the log says frames were made.
    frozen_seconds: float = 2.0
    # Pacing, from the log. A second is slow under this share of the rate the
    # run was held to, and has a hitch when its longest frame is this long.
    slow_share: float = 0.5
    hitch_ms: int = 250
    # The first visit to a level compiles and reads what it has never seen:
    # for this long into play slow seconds and hitches are said, not failed.
    first_visit_seconds: float = 30.0
    # After that, this share of the seconds may be slow or hitch.
    bad_seconds_share: float = 0.05
    # Pop-in that lasts: seconds after the first visit in which the renderer
    # still stopped to read textures it had not seen, as a share of them.
    lasting_texture_share: float = 0.5
    # A stall of the log's is a second or more without a world frame.
    stall_seconds: float = 1.0


@dataclass
class Finding:
    check: str
    severity: str
    seconds: float        # of the run where the log places it, else of the recording
    text: str
    frame: int = -1       # row of the recording, -1 for a finding from the log
    until: float = None
    count: int = 1

    def as_dict(self):
        return asdict(self)


@dataclass
class Context:
    """Where a recording lies in the run, and which of its seconds are judged."""
    log: object = None
    # Seconds of the run at the recording's first frame.
    offset: float = 0.0
    # With no log: whether the recording is taken as play throughout.
    assume_play: bool = True
    # Seconds of the run from which the scenario has a fight on screen: shots,
    # sparks and explosions light patches for single frames by design, so
    # from here the checks that would take them for faults only remark.
    combat_from: float = None
    # (from, to): loading screens and cutscenes the scenario knows of.
    exempt: tuple = ()

    def run_seconds(self, seconds):
        return seconds + self.offset

    def in_play(self, seconds):
        at = self.run_seconds(seconds)
        if any(start <= at <= end for start, end in self.exempt):
            return False
        if self.log is None or not self.log.seconds:
            return self.assume_play
        return self.log.in_play(at)

    def in_combat(self, seconds):
        return self.combat_from is not None and self.run_seconds(seconds) >= self.combat_from


def _runs(flags):
    """(first, last) rows of each run of true values."""
    padded = np.concatenate(([False], np.asarray(flags, bool), [False]))
    edges = np.flatnonzero(padded[1:] != padded[:-1])
    return [(int(a), int(b) - 1) for a, b in zip(edges[::2], edges[1::2])]


def _grouped(rows, seconds, gap=1.0):
    """Rows less than `gap` seconds apart, as lists: one fault seen over several frames."""
    groups = []
    for row in rows:
        if groups and seconds[row] - seconds[groups[-1][-1]] < gap:
            groups[-1].append(row)
        else:
            groups.append([row])
    return groups


def whole_flicker(m, context, t):
    """Frames whose whole brightness stepped away from both neighbours and back."""
    distinct = np.flatnonzero(m.repeat == 0)
    if len(distinct) < 3:
        return []
    luma = m.luma[distinct].astype(np.float64)
    rise = luma[1:-1] - luma[:-2]
    fall = luma[1:-1] - luma[2:]
    apart = np.abs(luma[:-2] - luma[2:])
    step = np.where(np.sign(rise) == np.sign(fall), np.minimum(np.abs(rise), np.abs(fall)), 0)
    # A patch bright enough to move the mean is the local check's, which
    # says where it is; this one is for a frame in which everything stepped.
    cells = m.cells[distinct].astype(np.float64)
    cell_rise = cells[1:-1] - cells[:-2]
    cell_fall = cells[1:-1] - cells[2:]
    cell_step = np.where(np.sign(cell_rise) == np.sign(cell_fall), np.minimum(np.abs(cell_rise), np.abs(cell_fall)), 0)
    cell_step = cell_step * np.sign(cell_rise) * np.sign(rise)[:, None]
    hits = []
    for hit in np.flatnonzero((step >= t.whole_flash_levels) & (apart <= 0.5 * step)):
        steps = cell_step[hit][~np.isnan(cells[hit + 1])]
        middle = float(np.median(steps))
        if middle > 0 and float(np.percentile(steps, 10)) >= t.whole_flash_even * middle:
            hits.append(int(hit) + 1)
    rows = [int(distinct[hit]) for hit in hits]
    if not rows:
        return []
    severity = FAIL if len(rows) >= t.whole_flash_events else WARN
    levels = [float(step[hit - 1] * np.sign(rise[hit - 1])) for hit in hits]
    return [Finding("flicker", severity, context.run_seconds(m.seconds[row]),
                    f"the whole frame {'brighter' if level > 0 else 'darker'} by {abs(level):.1f} levels for one frame",
                    frame=row) for row, level in zip(rows, levels)]


def _calm_and_combat(check, rows, m, context, events, weight, describe):
    """Findings for single-frame faults: judged where the scenario is calm, remarked on in a fight."""
    calm = [row for row in rows if not context.in_combat(m.seconds[row])]
    fight = [row for row in rows if context.in_combat(m.seconds[row])]
    severity = FAIL if len(calm) >= events else WARN
    findings = []
    for group in _grouped(calm, m.seconds):
        worst = max(group, key=weight)
        findings.append(Finding(check, severity, context.run_seconds(m.seconds[worst]), describe(group, worst),
                                frame=worst, until=context.run_seconds(m.seconds[group[-1]]), count=len(group)))
    if fight:
        worst = max(fight, key=weight)
        findings.append(Finding(check, NOTE, context.run_seconds(m.seconds[worst]),
                                f"in the fight, where shots and explosions do this by design and it is not judged: "
                                f"{describe(fight, worst)}", frame=worst,
                                until=context.run_seconds(m.seconds[fight[-1]]), count=len(fight)))
    return findings


def local_flicker(m, context, t, whole_rows=()):
    """Patches that differed for one frame alone: a sun shadow lit or lost, a wedge."""
    turning = np.maximum(m.flash_moving, m.moved - m.flash)
    need = t.local_flash_share + t.local_flash_per_moving * turning
    whole = set(whole_rows)
    rows = [int(row) for row in np.flatnonzero(m.flash >= need)
            if row not in whole and context.in_play(m.seconds[row])]

    def describe(group, worst):
        return (f"{len(group)} single-frame patch{'es' if len(group) > 1 else ''}, the largest "
                f"{m.flash[worst] * 100:.1f}% of the frame at {m.flash_x[worst]:.2f},{m.flash_y[worst]:.2f}")

    return _calm_and_combat("local-flicker", rows, m, context, t.local_flash_events,
                            lambda row: m.flash[row], describe)


def blocks(m, context, t):
    """Frames with a lattice of blocks left out or drawn alone."""
    rows = [int(row) for row in np.flatnonzero((m.blocks >= t.block_count) & (m.blocks_regular >= t.block_regular)
                                             & (m.blocks_side >= t.block_side_least) & (m.blocks_side <= t.block_side_most))
            if context.in_play(m.seconds[row])]
    severity = FAIL if len(rows) >= t.block_frames else WARN
    findings = []
    for group in _grouped(rows, m.seconds):
        worst = max(group, key=lambda row: m.blocks[row])
        findings.append(Finding(
            "blocks", severity, context.run_seconds(m.seconds[worst]),
            f"a lattice of {int(m.blocks[worst])} blocks {int(m.blocks_side[worst])} px a side, in {len(group)} frames",
            frame=worst, until=context.run_seconds(m.seconds[group[-1]]), count=len(group)))
    return findings


def blank(m, context, t):
    """Black, white-washed and one-colour pictures that stay, in play."""
    findings = []
    play = np.array([context.in_play(seconds) for seconds in m.seconds])
    kinds = (
        ("black", m.black >= t.black_share, "the picture is black"),
        ("white", (m.white >= t.white_share) | (m.darkest >= t.washed_darkest), "the picture is washed white"),
        ("uniform", (m.spread <= t.uniform_spread) & (m.black < t.black_share), "the picture is one flat colour"),
    )
    for check, flags, text in kinds:
        for first, last in _runs(flags & play):
            length = m.seconds[last] - m.seconds[first]
            if length >= t.blank_seconds:
                findings.append(Finding(check, FAIL, context.run_seconds(m.seconds[first]),
                                        f"{text} for {length:.1f} s in play", frame=(first + last) // 2,
                                        until=context.run_seconds(m.seconds[last]), count=last - first + 1))
    return findings


def missing_geometry(m, context, t):
    """Regions that were textured and are one flat colour from one frame to the next."""
    across, down = CELLS
    regions = across * down
    texture = np.nan_to_num(m.texture, nan=0.0)
    went_flat = np.zeros(texture.shape, bool)
    went_flat[1:] = (texture[:-1] >= t.textured) & (texture[1:] < t.flat)
    rows = []
    sizes = {}
    colour = np.nan_to_num(m.colour, nan=0.0)
    for row in np.flatnonzero(went_flat.sum(axis=1) >= t.missing_regions):
        if not context.in_play(m.seconds[row]):
            continue
        # A flat wall sliding into a region in a turn, as the edge that was
        # in it slides out, is flat there too: the region beside it was that
        # colour, without texture, the frame before. Geometry that goes missing leaves a
        # colour its neighbours did not have.
        was_flat = (texture[row - 1] < t.textured).reshape(down, across)
        before = colour[row - 1].reshape(down, across, 3)
        now = colour[row].reshape(down, across, 3)
        grid = went_flat[row].reshape(down, across).astype(np.uint8)
        for at_down, at_across in zip(*np.nonzero(grid)):
            beside = (slice(max(0, at_down - 1), at_down + 2), slice(max(0, at_across - 2), at_across + 3))
            alike = np.abs(before[beside] - now[at_down, at_across]).max(axis=2) <= t.flat_slid_levels
            if (alike & was_flat[beside]).any():
                grid[at_down, at_across] = 0
        count, _, boxes, _ = cv2.connectedComponentsWithStats(grid, connectivity=4)
        largest = int(boxes[1:, cv2.CC_STAT_AREA].max()) if count > 1 else 0
        # A view turning towards a dark wall or the sky goes flat there too,
        # with the rest of the picture moving; where geometry goes missing
        # the rest stands.
        elsewhere = m.moved[row] - max(float(m.flash[row]), largest / regions)
        if largest >= t.missing_regions and elsewhere <= t.missing_elsewhere:
            rows.append(int(row))
            sizes[int(row)] = largest

    def describe(group, worst):
        return f"{sizes[worst]} of {regions} regions went from textured to one flat colour in a frame"

    return _calm_and_combat("missing", rows, m, context, t.missing_events, lambda row: sizes[row], describe)


def frozen(m, context, t):
    """The picture standing still while the log says the game made frames."""
    if context.log is None or not context.log.seconds:
        return []
    made = {row.second: row.frames for row in context.log.seconds}
    distinct = np.flatnonzero(m.repeat == 0)
    findings = []
    times = np.append(m.seconds[distinct], m.seconds[-1])
    for index, row in enumerate(distinct):
        length = times[index + 1] - times[index]
        if length < t.frozen_seconds:
            continue
        start = context.run_seconds(times[index])
        # Every second of it must be play with frames made: a menu and a
        # loading screen stand still by right.
        seconds = range(int(start) + 1, int(start + length))
        judged = context.in_play(times[index]) and context.in_play(times[index + 1])
        if judged and seconds and all(made.get(second, 0) > 0 for second in seconds):
            findings.append(Finding("frozen", FAIL, start,
                                    f"the picture did not change for {length:.1f} s while the game made "
                                    f"{sum(made[second] for second in seconds)} frames",
                                    frame=int(row), until=start + length))
    return findings


def from_log(log, t, expect_driver=None, exempt=(), by_hand=False):
    """The checks the log alone answers: whether the run ended, its driver, stalls, pacing, pop-in."""
    findings = []
    if expect_driver and log.driver is not None:
        bundled = log.driver != "system"
        if (expect_driver == "bundled") != bundled:
            findings.append(Finding(
                "driver", FAIL, 0.0,
                f"the run used {'the phone’s own Vulkan driver' if not bundled else log.driver}, not "
                f"{'the Turnip the build carries' if expect_driver == 'bundled' else 'the phone’s own'}: "
                "its picture says nothing of a release"))
    if not log.finished:
        last = log.seconds[-1].second if log.seconds else 0
        # A run somebody played and closed has no closing report either, and is not a run that died.
        findings.append(Finding("unfinished", NOTE if by_hand else FAIL, float(last),
                                f"the log has no closing report: the run stopped or was killed after second {last}"))

    def exempted(seconds):
        return any(start <= seconds <= end for start, end in exempt)

    for stall in log.stalls:
        if exempted(stall.start):
            continue
        if stall.over and stall.length < t.stall_seconds:
            continue
        length = f"{stall.length:.1f} s" if stall.over else "to the end of the log"
        findings.append(Finding("stall", FAIL, stall.start, f"no world frame for {length}",
                                until=stall.start + (stall.length or 0.0)))
    if not log.seconds:
        # A build older than MW2_REPORT_SECONDS says when play began and has no line a second.
        if log.finished and log.in_play_at is None:
            findings.append(Finding("no-play", FAIL, 0.0, "the run never reached play: no second of it has a frame of the world"))
        return findings

    rates = sorted(row.frames for row in log.seconds)
    # What the run was held to: the limit when it had one, else what most
    # seconds reached.
    target = log.fps_limit or rates[int(len(rates) * 0.9)]
    first = log.in_play_at if log.in_play_at is not None else log.seconds[0].second
    settled = [row for row in log.seconds if row.second >= first + t.first_visit_seconds and not exempted(row.second)]
    visiting = [row for row in log.seconds if row.second < first + t.first_visit_seconds]

    def bad(row):
        return row.frames < t.slow_share * target or row.longest_ms >= t.hitch_ms

    early = [row for row in visiting if bad(row)]
    if early:
        worst = max(early, key=lambda row: row.longest_ms)
        findings.append(Finding("first-visit", NOTE, float(early[0].second),
                                f"{len(early)} slow seconds in the first {t.first_visit_seconds:.0f} s of play, "
                                f"the longest frame {worst.longest_ms} ms", until=float(early[-1].second), count=len(early)))
    late = [row for row in settled if bad(row)]
    if late:
        worst = max(late, key=lambda row: row.longest_ms)
        slowest = min(late, key=lambda row: row.frames)
        severity = FAIL if len(late) > t.bad_seconds_share * len(settled) else WARN
        findings.append(Finding("pacing", severity, float(late[0].second),
                                f"{len(late)} of {len(settled)} seconds slow or hitching after the first visit: "
                                f"the longest frame {worst.longest_ms} ms (second {worst.second}), "
                                f"the slowest second {slowest.frames} frames of {target} (second {slowest.second})",
                                until=float(late[-1].second), count=len(late)))
    reads = sorted({int(stutter.seconds) for stutter in log.stutters if stutter.textures})
    if reads:
        lasting = [second for second in reads if second >= first + t.first_visit_seconds and not exempted(second)]
        share = len(lasting) / max(1, len(settled))
        severity = FAIL if settled and share > t.lasting_texture_share else NOTE
        findings.append(Finding("pop-in", severity, float(reads[0]),
                                f"textures read for the first time in {len(reads)} seconds of play, {len(lasting)} of "
                                f"them after the first visit ({share * 100:.0f}% of those seconds), the last at {reads[-1]} s",
                                until=float(reads[-1]), count=len(reads)))
    return findings


def from_pictures(m, context, t):
    """Every picture check over one recording's measurements."""
    if len(m) == 0:
        return [Finding("no-picture", FAIL, context.offset, "the recording has no frames")]
    whole = whole_flicker(m, context, t)
    findings = list(whole)
    findings += local_flicker(m, context, t, [finding.frame for finding in whole])
    findings += blocks(m, context, t)
    findings += blank(m, context, t)
    findings += missing_geometry(m, context, t)
    findings += frozen(m, context, t)
    return findings


def verdict(findings):
    return FAIL if any(finding.severity == FAIL for finding in findings) else "PASS"

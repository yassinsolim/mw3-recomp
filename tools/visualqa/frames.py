"""Frames of a run and what is measured of each.

A run's picture comes from one of two places: a screen recording
(`adb shell screenrecord`, H.264, one video frame for each picture the display
showed) or the pictures the game writes itself (`MW2_DUMP_FRAMES`, PNM, exact
to the bit, one every so many frames). Both are read here one frame at a time
and reduced to a row of numbers; the detectors (detectors.py) work on the
rows alone, so a three minute recording is never held in memory.
"""
from dataclasses import dataclass, field
from pathlib import Path
import re

import cv2
import numpy as np

# The picture is measured at this size. A quarter of 1920x1080 keeps a
# 32 pixel block eight pixels wide and averages the encoder's noise away.
SMALL = (480, 270)
# Regions for the local checks: each a sixteenth of the frame's width and a
# ninth of its height, so square on a 16:9 screen.
CELLS = (16, 9)

# What the screen draws over the game, as fractions of the frame (left, top,
# right, bottom). The game's own counters and map change every frame and the
# app's touch controls never do; neither says anything about the world.
MASKS = {
    # A pad in the hand: the title's own HUD alone.
    "hud": [
        (0.00, 0.00, 0.17, 0.27),  # the minimap
        (0.00, 0.76, 0.17, 1.00),  # the score and the clock
        (0.66, 0.80, 1.00, 1.00),  # the weapon, its rounds and the grenades
        (0.28, 0.00, 0.72, 0.09),  # the compass and the kill feed's first line
    ],
    # The app's touch controls and its statistics line over that.
    "touch": [
        (0.00, 0.00, 0.54, 0.07),  # the statistics line
        (0.00, 0.07, 0.21, 0.31),  # LT, LB and the minimap under them
        (0.38, 0.03, 0.62, 0.13),  # the two arrows at the top
        (0.80, 0.00, 1.00, 0.30),  # MENU, RB and RT
        (0.80, 0.40, 1.00, 1.00),  # Y, X, B, A and the right stick's label
        (0.06, 0.60, 0.34, 1.00),  # the left stick, the score under it
        (0.66, 0.66, 0.80, 1.00),  # the right stick, the weapon's rounds
        (0.28, 0.00, 0.72, 0.09),
    ],
    "none": [],
}


def mask_image(name):
    """The pixels of a SMALL frame that are looked at: 1 where they are, 0 under a mask."""
    width, height = SMALL
    keep = np.ones((height, width), np.float32)
    for left, top, right, bottom in MASKS[name]:
        keep[int(top * height):int(np.ceil(bottom * height)), int(left * width):int(np.ceil(right * width))] = 0
    return keep


@dataclass
class Frame:
    index: int
    seconds: float
    picture: np.ndarray  # BGR, as recorded


def video_frames(path):
    """The frames of a recording in order, each with the time it was shown at."""
    capture = cv2.VideoCapture(str(path))
    if not capture.isOpened():
        raise OSError(f"cannot read the recording {path}")
    index = 0
    rate = capture.get(cv2.CAP_PROP_FPS) or 60.0
    while True:
        # The time has to be asked for before the frame is taken: afterwards
        # it is the next frame's.
        seconds = capture.get(cv2.CAP_PROP_POS_MSEC) / 1000.0
        taken, picture = capture.read()
        if not taken:
            break
        # A recording whose container gives no times is taken as evenly spaced.
        if index and seconds <= 0:
            seconds = index / rate
        yield Frame(index, seconds, picture)
        index += 1
    capture.release()


_DUMP_NUMBER = re.compile(r"(\d+)(?=\D*$)")
_FINISHED_FRAME = re.compile(r"^frame_\d+$")


def dump_frames(folder, times=None, frames_a_second=60.0):
    """The game's own pictures in a folder, in the order of the frame numbers in their names.

    The files carry no time: `times` gives a frame number's second of the
    run (the log says when some were written, gamelog.py). Without it a
    frame's number over the rate the run was held to stands in.
    """
    paths = [p for p in Path(folder).iterdir() if p.suffix.lower() in (".pnm", ".ppm", ".png", ".jpg", ".bmp")]
    # Beside the finished frames the game may have written its render
    # targets under longer names; only the frames are a picture of the screen.
    finished = [p for p in paths if _FINISHED_FRAME.match(p.stem)]
    paths = finished or paths

    def number(path):
        found = _DUMP_NUMBER.search(path.stem)
        return int(found.group(1)) if found else 0

    for index, path in enumerate(sorted(paths, key=number)):
        picture = cv2.imread(str(path), cv2.IMREAD_COLOR)
        if picture is None:
            continue
        seconds = times(number(path)) if times else number(path) / frames_a_second
        yield Frame(index, float(seconds), picture)


def frames_of(source, times=None):
    """The frames of a recording, of a folder of the game's own pictures, or of one picture."""
    source = Path(source)
    if source.is_dir():
        return dump_frames(source, times)
    if source.suffix.lower() in (".png", ".jpg", ".pnm", ".ppm", ".bmp"):
        picture = cv2.imread(str(source), cv2.IMREAD_COLOR)
        if picture is None:
            raise OSError(f"cannot read the picture {source}")
        return iter([Frame(0, 0.0, picture)])
    return video_frames(source)


@dataclass
class Measurements:
    """One row a frame, as arrays over the frames."""
    seconds: np.ndarray
    luma: np.ndarray          # mean brightness of what is looked at, 0-255
    cells: np.ndarray         # (frames, cells) mean brightness of each region
    texture: np.ndarray       # (frames, cells) how much each region varies inside
    colour: np.ndarray        # (frames, cells, 3) mean colour of each region
    change: np.ndarray        # mean difference from the frame before, 0-255
    moved: np.ndarray         # share of the pixels that differ from the frame before
    black: np.ndarray         # share of the pixels that are black
    white: np.ndarray         # share that are white
    darkest: np.ndarray       # the brightness 5% of the pixels are under
    spread: np.ndarray        # standard deviation of the brightness
    repeat: np.ndarray        # 1 where the frame is the one before it again
    flash: np.ndarray         # share of the frame in the largest patch that differed for this frame alone
    flash_moving: np.ndarray  # share of the frame that changed between the frames either side
    flash_x: np.ndarray       # where that patch is, as fractions of the frame
    flash_y: np.ndarray
    blocks: np.ndarray        # rectangles of one size standing out from their surround
    blocks_regular: np.ndarray  # how nearly they stand on one lattice, 0-1
    blocks_side: np.ndarray   # their side in pixels of a frame 1920 wide
    size: tuple = (0, 0)
    extra: dict = field(default_factory=dict)

    def __len__(self):
        return len(self.seconds)

    def save(self, path):
        values = {name: getattr(self, name) for name in self.__dataclass_fields__ if name not in ("size", "extra")}
        np.savez_compressed(path, size=np.array(self.size), **values)

    @classmethod
    def load(cls, path):
        with np.load(path) as stored:
            values = {name: stored[name] for name in cls.__dataclass_fields__ if name not in ("size", "extra")}
            return cls(size=tuple(int(v) for v in stored["size"]), **values)


# The block check works on the picture at half its size: a 32 pixel block of
# the scene is 16 to 24 pixels there, by how much smaller the scene is drawn.
BLOCK_PICTURE = (960, 540)
# A block stands out from what is round it by this much. The lattices
# recorded on the AYN Thor (Call of Duty 4's Bog, Modern Warfare 3's Dome,
# Modern Warfare 2's S.S.D.D.) step by 40 to 200; the encoder's own blocks by 2 to 6.
BLOCK_STEP = 24
BLOCK_SIDES = (8, 40)
# Wider than any block, so that a block is small beside it and the walls and
# sky round it are not.
_BLOCK_SURROUND = cv2.getStructuringElement(cv2.MORPH_RECT, (31, 31))


def _on_a_lattice(centres, side):
    """How nearly positions fall at one spacing: 1 when they all do.

    Many spacings are tried and the best kept, which eight positions at
    random reach 0.7 with; the lattices recorded reach 0.94 to 1.00.
    Alternate columns of those are half a spacing apart, so spacings down
    to under one block's side are tried.
    """
    best = 0.0
    for spacing in np.arange(0.75 * side, 6.01 * side, 0.25):
        best = max(best, float(abs(np.exp(2j * np.pi * centres / spacing).mean())))
    return best


def block_lattice(picture, keep):
    """Blocks left out of a picture: how many, how nearly on a lattice, and their side.

    Scene blocks that were not drawn (or drawn black, or alone drawn) are
    upright rectangles of one size standing on the lines of one lattice.
    They are found as small patches darker or lighter than their surround,
    kept when filled rectangles of one size, and counted when they stand in
    at least three columns and three rows. A picture's own windows and
    letters are neither one size nor in rows and columns both; a wall of
    honeycomb and the slots of a grille are, and are not square as the
    renderer's blocks are (Solar: 23 by 21 and 13 by 9 at this size, where
    the lattices recorded are 15 to 18 a side and within 1 of square).
    """
    grey = cv2.cvtColor(cv2.resize(picture, BLOCK_PICTURE, interpolation=cv2.INTER_AREA), cv2.COLOR_BGR2GRAY)
    best = (0, 0.0, 0.0)
    for patches in (cv2.MORPH_BLACKHAT, cv2.MORPH_TOPHAT):
        stands_out = cv2.morphologyEx(grey, patches, _BLOCK_SURROUND)
        found = ((stands_out >= BLOCK_STEP) & keep).astype(np.uint8)
        count, _, boxes, centres = cv2.connectedComponentsWithStats(found, connectivity=4)
        if count < 7:
            continue
        boxes, centres = boxes[1:], centres[1:]
        wide, high, area = boxes[:, cv2.CC_STAT_WIDTH], boxes[:, cv2.CC_STAT_HEIGHT], boxes[:, cv2.CC_STAT_AREA]
        low, most = BLOCK_SIDES
        blocks = ((wide >= low) & (wide <= most) & (high >= low) & (high <= most) & (area >= 0.8 * wide * high)
                  & (np.maximum(wide, high) <= 2 * np.minimum(wide, high)))
        if blocks.sum() < 6:
            continue
        side_across, side_down = np.median(wide[blocks]), np.median(high[blocks])
        if abs(side_across - side_down) > 1.5:
            continue
        alike = blocks & (np.abs(wide - side_across) <= 2) & (np.abs(high - side_down) <= 2)
        number = int(alike.sum())
        if number <= best[0] or number < 6:
            continue
        across, down = centres[alike, 0], centres[alike, 1]
        columns = len(np.unique(np.round(across / side_across)))
        rows = len(np.unique(np.round(down / side_down)))
        if columns < 3 or rows < 3:
            continue
        # Across and down both: positions in one row are on a lattice across by chance one time in ten.
        regular = min(_on_a_lattice(across, side_across), _on_a_lattice(down, side_down))
        best = (number, regular, float(side_across) * 2)
    return best


# A pixel flashed when it differs from the frame before and the frame after
# by this much, the same way, while those two agree to within half of it.
# The whole-frame flash of Advanced Warfare's gamma fault was 13 levels; the
# sun shadow's wedge 60 to 200.
FLASH_STEP = 12
_FLASH_THICK = np.ones((5, 5), np.uint8)
# A thing that moves further in a frame than it is thick is a patch that
# differs from both neighbours too: a post or a shadow's band passing in a
# turn, the whole view at the frame a turn reverses in, a beam in a jump.
# Such a patch is in a neighbour, moved; a flash is in neither. The patch is
# looked for in both neighbours this far (across, down) either way, in
# pixels of SMALL: the fastest turn recorded moves the view 59 a frame at
# 60 frames a second, a jump 26.
FLASH_REACH = (72, 40)
# It moved when one neighbour, with the patch moved, differs from it by under
# this share of what it differs by where it stands. Posts, bands and a
# reversing view measured 0.13 to 0.60; a sun shadow lost for a frame and a
# frame with half the world undrawn 0.93 to 0.96.
FLASH_MOVED = 0.7
# And is the same shapes there, not only nearer in brightness, as a darker
# wall is to a patch gone black: the two agree by this much (a correlation;
# the moved ones measured 0.44 to 0.99 at their place, a flat patch 0.18).
FLASH_MOVED_ALIKE = 0.35
# The largest patches of a frame are tried in turn: behind a post that moved
# there may be a patch that flashed.
FLASH_PATCHES = 4


def _flashed_patches(before, frame, after, seen):
    """The thick patches of `frame` that differ from both neighbours alike, the largest first: (pixels, x, y, box)."""
    rise = frame - before
    fall = frame - after
    apart = np.abs(before - after)
    least = np.where(np.sign(rise) == np.sign(fall), np.minimum(np.abs(rise), np.abs(fall)), 0)
    flashed = ((least >= FLASH_STEP) & (apart <= 0.5 * least) & seen).astype(np.uint8)
    thick = cv2.morphologyEx(flashed, cv2.MORPH_OPEN, _FLASH_THICK)
    count, _, boxes, centres = cv2.connectedComponentsWithStats(thick, connectivity=8)
    patches = [(int(boxes[index, cv2.CC_STAT_AREA]), float(centres[index, 0]), float(centres[index, 1]),
                tuple(int(value) for value in boxes[index, :4])) for index in range(1, count)]
    return sorted(patches, reverse=True)[:FLASH_PATCHES]


def _moved_from(neighbour, frame, box, seen):
    """Whether what `frame` has in `box` is in `neighbour` at another place within reach."""
    left, top, wide, high = box
    # A little of what is round the patch goes with it: its edges are what place it.
    left, top = max(0, left - 4), max(0, top - 4)
    right, bottom = min(frame.shape[1], left + wide + 8), min(frame.shape[0], top + high + 8)
    across, down = FLASH_REACH
    around = cv2.copyMakeBorder(neighbour, down, down, across, across, cv2.BORDER_REPLICATE)
    around = around[top:bottom + 2 * down, left:right + 2 * across]
    patch = frame[top:bottom, left:right]
    # What the HUD covers stands still while the world moves under it, and is left out
    # (a pillar passing behind the minimap was a patch that flashed).
    looked = seen[top:bottom, left:right]
    differs = cv2.matchTemplate(around, patch, cv2.TM_SQDIFF, mask=looked.astype(np.float32))
    least, _, (place_across, place_down), _ = cv2.minMaxLoc(differs)
    if least >= FLASH_MOVED * FLASH_MOVED * float(differs[down, across]):
        return False
    there = around[place_down:place_down + patch.shape[0], place_across:place_across + patch.shape[1]]
    if there[looked].std() == 0 or patch[looked].std() == 0:
        return False
    return float(np.corrcoef(there[looked], patch[looked])[0, 1]) >= FLASH_MOVED_ALIKE


def flash(before, frame, after, seen):
    """The largest patch that was different for this one frame, as a share of the frame, and where.

    Things moving across the picture differ from both neighbours too, but in
    thin lines along their edges: only patches at least five pixels thick
    (twenty of the screen's) are kept, and of those only the ones that are
    in neither neighbour at another place (FLASH_REACH): a post passing in
    a turn is, and so is the whole view at a hit's jolt. The share of the
    picture that changed between the two neighbours is returned beside it,
    for the detector to ask more of a frame in a fast turn.
    """
    moving = float((np.abs(before - after)[seen] > 6).mean())
    for pixels, across, down, box in _flashed_patches(before, frame, after, seen):
        if _moved_from(before, frame, box, seen) or _moved_from(after, frame, box, seen):
            continue
        height, width = frame.shape
        return pixels / float(seen.sum()), moving, across / width, down / height
    return 0.0, moving, 0.0, 0.0


def measure(frames, mask="hud", progress=None):
    """Reads every frame once and returns its measurements."""
    keep = mask_image(mask)
    seen = keep > 0
    kept = float(keep.sum())
    block_keep = cv2.resize(keep, BLOCK_PICTURE, interpolation=cv2.INTER_NEAREST) > 0
    cell_weight = cv2.resize(keep, CELLS, interpolation=cv2.INTER_AREA)
    # A region more than half under a mask says too little to be judged.
    cell_seen = cell_weight >= 0.5
    safe_weight = np.where(cell_weight > 0, cell_weight, 1)
    rows = {name: [] for name in Measurements.__dataclass_fields__ if name not in ("size", "extra")}
    # The last two frames that differed from the one before them, with their
    # rows: a game at 30 frames a second is recorded twice a frame, and a
    # flash is one frame of the game's, not of the recording's.
    earlier = None
    last = None
    last_row = None
    size = (0, 0)
    for row, frame in enumerate(frames):
        picture = frame.picture
        size = (picture.shape[1], picture.shape[0])
        small_colour = cv2.resize(picture, SMALL, interpolation=cv2.INTER_AREA)
        small = cv2.cvtColor(small_colour, cv2.COLOR_BGR2GRAY).astype(np.float32)
        masked = small * keep
        rows["seconds"].append(frame.seconds)
        rows["luma"].append(float(masked.sum()) / kept)
        means = cv2.resize(masked, CELLS, interpolation=cv2.INTER_AREA) / safe_weight
        squares = cv2.resize(masked * small, CELLS, interpolation=cv2.INTER_AREA) / safe_weight
        rows["cells"].append(np.where(cell_seen, means, np.nan).ravel())
        rows["texture"].append(np.where(cell_seen, np.sqrt(np.maximum(squares - means * means, 0)), np.nan).ravel())
        colour = cv2.resize(small_colour.astype(np.float32) * keep[:, :, None], CELLS, interpolation=cv2.INTER_AREA)
        rows["colour"].append((colour / safe_weight[:, :, None]).reshape(-1, 3))
        values = small[seen]
        rows["black"].append(float((values < 16).mean()))
        rows["white"].append(float((values > 235).mean()))
        rows["darkest"].append(float(np.percentile(values, 5)))
        rows["spread"].append(float(values.std()))
        for name in ("flash", "flash_moving", "flash_x", "flash_y"):
            rows[name].append(0.0)
        if last is None:
            change, moved = 0.0, 0.0
        else:
            difference = np.abs(small - last)[seen]
            change, moved = float(difference.mean()), float((difference > 6).mean())
        rows["change"].append(change)
        rows["moved"].append(moved)
        # The encoder writes a picture that did not change again with a
        # difference of a few hundredths of a level and no pixel moved.
        repeated = last is not None and moved == 0.0 and change < 0.1
        rows["repeat"].append(1.0 if repeated else 0.0)
        if not repeated:
            if earlier is not None:
                found = flash(earlier, last, small, seen)
                for name, value in zip(("flash", "flash_moving", "flash_x", "flash_y"), found):
                    rows[name][last_row] = value
            earlier, last, last_row = last, small, row
        number, regular, side = block_lattice(picture, block_keep)
        rows["blocks"].append(number)
        rows["blocks_regular"].append(regular)
        rows["blocks_side"].append(side)
        if progress and row % 600 == 0:
            progress(frame)
    arrays = {name: np.array(values, np.float32) for name, values in rows.items()}
    arrays["seconds"] = np.array(rows["seconds"], np.float64)
    return Measurements(size=size, **arrays)


def pictures_at(source, indices):
    """The frames with these indices, read again from the source: {index: picture}."""
    wanted = set(int(i) for i in indices)
    found = {}
    if not wanted:
        return found
    last = max(wanted)
    for frame in frames_of(source):
        if frame.index in wanted:
            found[frame.index] = frame.picture
        if frame.index >= last:
            break
    return found

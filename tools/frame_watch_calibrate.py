#!/usr/bin/env python3
"""What the watch over a run's first frames would say of recordings of real runs.

    python tools/frame_watch_calibrate.py <list.txt> [--still <screenshot.png>]... [--write <fixture.tsv>]

The app looks at a frame twice a second until it has seen a picture on a
driver (runtime/gpu/vulkan/picture_probe.h) and judges the looks
(FrameWatch.kt). This takes screen recordings (the private ones the visual
checks are calibrated on, tools/visualqa/calibration.json, and any others:
<list.txt> names one .mp4 a line), looks at them as the runtime does, and
prints how far real frames are from each rule: change a number in
FrameWatch.kt only with this beside it. --still measures single pictures,
which have no look before them (a player's screenshot of a fault).

--write keeps the numbers of every look, no picture of any, as the fixture
FrameWatchTest reads (android/app/src/test/resources/frame_looks.tsv): a
line a recording, `name<TAB>looks`, a look `isolated.ground.stable`, or `F`
for one colour.

A recording is not what the runtime sees: it has the app's own overlay on
it (the touch buttons, the counter), which the runtime's copy is taken
under. Samples unlike their neighbours at the same place in three looks of
five are taken for the overlay and left out, and a look is one colour when
93% of it is and 2% at the most is unlike its neighbours. Needs numpy and
OpenCV.
"""
import argparse
import os
import sys

import cv2
import numpy as np

WIDTH, HEIGHT = 64, 36
INNER = (WIDTH - 2) * (HEIGHT - 2)
# FrameWatch.kt's numbers.
SPECKLE, GROUND, MOVED = 80, 333, 4
GOOD_LOOKS, NOISE_LOOKS, FLAT_LOOKS = 6, 40, 120


def samples(image):
    """The nearest samples of the centred 16:9 picture of a BGR frame, red first."""
    height, width = image.shape[:2]
    across = min(width, int(round(height * 16 / 9)))
    xs = (width - across) // 2 + ((np.arange(WIDTH) + 0.5) * across / WIDTH).astype(int)
    ys = ((np.arange(HEIGHT) + 0.5) * height / HEIGHT).astype(int)
    return image[ys][:, xs][:, :, ::-1].astype(np.int16)


def unlike(a, b, by):
    return (np.abs(a - b) > by).any(axis=-1)


def isolated(looks):
    centre = looks[:, 1:-1, 1:-1]
    return (unlike(centre, looks[:, 1:-1, :-2], 24) & unlike(centre, looks[:, 1:-1, 2:], 24) &
            unlike(centre, looks[:, :-2, 1:-1], 24) & unlike(centre, looks[:, 2:, 1:-1], 24))


def measure(looks, recording=True):
    """vk::probe::Analyse of each look of an array [look, y, x, rgb]: per thousand isolated, ground, stable; and flat."""
    looks = looks.astype(np.int16)
    alone = isolated(looks)
    if recording and len(looks) > 20:
        alone &= ~(alone.mean(axis=0) >= 0.6)
    changed = np.zeros(looks.shape[:3], bool)
    changed[1:] = unlike(looks[1:], looks[:-1], 24)
    same = alone.copy()
    same[1:] = alone[1:] & alone[:-1] & ~changed[1:, 1:-1, 1:-1]
    flatly = looks.reshape(len(looks), -1, 3)
    middle = np.sort(flatly, axis=1)[:, WIDTH * HEIGHT // 2][:, None, None, :]
    ground = (~unlike(looks, middle, 6)).reshape(len(looks), -1).sum(axis=1) * 1000 // (WIDTH * HEIGHT)
    n_alone = alone.sum(axis=(1, 2)) * 1000 // INNER
    n_same = same.sum(axis=(1, 2)) * 1000 // INNER
    if recording:
        flat = (ground >= 930) & (n_alone <= 20)
    else:
        flat = ~unlike(looks, looks[:, :1, :1], 6).reshape(len(looks), -1).any(axis=1)
    return n_alone, ground, n_same, flat


def recording(path):
    capture = cv2.VideoCapture(path)
    rate = capture.get(cv2.CAP_PROP_FPS) or 30.0
    every = max(1, int(round(rate / 2)))
    looks, index = [], 0
    while capture.grab():
        if index % every == 0:
            ok, image = capture.retrieve()
            if not ok:
                break
            looks.append(samples(image))
        index += 1
    return np.array(looks)


def watch(speckle, flat):
    """FrameWatch over the looks from the first, every flat one taken as drawn on: the verdict."""
    judged = flats = speckles = run = 0
    for is_speckle, is_flat in zip(speckle, flat):
        judged += 1
        if is_flat:
            flats += 1
            run = 0
        elif is_speckle:
            speckles += 1
            run = 0
        else:
            run += 1
        if run >= GOOD_LOOKS:
            return "good"
        if judged >= NOISE_LOOKS and speckles * 5 >= judged * 4:
            return "noise"
        if judged >= FLAT_LOOKS and flats == judged:
            return "flat"
    return "watching"


def longest(flags):
    best = run = 0
    for flag in flags:
        run = run + 1 if flag else 0
        best = max(best, run)
    return best


def name_of(path):
    parts = os.path.splitext(path.replace("\\", "/"))[0].split("/")
    return "/".join(parts[-4:]).replace("\t", " ")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("list")
    parser.add_argument("--still", action="append", default=[])
    parser.add_argument("--write")
    args = parser.parse_args()

    for path in args.still:
        image = cv2.imdecode(np.fromfile(path, dtype=np.uint8), cv2.IMREAD_COLOR)
        alone, ground, _, flat = measure(samples(image)[None], recording=False)
        print(f"still {os.path.basename(path)}: {alone[0]} per thousand unlike their neighbours (speckle from {SPECKLE}),"
              f" {ground[0]} the ground's colour (from {GROUND}){', one colour' if flat[0] else ''}")

    paths = [line.strip() for line in open(args.list, encoding="utf-8") if line.strip().lower().endswith(".mp4")]
    total = speckles = 0
    worst_window, worst_run, worst_flat, most_alone = 0.0, 0, 0, 0
    verdicts = {"good": 0, "noise": 0, "flat": 0, "watching": 0}
    lines = []
    for path in paths:
        looks = recording(path)
        if len(looks) < 10:
            continue
        alone, ground, same, flat = measure(looks)
        speckle = ~flat & (alone >= SPECKLE) & (ground >= GROUND) & (same * MOVED <= alone)
        speckle[0] = False
        total += len(looks)
        speckles += int(speckle.sum())
        most_alone = max(most_alone, int(alone.max()))
        if len(looks) >= NOISE_LOOKS:
            worst_window = max(worst_window, np.convolve(speckle.astype(float), np.ones(NOISE_LOOKS) / NOISE_LOOKS, "valid").max())
        worst_run = max(worst_run, longest(speckle))
        worst_flat = max(worst_flat, longest(flat))
        # As if the run had begun at any look of the recording: no start is known to be the one.
        for start in range(len(looks)):
            verdicts[watch(speckle[start:], flat[start:])] += 1
        lines.append(name_of(path) + "\t" + " ".join(
            "F" if f else f"{a}.{g}.{s}" for a, g, s, f in zip(alone, ground, same, flat)))
    print(f"{len(lines)} recordings, {total} looks")
    print(f"samples unlike their neighbours: {most_alone} per thousand at the most")
    print(f"speckle looks: {speckles}; the longest run {worst_run} (the verdict wants {NOISE_LOOKS * 4 // 5} of {NOISE_LOOKS});"
          f" the worst {NOISE_LOOKS // 2} s hold {worst_window:.0%} (the verdict wants 80%)")
    print(f"one colour: the longest stretch {worst_flat / 2:.1f} s (the verdict wants {FLAT_LOOKS // 2} s)")
    print(f"the watch begun at every look: {verdicts}")
    if args.write:
        with open(args.write, "w", encoding="utf-8", newline="\n") as out:
            out.write("\n".join(lines) + "\n")
        print(f"wrote {args.write}")
    return 1 if verdicts["noise"] or verdicts["flat"] else 0


if __name__ == "__main__":
    sys.exit(main())

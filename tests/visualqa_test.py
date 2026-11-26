"""tools/visualqa's checks on pictures drawn here: no frame of a game is needed or kept."""
from pathlib import Path
import sys
import tempfile
import unittest

import cv2
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools/visualqa"))

import detectors  # noqa: E402
import device  # noqa: E402
import frames  # noqa: E402
import gamelog  # noqa: E402
import scenarios  # noqa: E402

WIDTH, HEIGHT = 960, 540


def scene(seed=1):
    """A textured picture: blurred noise over a slope of brightness, as a wall and a floor are."""
    noise = np.random.default_rng(seed).integers(70, 190, (HEIGHT // 12, WIDTH // 12, 3)).astype(np.uint8)
    return cv2.GaussianBlur(cv2.resize(noise, (WIDTH, HEIGHT), interpolation=cv2.INTER_CUBIC), (0, 0), 8)


def run_of(pictures, rate=60.0):
    return frames.measure(frames.Frame(index, index / rate, picture) for index, picture in enumerate(pictures))


def checks(pictures, **context):
    found = detectors.from_pictures(run_of(pictures), detectors.Context(**context), detectors.Thresholds())
    return {finding.check: finding.severity for finding in found}


def drifting(count, base=None):
    """Frames of one scene sliding sideways two pixels a frame: a view that moves and has no fault."""
    base = scene() if base is None else base
    return [np.roll(base, 2 * index, axis=1) for index in range(count)]


class PictureChecks(unittest.TestCase):
    def test_clean_run_passes(self):
        self.assertEqual(checks(drifting(40)), {})

    def test_whole_frame_flash(self):
        pictures = drifting(40)
        pictures[20] = cv2.add(pictures[20], 13)
        self.assertEqual(checks(pictures).get("flicker"), detectors.FAIL)
        # In a menu too: the fault it was measured on was seen in a lobby.
        self.assertEqual(checks(pictures, assume_play=False).get("flicker"), detectors.FAIL)

    def test_flash_nobody_sees_is_not_flicker(self):
        pictures = drifting(40)
        pictures[20] = cv2.add(pictures[20], 1)
        self.assertEqual(checks(pictures), {})

    def test_a_step_that_stays_is_not_flicker(self):
        pictures = drifting(40)
        for index in range(20, 40):
            pictures[index] = cv2.add(pictures[index], 13)
        self.assertNotIn("flicker", checks(pictures))

    def test_flash_at_thirty_frames_a_second(self):
        # Each of the game's frames recorded twice: the flash is two frames of the recording long.
        game = drifting(20)
        game[10] = cv2.add(game[10], 13)
        pictures = [picture for picture in game for _ in range(2)]
        self.assertEqual(checks(pictures).get("flicker"), detectors.FAIL)

    def wedge(self, picture):
        lit = picture.copy()
        cv2.fillPoly(lit, [np.array([[100, 500], [900, 150], [900, 420]])], (235, 240, 250))
        return lit

    def test_patch_lit_for_one_frame(self):
        pictures = drifting(60)
        pictures[20] = self.wedge(pictures[20])
        # One frame of it in a run is the fault: the faults found were single frames.
        self.assertEqual(checks(pictures).get("local-flicker"), detectors.FAIL)
        # A menu's own animation may flash; the patches are judged in play alone.
        self.assertNotIn("local-flicker", checks(pictures, assume_play=False))

    def test_in_a_fight_flashes_are_remarked_not_failed(self):
        pictures = drifting(60)
        pictures[20] = self.wedge(pictures[20])
        pictures[40] = self.wedge(pictures[40])
        self.assertEqual(checks(pictures, combat_from=0.0).get("local-flicker"), detectors.NOTE)
        # A patch that lights a third of the frame moves its mean, and is still not the whole frame's flicker.
        self.assertNotIn("flicker", checks(pictures))

    def test_jolt_of_a_hit_is_not_flicker(self):
        # One frame of the same picture moved and back, as a hit moves the player's view.
        base = scene()
        pictures = [base] * 20 + [np.roll(base, 24, axis=1)] + [base] * 20 + [np.roll(base, -20, axis=0)] + [base] * 20
        pictures = [cv2.add(picture, index % 2) for index, picture in enumerate(pictures)]
        self.assertNotIn("local-flicker", checks(pictures))

    def test_fast_turn_is_not_flicker(self):
        base = scene()
        pictures = [np.roll(base, 37 * index, axis=1) for index in range(40)]
        self.assertEqual(checks(pictures), {})

    def test_post_passing_in_a_turn_is_not_flicker(self):
        # A dark post 60 pixels thick crossing the view 100 pixels a frame: where it stands in a
        # frame neither neighbour has it, and each neighbour has it a little further on.
        pictures = []
        for index in range(12):
            picture = scene().copy()
            picture[60:480, 150 + 100 * (index % 6):210 + 100 * (index % 6)] = 20
            pictures.append(cv2.add(picture, index % 2))
        self.assertNotIn("local-flicker", checks(pictures))
        # The same patch in one frame alone is in neither neighbour anywhere.
        still = drifting(12)
        still[6] = still[6].copy()
        still[6][60:480, 450:510] = 20
        self.assertEqual(checks(still).get("local-flicker"), detectors.FAIL)

    def lattice(self, picture):
        holed = picture.copy()
        for row in range(8):
            for column in range(12):
                left = 60 + column * 45
                top = 30 + row * 30 + (15 if column % 2 else 0)
                holed[top:top + 17, left:left + 17] = 0
        return holed

    def test_block_lattice(self):
        pictures = drifting(30)
        for index in range(10, 16):
            pictures[index] = self.lattice(pictures[index])
        found = checks(pictures)
        self.assertEqual(found.get("blocks"), detectors.FAIL)

    def test_scattered_rectangles_are_not_a_lattice(self):
        picture = scene()
        spots = np.random.default_rng(5).integers(0, 400, (12, 2))
        for across, down in spots:
            side = 10 + (across % 14)
            picture[down:down + side, across * 2:across * 2 + side] = 0
        self.assertNotIn("blocks", checks([picture] * 8 + drifting(8, picture)))

    def test_black_in_play(self):
        pictures = drifting(30) + [np.zeros((HEIGHT, WIDTH, 3), np.uint8)] * 360 + drifting(30)
        self.assertEqual(checks(pictures).get("black"), detectors.FAIL)
        self.assertNotIn("black", checks(pictures, assume_play=False))

    def test_short_fade_is_not_a_fault(self):
        pictures = drifting(30) + [np.zeros((HEIGHT, WIDTH, 3), np.uint8)] * 60 + drifting(30)
        self.assertNotIn("black", checks(pictures))

    def test_washed_white(self):
        washed = cv2.add(cv2.convertScaleAbs(scene(), alpha=0.2), 200)
        self.assertEqual(checks(drifting(30) + [washed] * 360).get("white"), detectors.FAIL)

    def test_ground_gone(self):
        pictures = drifting(60)
        for index in (20, 21, 40, 41):
            pictures[index] = pictures[index].copy()
            pictures[index][300:540, 200:760] = (128, 128, 128)
        self.assertEqual(checks(pictures).get("missing"), detectors.FAIL)

    def test_frozen_while_the_game_runs(self):
        still = scene()
        pictures = drifting(60) + [still] * 240 + drifting(60)
        log = gamelog.parse("".join(
            f"[i] report: second {second}: 60 frames, 60 on time, the longest 17 ms; a frame\n" for second in range(0, 8))
            + "[i] report: 400 frames in 7 s, 400 of them of the world\n")
        self.assertEqual(checks(pictures, log=log).get("frozen"), detectors.FAIL)
        # Without a log nothing says the game went on: a still view is not a fault.
        self.assertNotIn("frozen", checks(pictures))


LOG = """
===== new run, pid 1 =====
[i] android: MW2_FPS_LIMIT=60
[i] driver: libvulkan_freedreno.so from /data/user/0/app/files/drivers/turnip
[i] stutters: in play at 10.200 s, watching
{seconds}
[w] STUTTER 1: 2 blanks lost at 12.500 s -- 41.0 ms between world frames; the title presented late; of that, 30.1 ms reading new textures (3)
{tail}
"""


def log_of(rows, tail="[i] report: 5000 frames in 100 s, 4000 of them of the world"):
    lines = "\n".join(f"[i] report: second {second}: {made} frames, {made} on time, the longest {longest} ms; a frame"
                      for second, made, longest in rows)
    return gamelog.parse(LOG.format(seconds=lines, tail=tail))


def log_checks(log, **more):
    return {finding.check: finding.severity for finding in detectors.from_log(log, detectors.Thresholds(), **more)}


class LogChecks(unittest.TestCase):
    def test_reads_the_run(self):
        log = log_of([(second, 60, 17) for second in range(11, 100)])
        self.assertTrue(log.finished)
        self.assertEqual(log.driver, "libvulkan_freedreno.so")
        self.assertEqual(log.fps_limit, 60)
        self.assertAlmostEqual(log.in_play_at, 10.2)
        self.assertTrue(log.in_play(50.5))
        self.assertFalse(log.in_play(10.5))
        self.assertEqual(len(log.stutters), 1)
        self.assertTrue(log.stutters[0].textures)

    def test_steady_run_passes(self):
        found = log_checks(log_of([(second, 60, 17) for second in range(11, 100)]), expect_driver="bundled")
        self.assertEqual(found, {"pop-in": detectors.NOTE})

    def test_first_visit_is_said_not_failed(self):
        rows = [(second, 20 if second < 20 else 60, 400 if second < 20 else 17) for second in range(11, 100)]
        self.assertEqual(log_checks(log_of(rows)).get("first-visit"), detectors.NOTE)
        self.assertNotIn("pacing", log_checks(log_of(rows)))

    def test_slow_after_the_first_visit_fails(self):
        rows = [(second, 20 if second > 70 else 60, 17) for second in range(11, 100)]
        self.assertEqual(log_checks(log_of(rows)).get("pacing"), detectors.FAIL)

    def test_stall(self):
        tail = ("[w] STALL: no world frame since 60.000 s; what every thread is doing follows\n"
                "[w] STALL: over at 64.500 s, 4.5 s after the last world frame\n"
                "[i] report: 5000 frames in 100 s, 4000 of them of the world")
        log = log_of([(second, 60, 17) for second in range(11, 100)], tail)
        self.assertEqual(log_checks(log).get("stall"), detectors.FAIL)
        # A stall the scenario expects (a level loading) is not one.
        self.assertNotIn("stall", log_checks(log, exempt=((0.0, 65.0),)))

    def test_run_that_never_ended(self):
        tail = "[w] STALL: no world frame since 60.000 s; what every thread is doing follows"
        found = log_checks(log_of([(second, 60, 17) for second in range(11, 60)], tail))
        self.assertEqual(found.get("unfinished"), detectors.FAIL)
        self.assertEqual(found.get("stall"), detectors.FAIL)
        # A run somebody played and closed has no closing report either: analysing its recording does not fail on that.
        log = log_of([(second, 60, 17) for second in range(11, 60)], "")
        self.assertEqual(log_checks(log, by_hand=True).get("unfinished"), detectors.NOTE)

    def test_the_phones_own_driver_fails_a_run(self):
        text = LOG.format(seconds="", tail="[i] report: 5 frames in 1 s, 0 of them of the world")
        log = gamelog.parse(text + "[i] driver: the system's libvulkan.so\n")
        self.assertEqual(log.driver, "system")
        self.assertEqual(log_checks(log, expect_driver="bundled").get("driver"), detectors.FAIL)
        self.assertNotIn("driver", log_checks(log, expect_driver="system"))


class Locks(unittest.TestCase):
    def test_reserved_device(self):
        class Waited(Exception):
            pass

        def wait(seconds):
            raise Waited

        with tempfile.TemporaryDirectory() as folder:
            reservation = Path(folder) / "thor.next"
            reservation.write_text("DrawDrop\n")
            asleep = device.time.sleep
            device.time.sleep = wait
            try:
                # Free, and kept for somebody else: it is waited for, not taken.
                other = device.DeviceLock(folder, "thor", "VisualQA", "test")
                with self.assertRaises(Waited):
                    other.acquire(say=lambda text: None)
                self.assertFalse((Path(folder) / "thor.lock").exists())
                # The one it is kept for takes it and removes the reservation.
                named = device.DeviceLock(folder, "thor", "DrawDrop", "test")
                named.acquire(say=lambda text: None)
                self.assertTrue((Path(folder) / "thor.lock" / "owner.txt").is_file())
                self.assertFalse(reservation.exists())
                # Held: nobody else takes it.
                with self.assertRaises(Waited):
                    other.acquire(say=lambda text: None)
                named.release()
                self.assertFalse((Path(folder) / "thor.lock").exists())
            finally:
                device.time.sleep = asleep


class Scenarios(unittest.TestCase):
    def test_scripts(self):
        for scenario in scenarios.SUITE:
            entries = scenario.input_script().split(",")
            for entry in entries:
                parts = entry.split(":")
                self.assertIn(len(parts), (2, 3), entry)
                float(parts[0])
            times = [float(entry.split(":")[0]) for entry in entries]
            self.assertLess(max(times), scenario.seconds)
            fired = [float(entry.split(":")[0]) for entry in entries if entry.split(":")[1] == "rt"]
            # The player fires in the fight alone: a shot's flash in the calm part would be taken for flicker.
            self.assertEqual(bool(fired), scenario.combat_from is not None)
            self.assertTrue(all(at >= scenario.combat_from for at in fired))
            self.assertIn(scenario.game, scenarios.DISC_IMAGES)


if __name__ == "__main__":
    unittest.main()

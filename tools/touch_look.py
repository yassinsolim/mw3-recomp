#!/usr/bin/env python3
"""Measures looking by touch on a phone: how far a swipe turns the view, and how soon.

    python tools/touch_look.py --device <serial> --apks <folder of test apps> --out <folder> cod4-campaign
        [--fps 30] [--sensitivity 13] [--aiming 50] [--suffix .tc] [--record]

It plays one of the visual QA scenarios (tools/visualqa/scenarios.py) up to
the point where the level is in play, with the on-screen controls on and
MW2_TRACE_LOOK=1, then swipes the look area with `adb shell input` -- so
many centimetres of this phone's glass, slowly, briskly and as a flick,
back and forth, up and down, and again with Aim held -- and reads from the
game's log how far the view turned each time (docs/android.md, Looking by
touch). The phone's lock is the caller's business, as is the test apps'
suffix; the app is removed afterwards.
"""
import argparse
import json
import re
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "visualqa"))
import device as phone  # noqa: E402
import scenarios  # noqa: E402

CM_PER_INCH = 2.54
LOOK = re.compile(r"look: yaw (-?[\d.]+) pitch (-?[\d.]+) added (-?[\d.]+) (-?[\d.]+) zoom ([\d.]+) waited ([\d.]+) ms")


def pixels_per_inch(target):
    """The density the phone draws at, which is what the app takes a centimetre from (TouchLook.kt)."""
    return float(re.findall(r"density: (\d+)", target.shell("wm density"))[-1])


def screen(target):
    """The screen's size in landscape, in pixels."""
    width, height = map(int, re.findall(r"(\d+)x(\d+)", target.shell("wm size"))[-1])
    return max(width, height), min(width, height)


class Log:
    """The game's log on the phone, read from where the last read ended."""

    def __init__(self, target, game):
        self.target, self.path, self.at = target, f"{target.external(game)}/game.log", 0
        self.yaw = self.pitch = None

    def more(self):
        lines = self.target.shell(f"tail -n +{self.at + 1} {self.path}").splitlines()
        self.at += len(lines)
        return lines

    def turned(self):
        """What the new lines say: degrees turned right and down, the zoom, and how long each turn waited
        between the screen reporting it and the view taking it."""
        right = down = 0.0
        zoom, waits = None, []
        for line in self.more():
            found = LOOK.search(line)
            if not found:
                continue
            yaw, pitch = float(found.group(1)), float(found.group(2))
            if self.yaw is not None:
                # Yaw counts up to the left and wraps at half a turn.
                right -= (yaw - self.yaw + 180.0) % 360.0 - 180.0
                down += pitch - self.pitch
            self.yaw, self.pitch, zoom = yaw, pitch, float(found.group(5))
            if float(found.group(6)) > 0:
                waits.append(float(found.group(6)))
        return {"right": right, "down": down, "zoom": zoom, "waits": waits}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("scenario")
    parser.add_argument("--device", required=True)
    parser.add_argument("--apks", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--suffix", default=".tc")
    parser.add_argument("--fps", type=int, default=60)
    parser.add_argument("--sensitivity", type=float, default=13.0, help="degrees a centimetre")
    parser.add_argument("--aiming", type=int, default=50, help="percent of the sensitivity while aiming")
    parser.add_argument("--record", action="store_true", help="record the screen while it swipes and fires")
    parser.add_argument("--keep", action="store_true", help="leave the app on the phone")
    arguments = parser.parse_args()

    scenario = scenarios.named(arguments.scenario)
    out = Path(arguments.out)
    out.mkdir(parents=True, exist_ok=True)
    target = phone.Device(arguments.device, arguments.suffix)
    image = target.disc_image(scenario.game, scenarios.DISC_IMAGES[scenario.game])
    test_files = scenarios.TEST_FILES.get(scenario.game)
    files = test_files or target.owner_game(scenario.game)
    if image is None or not target.has(f"{files}/default.xex"):
        raise SystemExit(f"the game ({files}) or its disc image is not on this phone")

    per_inch = pixels_per_inch(target)
    width, height = screen(target)
    per_cm = per_inch / CM_PER_INCH
    report = {"scenario": scenario.name, "device": arguments.device, "fps": arguments.fps,
              "pixels_per_inch": per_inch, "screen": [width, height], "screen_cm": round(width / per_cm, 2),
              "sensitivity": arguments.sensitivity, "aiming_percent": arguments.aiming, "steps": []}
    try:
        target.install(scenario.game, arguments.apks)
        target.provision(scenario.game, image, files, packages=test_files is not None, more_prefs={
            "touchControls": True, "fpsLimit": arguments.fps,
            "touchLookDegreesPerCm": arguments.sensitivity, "touchLookAiming": arguments.aiming / 100.0})
        switches = scenario.switches()
        # The scenario's way into the level, without the walk that follows it or the bots that would
        # shoot the player while the view is measured; the level is held open.
        switches.update({"MW2_INPUT_SCRIPT": scenario.inputs, "MW2_RUN_SECONDS": str(int(scenario.play_from) + 240),
                         "MW2_CONSOLE": re.sub(r";[\d.]+:bots \d+", "", scenario.console),
                         "MW2_TRACE_LOOK": "1"})
        clock = target.start(scenario.game, scenario.title, switches)
        clock.close()
        time.sleep(scenario.play_from + 6)
        log = Log(target, scenario.game)
        recording = None
        if arguments.record:
            recording = subprocess.Popen(
                [target.adb_path, "-s", arguments.device, "shell",
                 "screenrecord --bit-rate 8000000 --time-limit 150 /sdcard/vq_look.mp4"],
                stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            time.sleep(1)

        # The middle of the look area, clear of the buttons at its edges.
        middle_x, middle_y = int(width * 0.68), int(height * 0.5)

        def swipe(name, cm_right, cm_down, milliseconds, aiming=False):
            dx, dy = round(cm_right * per_cm), round(cm_down * per_cm)
            log.turned()
            target.shell(f"input swipe {middle_x - dx // 2} {middle_y - dy // 2} "
                         f"{middle_x + dx - dx // 2} {middle_y + dy - dy // 2} {milliseconds}")
            time.sleep(0.6)
            got = log.turned()
            factor = (arguments.aiming / 100.0 if aiming else 1.0)
            got.update({"step": name, "cm": [cm_right, cm_down], "ms": milliseconds, "aiming": aiming,
                        "asked_right": dx / per_cm * arguments.sensitivity * factor,
                        "asked_down": dy / per_cm * arguments.sensitivity * factor})
            waits = got.pop("waits")
            got["waited_ms"] = round(sum(waits) / len(waits), 2) if waits else None
            got["waited_max_ms"] = round(max(waits), 2) if waits else None
            report["steps"].append(got)
            print(f"{name:28s} asked {got['asked_right']:8.2f} {got['asked_down']:7.2f}  turned {got['right']:8.2f} "
                  f"{got['down']:7.2f}  zoom {got['zoom']}  waited {got['waited_ms']} ms")
            return got

        # A first movement, so that the log has the view's angles to count from.
        swipe("first", 0.5, 0, 200)
        swipe("first back", -0.5, 0, 200)
        for name, milliseconds in (("slow, 2 s", 2000), ("brisk, 0.3 s", 300), ("flick, 0.06 s", 60)):
            swipe(f"2 cm right, {name}", 2, 0, milliseconds)
            swipe(f"2 cm left, {name}", -2, 0, milliseconds)
        swipe("1 cm down", 0, 1, 300)
        swipe("1 cm up", 0, -1, 300)
        swipe("0.1 cm right, 1 s", 0.1, 0, 1000)
        swipe("0.1 cm left, 1 s", -0.1, 0, 1000)
        # Ten there and back: what is left over is drift.
        log.turned()
        for _ in range(10):
            target.shell(f"input swipe {middle_x - 100} {middle_y} {middle_x + 100} {middle_y + 60} 150")
            target.shell(f"input swipe {middle_x + 100} {middle_y + 60} {middle_x - 100} {middle_y} 150")
        time.sleep(0.6)
        drift = log.turned()
        report["drift"] = {"right": drift["right"], "down": drift["down"]}
        print(f"ten there and back: left over {drift['right']:.3f} right, {drift['down']:.3f} down")
        # Ten seconds with no finger down: the view stays where it is.
        time.sleep(10)
        still = log.turned()
        report["still"] = {"right": still["right"], "down": still["down"]}
        print(f"ten seconds untouched: {still['right']:.3f} right, {still['down']:.3f} down")

        # Aim is a toggle at the top left (ControlLayout.defaults): held from this tap to the next.
        aim_x, aim_y = int(width * 0.055), int(height * 0.18)
        target.shell(f"input tap {aim_x} {aim_y}")
        time.sleep(1.2)
        swipe("aiming: 2 cm right", 2, 0, 300, aiming=True)
        swipe("aiming: 2 cm left", -2, 0, 300, aiming=True)
        target.shell(f"input tap {aim_x} {aim_y}")
        time.sleep(0.8)

        # Fire held and dragged: the thumb that pressed it goes on aiming.
        fire_x, fire_y = int(width * 0.928), int(height * 0.18)
        log.turned()
        target.shell(f"input swipe {fire_x} {fire_y} {fire_x - round(2 * per_cm)} {fire_y + round(per_cm)} 1200")
        time.sleep(0.6)
        fired = log.turned()
        report["fire_drag"] = {"right": fired["right"], "down": fired["down"]}
        print(f"Fire held and dragged 2 cm left, 1 cm down: turned {fired['right']:.2f} right, {fired['down']:.2f} down")

        if recording:
            target.shell("pkill -INT screenrecord")
            recording.wait(timeout=20)
            time.sleep(1)
            target.adb("pull", "/sdcard/vq_look.mp4", str(out / "look.mp4"))
        target.stop(scenario.game)
        target.pull_log(scenario.game, out / "game.log")
    finally:
        if not arguments.keep:
            target.remove(scenario.game)
    (out / "report.json").write_text(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Checks on a phone that Invert horizontal and Invert vertical flip the right axis.

    python tools/invert_look.py --device <serial> --apks <folder of test apps> --out <folder> cod4-campaign
        [--suffix .inv] [--feed]

For each setting of the two switches (and the pad's) it plays one of the visual
QA scenarios (tools/visualqa/scenarios.py) up to the point where the level is in
play, with MW2_TRACE_LOOK=1, swipes the look area with `adb shell input` -- 2 cm
to the right, then 1 cm down -- and reads from the game's log (`look:` lines)
which way the view turned, in degrees to the right and downwards. Each setting is
a fresh run, started with the preferences that setting means, so that what is
tested is what a player's start-up reads.

With --feed, also the gyroscope and a controller's right stick, which `adb`
cannot move: a debug build whose options.txt names MW2_TEST_FEED=1 makes a
phone's turn and a stick's deflection by hand when a file called feed_now is
written next to it (`gyro right`, `gyro down`, `pad right`, `pad down`), through
the same handlers the sensor's events and a controller's motion events go to.
That hook is a local patch for measuring and is not part of the app.

The phone's lock is the caller's business, as is the test apps' suffix; the app
is removed afterwards. Exit status 1 when any axis turned the wrong way.
"""
import argparse
import json
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "visualqa"))
import device as phone  # noqa: E402
import scenarios  # noqa: E402
import touch_look  # noqa: E402

# What a setting means, as the app's preferences: the keys Prefs.kt's invertLookHorizontal,
# invertLookVertical and invertPadHorizontal write, and the one an older version wrote.
SETTINGS = [
    # name, preferences, flips right (touch, gyroscope), flips down (touch, gyroscope), flips the stick's right
    ("neither", {}, False, False, False),
    ("horizontal", {"lookInvertX": True}, True, False, False),
    ("vertical", {"lookInvertY": True}, False, True, False),
    ("both", {"lookInvertX": True, "lookInvertY": True}, True, True, False),
    ("old switch only", {"lookInvert": True}, False, True, False),
    ("controller's stick", {"padInvertX": True}, False, False, True),
]


def sign(degrees, least=1.0):
    return 0 if abs(degrees) < least else (1 if degrees > 0 else -1)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("scenario")
    parser.add_argument("--device", required=True)
    parser.add_argument("--apks", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--suffix", default=".inv")
    parser.add_argument("--fps", type=int, default=60)
    parser.add_argument("--feed", action="store_true", help="also the gyroscope and a controller's stick (a patched debug build)")
    parser.add_argument("--only", help="run only the settings with these names, separated by commas")
    parser.add_argument("--keep", action="store_true", help="leave the app on the phone")
    arguments = parser.parse_args()

    scenario = scenarios.named(arguments.scenario)
    out = Path(arguments.out)
    out.mkdir(parents=True, exist_ok=True)
    target = phone.Device(arguments.device, arguments.suffix)
    image = target.disc_image(scenario.game, scenarios.DISC_IMAGES[scenario.game])
    files = scenarios.TEST_FILES.get(scenario.game) or target.owner_game(scenario.game)
    if image is None or not target.has(f"{files}/default.xex"):
        raise SystemExit(f"the game ({files}) or its disc image is not on this phone")

    per_cm = touch_look.pixels_per_inch(target) / touch_look.CM_PER_INCH
    width, height = touch_look.screen(target)
    middle_x, middle_y = int(width * 0.68), int(height * 0.5)
    external = target.external(scenario.game)
    report = []
    wrong = []

    def swipe(log, cm_right, cm_down):
        dx, dy = round(cm_right * per_cm), round(cm_down * per_cm)
        log.turned()
        target.shell(f"input swipe {middle_x - dx // 2} {middle_y - dy // 2} "
                     f"{middle_x + dx - dx // 2} {middle_y + dy - dy // 2} 300")
        time.sleep(0.8)
        return log.turned()

    def feed(log, what, seconds):
        log.turned()
        target.shell(f"echo '{what}' > {external}/feed_now")
        time.sleep(seconds)
        return log.turned()

    try:
        target.install(scenario.game, arguments.apks)
        chosen = arguments.only.split(",") if arguments.only else None
        for name, preferences, flips_right, flips_down, flips_stick in SETTINGS:
            if chosen and name not in chosen:
                continue
            prefs = {"touchControls": True, "fpsLimit": arguments.fps, "touchLookDegreesPerCm": 13.0,
                     "touchLookAiming": 1.0, "gyroMode": 0}
            prefs.update(preferences)
            target.provision(scenario.game, image, files, more_prefs=prefs)
            switches = scenario.switches()
            switches.update({"MW2_INPUT_SCRIPT": scenario.inputs, "MW2_RUN_SECONDS": str(int(scenario.play_from) + 120),
                             "MW2_CONSOLE": re.sub(r";[\d.]+:bots \d+", "", scenario.console),
                             "MW2_TRACE_LOOK": "1"})
            if arguments.feed:
                switches["MW2_TEST_FEED"] = "1"
            clock = target.start(scenario.game, scenario.title, switches)
            clock.close()
            time.sleep(scenario.play_from + 6)
            log = touch_look.Log(target, scenario.game)
            # A first movement, so that the log has the view's angles to count from.
            swipe(log, 0.5, 0)
            swipe(log, -0.5, 0)
            row = {"setting": name}
            right = swipe(log, 2, 0)["right"]
            down = swipe(log, 0, 1)["down"]
            row["touch"] = {"right_degrees": round(right, 2), "down_degrees": round(down, 2)}
            expected = [("touch right", right, -1 if flips_right else 1), ("touch down", down, -1 if flips_down else 1)]
            if arguments.feed:
                got = feed(log, "gyro right", 2.5)["right"]
                row["gyro_right"] = round(got, 2)
                expected.append(("gyro right", got, -1 if flips_right else 1))
                got = feed(log, "gyro down", 2.5)["down"]
                row["gyro_down"] = round(got, 2)
                expected.append(("gyro down", got, -1 if flips_down else 1))
                got = feed(log, "pad right", 3.0)["right"]
                row["pad_right"] = round(got, 2)
                expected.append(("pad right", got, -1 if flips_stick else 1))
                got = feed(log, "pad down", 3.0)["down"]
                row["pad_down"] = round(got, 2)
                # The game's own invert is not in these: its option is off.
                expected.append(("pad down", got, 1))
            row["verdicts"] = {}
            for what, degrees, wanted in expected:
                verdict = "ok" if sign(degrees) == wanted else "WRONG"
                row["verdicts"][what] = f"{verdict} ({degrees:+.2f} deg, wanted {'+' if wanted > 0 else '-'})"
                if verdict != "ok":
                    wrong.append(f"{name}: {what} turned {degrees:+.2f}")
            print(f"{name:20s} " + "  ".join(f"{what}: {text}" for what, text in row["verdicts"].items()))
            report.append(row)
            target.pull_log(scenario.game, out / f"game-{name.replace(' ', '_').replace(chr(39), '')}.log")
            target.stop(scenario.game)
    finally:
        if not arguments.keep:
            target.remove(scenario.game)
        (out / "report.json").write_text(json.dumps(report, indent=2))
    if wrong:
        print("WRONG: " + "; ".join(wrong))
        sys.exit(1)
    print("every axis turned the way its switch says")


if __name__ == "__main__":
    main()

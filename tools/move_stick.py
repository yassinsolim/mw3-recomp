#!/usr/bin/env python3
"""Checks on a phone that the floating movement stick and auto-sprint do what the settings say.

    python tools/move_stick.py --device <serial> --apks <folder of test apps> --out <folder> cod4-campaign
        [--suffix .ads] [--fps 30] [--keep]

It plays one of the visual QA scenarios (tools/visualqa/scenarios.py) up to the
point where the level is in play, with the on-screen controls on and
MW2_TRACE_LOOK=1, and puts a finger on the left side of the screen with
`input motionevent` (down, move, up, so that it can stay still and be moved
back). Three records are set side by side, on the phone's uptime clock:

  * the app: the debug build's `TouchStick` line in logcat says where the stick was
    put down (pixels) when a finger came down on it,
  * the game's pad: `stick:` lines in game.log (runtime/android/input.cpp): the left
    stick's x and y (a pull of 32767 is the edge) and the click of the stick
    (l3 = sprint), when the click changes or the stick moves by an eighth,
  * pictures, taken with the finger down.

With "Floating movement stick" and "Auto-sprint" on:

  origin A     a finger down, still, in the left half where no button is: the stick is put down
               there (the picture has it under the thumb) and the pad's stick is at the centre
  origin B     a finger down somewhere else and pushed half way: a new origin, and the stick at half
  sprint       pushed past the edge and held: sprint (l3) comes a delay after the stick reaches the
               edge; pulled back to under 70% it goes up; the finger up lets go too
  short edge   the edge held for less than the delay: no sprint

and then the same touches with both off: nothing is put down, nothing moves, nothing sprints. The
phone's lock is the caller's business, as is the test apps' suffix; the app is removed afterwards.
Exit status 1 when anything is wrong.
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
import touch_look  # noqa: E402

STICK = re.compile(r"stick: lx (-?\d+) ly (-?\d+) l3 (\d) at ([\d.]+) ms")
ORIGIN = re.compile(r"TouchStick\S*:?\s+origin (-?\d+) (-?\d+) at (\d+) ms")

SPRINT_DELAY_MS = 300   # AutoSprint.DEFAULT_DELAY_MILLIS
STICK_DP = 150          # the movement stick's size in the default layout (ControlLayout.defaults)


def sticks(text):
    """(ms, x, y, l3) from the game's log: x and y as fractions of the edge."""
    return [(float(m.group(4)), int(m.group(1)) / 32767.0, int(m.group(2)) / 32767.0, int(m.group(3)))
            for m in STICK.finditer(text)]


def origins(text):
    return [(float(m.group(3)), int(m.group(1)), int(m.group(2))) for m in ORIGIN.finditer(text)]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("scenario")
    parser.add_argument("--device", required=True)
    parser.add_argument("--apks", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--suffix", default=".ads")
    parser.add_argument("--fps", type=int, default=30)
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
    width, height = touch_look.screen(target)
    radius = STICK_DP / 2 * touch_look.pixels_per_inch(target) / 160.0
    a = (int(width * 0.20), int(height * 0.55))       # in the left half, clear of the D-pad, Run and the triggers
    b = (int(width * 0.33), int(height * 0.45))
    report = {"device": arguments.device, "scenario": scenario.name, "fps": arguments.fps, "radius_px": radius,
              "a": a, "b": b, "runs": {}}
    problems = []

    def check(ok, what):
        print(("  ok   " if ok else "  FAIL ") + what)
        if not ok:
            problems.append(what)

    def motion(kind, point):
        return f"input motionevent {kind} {int(point[0])} {int(point[1])}"

    def run_touch(commands, picture_after=None):
        """The commands in one shell on the phone; a picture a while into them when asked."""
        shell = subprocess.Popen(
            [target.adb_path, "-s", arguments.device, "shell", "; ".join(commands)],
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if picture_after:
            seconds, name = picture_after
            time.sleep(seconds)
            target.shell("screencap -p /sdcard/move_stick.png")
            target.adb("pull", "/sdcard/move_stick.png", str(out / name))
            target.shell("rm /sdcard/move_stick.png")
        shell.wait(timeout=60)
        time.sleep(1.0)

    def right_of(point, radii):
        return (point[0] + radii * radius, point[1])

    for setting_on in (True, False):
        name = "on" if setting_on else "off"
        print(f"\n== Floating stick and auto-sprint {name} ==")
        target.install(scenario.game, arguments.apks)
        target.provision(scenario.game, image, files, more_prefs={
            "touchControls": True, "fpsLimit": arguments.fps, "floatingStick": setting_on, "autoSprint": setting_on,
            "touchLookDegreesPerCm": 13.0, "gyroMode": 0, "tapToFire": False})
        switches = scenario.switches()
        switches.update({"MW2_INPUT_SCRIPT": scenario.inputs, "MW2_RUN_SECONDS": str(int(scenario.play_from) + 120),
                         "MW2_CONSOLE": re.sub(r";[\d.]+:bots \d+", "", scenario.console), "MW2_TRACE_LOOK": "1"})
        target.start(scenario.game, scenario.title, switches).close()
        time.sleep(scenario.play_from + 6)
        target.adb("logcat", "-c")
        time.sleep(0.5)

        # origin A: a finger down and still.
        run_touch([motion("DOWN", a), "sleep 2", motion("UP", a)], picture_after=(1.2, f"origin-a-{name}.png"))
        # origin B: down, then pushed half way and held.
        run_touch([motion("DOWN", b), "sleep 0.3", motion("MOVE", right_of(b, 0.5)), "sleep 2",
                   motion("UP", right_of(b, 0.5))], picture_after=(1.6, f"origin-b-{name}.png"))
        # sprint: to past the edge and held, then back to 40% and held, then up.
        run_touch([motion("DOWN", a), "sleep 0.2", motion("MOVE", right_of(a, 0.6)), "sleep 0.2",
                   motion("MOVE", right_of(a, 1.3)), "sleep 2", motion("MOVE", right_of(a, 0.4)), "sleep 0.8",
                   motion("UP", right_of(a, 0.4))], picture_after=(1.4, f"sprint-{name}.png"))
        # a short edge: held for a tenth of a second only.
        run_touch([motion("DOWN", a), "sleep 0.2", motion("MOVE", right_of(a, 1.3)), "sleep 0.1",
                   motion("MOVE", right_of(a, 0.3)), "sleep 0.6", motion("UP", right_of(a, 0.3))])
        touch_text = target.adb("logcat", "-d", "-s", "TouchStick:I")
        (out / f"touch-{name}.txt").write_text(touch_text)
        target.stop(scenario.game)
        log = out / f"game-{name}.log"
        target.pull_log(scenario.game, log)

        game_text = log.read_text(errors="replace")
        lines = sticks(game_text)
        starts = origins(touch_text)
        report["runs"][name] = {"origins": starts, "stick_lines": lines}
        print(f"origins put down: {starts}; the game saw {len(lines)} stick changes")

        if not setting_on:
            check(not starts, "off: no stick is put down")
            check(not lines, "off: the pad's stick never moves and sprint is never pressed")
        else:
            check(len(starts) == 4, f"on: four fingers put a stick down ({len(starts)})")
            if len(starts) >= 4:
                (_, ax, ay), (_, bx, by) = starts[0], starts[1]
                check(abs(ax - a[0]) <= 3 and abs(ay - a[1]) <= 3, f"origin A is where the finger came down {a} -> {(ax, ay)}")
                check(abs(bx - b[0]) <= 3 and abs(by - b[1]) <= 3, f"origin B is where the next finger came down {b} -> {(bx, by)}")
                check((ax, ay) != (bx, by), "the origin moved with the touch")
                ends = [starts[i + 1][0] if i + 1 < len(starts) else starts[i][0] + 3000 for i in range(len(starts))]
                windows = [[e for e in lines if starts[i][0] - 5 <= e[0] < ends[i]] for i in range(len(starts))]
                # B: pushed half way: the pad's stick is at about a half to the right.
                half = [e for e in windows[1] if 0.4 <= e[1] <= 0.6]
                check(bool(half), f"push of half a radius from the origin is half on the pad: {[round(e[1], 2) for e in windows[1]]}")
                # Sprint.
                sprint = windows[2]
                edge = next((e for e in sprint if e[1] >= 0.9), None)
                click = next((e for e in sprint if e[3] == 1), None)
                release = next((e for e in sprint if click and e[0] > click[0] and e[3] == 0), None)
                check(bool(edge), "sprint: the stick reaches the edge on the pad")
                check(bool(click), "sprint: the stick's click is pressed")
                if edge and click:
                    gap = click[0] - edge[0]
                    report["runs"][name]["sprint_after_edge_ms"] = gap
                    print(f"  edge at {edge[0] - starts[2][0]:.0f} ms, sprint pressed {gap:.0f} ms after the edge, "
                          f"let go {release[0] - click[0]:.0f} ms later" if release else f"  sprint pressed {gap:.0f} ms after the edge")
                    check(SPRINT_DELAY_MS - 150 <= gap <= SPRINT_DELAY_MS + 150,
                          f"sprint: pressed {gap:.0f} ms after the edge (delay {SPRINT_DELAY_MS}, a frame is {1000 // arguments.fps})")
                check(bool(release), "sprint: let go when pulled back below the way back")
                if release:
                    back = [e for e in sprint if e[0] <= release[0]][-1]
                    check(abs(back[1]) < 0.8, f"sprint: let go with the stick at {back[1]:.2f}")
                # Short edge.
                check(not any(e[3] for e in windows[3]), "short edge: no sprint")

        if not arguments.keep:
            target.remove(scenario.game)

    (out / "report.json").write_text(json.dumps(report, indent=1))
    print("\nproblems:" if problems else "\nall checks passed", *problems, sep="\n  ")
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Checks on a phone that "Aim when firing (touch)" holds the triggers in the right order.

    python tools/aim_fire.py --device <serial> --apks <folder of test apps> --out <folder> cod4-campaign
        [--suffix .ads] [--fps 30] [--keep]

It plays one of the visual QA scenarios (tools/visualqa/scenarios.py) up to the
point where the level is in play, with the on-screen controls on and
MW2_TRACE_LOOK=1, then presses the Fire button with `adb shell input` and sets
three clocks side by side, all of them the phone's uptime:

  * the touch screen's events (`getevent -lt`: when the finger went down and came up),
  * the game's own reading of the pad (`trigger:` lines in game.log, written by
    android/input.cpp when a trigger's state changes: lt, rt, at ... ms),
  * the view (`look:` lines, with the sight's zoom factor and when the frame was made).

The runs, each with the setting on (and Aim Fire held down for the first):

  hold         Fire held for 1 s: LT first, RT one delay later, both let go together
  tap          Fire down and up inside the delay: the shot still goes out, for its minimum
  aiming       Aim toggled on first: Fire is a plain RT and Aim stays on afterwards
  drag         Fire held and dragged: the zoom factor against time says how long the sights take

and one run with the setting off (Fire is a plain RT, LT never moves). A
picture is taken with Fire held. The phone's lock is the caller's business, as
is the test apps' suffix; the app is removed afterwards. Exit status 1 when an
order or a time is wrong.
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

TRIGGER = re.compile(r"trigger: lt (\d) rt (\d) at ([\d.]+) ms")
LOOK = re.compile(r"look: .* zoom ([\d.]+) waited [\d.]+ ms at ([\d.]+) ms")
TOUCH = re.compile(r"\[\s*([\d.]+)\].*BTN_TOUCH\s+(DOWN|UP)")

DELAY_MS = 150     # AimWhenFiring.DEFAULT_DELAY_MILLIS
MIN_SHOT_MS = 70   # AimWhenFiring.MIN_SHOT_MILLIS


def trigger_edges(text):
    """(ms, lt, rt) for each change of the triggers the game's poll saw."""
    return [(float(m.group(3)), int(m.group(1)), int(m.group(2))) for m in TRIGGER.finditer(text)]


def zoom_samples(text):
    return [(float(m.group(2)), float(m.group(1))) for m in LOOK.finditer(text)]


def touches(text):
    """(ms, 'DOWN' | 'UP') from getevent -lt, whose clock is the phone's uptime in seconds."""
    return [(float(m.group(1)) * 1000.0, m.group(2)) for m in TOUCH.finditer(text)]


def around(edges, start, end):
    """The trigger edges from `start` to `end`, as (ms from start, lt, rt)."""
    return [(round(ms - start, 1), lt, rt) for ms, lt, rt in edges if start - 5 <= ms <= end]


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
    fire_x, fire_y = int(width * 0.928), int(height * 0.18)
    aim_x, aim_y = int(width * 0.055), int(height * 0.18)
    report = {"device": arguments.device, "scenario": scenario.name, "fps": arguments.fps, "runs": {}}
    problems = []

    def check(ok, what):
        print(("  ok   " if ok else "  FAIL ") + what)
        if not ok:
            problems.append(what)

    def play(setting_on):
        """One run of the game; the log of the touch screen and the game's, as text."""
        target.provision(scenario.game, image, files, more_prefs={
            "touchControls": True, "fpsLimit": arguments.fps, "aimWhenFiring": setting_on,
            "touchLookDegreesPerCm": 13.0, "touchLookAiming": 1.0, "gyroMode": 0, "tapToFire": False})
        switches = scenario.switches()
        switches.update({"MW2_INPUT_SCRIPT": scenario.inputs, "MW2_RUN_SECONDS": str(int(scenario.play_from) + 150),
                         "MW2_CONSOLE": re.sub(r";[\d.]+:bots \d+", "", scenario.console), "MW2_TRACE_LOOK": "1"})
        target.start(scenario.game, scenario.title, switches).close()
        time.sleep(scenario.play_from + 6)

    def hold_fire(milliseconds, picture=None):
        shell = subprocess.Popen(
            [target.adb_path, "-s", arguments.device, "shell",
             f"input swipe {fire_x} {fire_y} {fire_x} {fire_y} {milliseconds}"],
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if picture:
            time.sleep(min(milliseconds / 1000.0 * 0.7, 1.2) + 0.4)
            target.shell(f"screencap -p /sdcard/aim_fire.png")
            target.adb("pull", "/sdcard/aim_fire.png", str(out / picture))
            target.shell("rm /sdcard/aim_fire.png")
        shell.wait(timeout=60)
        time.sleep(0.8)

    def drag_fire(milliseconds):
        target.shell(f"input swipe {fire_x} {fire_y} {fire_x - 90} {fire_y + 30} {milliseconds}")
        time.sleep(0.8)

    for setting_on in (True, False):
        name = "on" if setting_on else "off"
        print(f"\n== Aim when firing {name} ==")
        target.install(scenario.game, arguments.apks)
        play(setting_on)
        getevent = open(out / f"getevent-{name}.txt", "w")
        watcher = subprocess.Popen([target.adb_path, "-s", arguments.device, "shell", "getevent -lt"],
                                   stdin=subprocess.DEVNULL, stdout=getevent, stderr=subprocess.DEVNULL)
        time.sleep(1.5)
        marks = {}

        def mark(step):
            marks[step] = time.monotonic()

        steps = []
        # (name, action) in the order they are done; each is read from the logs afterwards.
        steps.append(("hold", lambda: hold_fire(1000, picture=f"hold-{name}.png")))
        steps.append(("tap", lambda: (target.shell(f"input swipe {fire_x} {fire_y} {fire_x} {fire_y} 30"), time.sleep(1.0))))
        if setting_on:
            def aiming():
                target.shell(f"input tap {aim_x} {aim_y}")
                time.sleep(1.0)
                hold_fire(1000, picture=f"aiming-{name}.png")
                target.shell(f"input tap {aim_x} {aim_y}")
                time.sleep(1.0)
            steps.append(("aiming", aiming))
            steps.append(("drag", lambda: drag_fire(2500)))
        for step, action in steps:
            action()
            time.sleep(0.3)
        watcher.terminate()
        getevent.close()
        target.stop(scenario.game)
        log = out / f"game-{name}.log"
        target.pull_log(scenario.game, log)

        game_text = log.read_text(errors="replace")
        edges = trigger_edges(game_text)
        events = touches((out / f"getevent-{name}.txt").read_text(errors="replace"))
        downs = [ms for ms, kind in events if kind == "DOWN"]
        ups = [ms for ms, kind in events if kind == "UP"]
        report["runs"][name] = {"touch_down": downs, "touch_up": ups, "trigger_edges": edges}
        print(f"touches: {len(downs)} down, {len(ups)} up; the game saw {len(edges)} trigger changes")

        # Touch pairs in the order they were made: hold, tap, [aim, fire, aim, drag].
        def pair(index):
            return (downs[index], ups[index]) if index < min(len(downs), len(ups)) else (None, None)

        sequences = {"hold": 0, "tap": 1}
        if setting_on:
            sequences.update({"aim on": 2, "aiming: fire": 3, "aim off": 4, "drag": 5})
        for label, index in sequences.items():
            down, up = pair(index)
            if down is None:
                check(False, f"{label}: no touch was seen")
                continue
            seen = around(edges, down, up + 600)
            print(f"{label}: down 0, up {up - down:.0f} ms; (ms after down, lt, rt) {seen}")
            report["runs"][name][label] = {"up_ms": up - down, "edges": seen}
            if label == "hold":
                if setting_on:
                    lt_on = next((e for e in seen if e[1] == 1), None)
                    rt_on = next((e for e in seen if e[2] == 1), None)
                    off = next((e for e in seen if e[0] > 0 and e[1] == 0 and e[2] == 0), None)
                    check(bool(lt_on and rt_on and lt_on[0] <= rt_on[0]), "hold: LT is pressed before or with RT")
                    if lt_on and rt_on:
                        gap = rt_on[0] - lt_on[0]
                        report["runs"][name]["hold"]["lt_to_rt_ms"] = gap
                        report["runs"][name]["hold"]["down_to_rt_ms"] = rt_on[0]
                        print(f"  LT {lt_on[0]} ms, RT {rt_on[0]} ms after the finger: delay to first shot {rt_on[0]:.0f} ms")
                        check(DELAY_MS - 40 <= gap <= DELAY_MS + 110, f"hold: RT {gap:.0f} ms after LT (delay {DELAY_MS})")
                    check(bool(off) and off[0] >= (up - down) - 5, "hold: both let go together after the finger came up")
                else:
                    check(not any(e[1] for e in seen) and any(e[2] for e in seen), "off: Fire is RT alone")
            elif label == "tap" and setting_on:
                rt_on = next((e for e in seen if e[2] == 1), None)
                off = next((e for e in seen if e[0] > 0 and e[1] == 0 and e[2] == 0), None)
                check(bool(rt_on), "tap: a tap shorter than the delay still shoots")
                check(bool(off) and rt_on is not None and off[0] > rt_on[0], "tap: both let go after the shot")
            elif label == "aiming: fire":
                rt_on = next((e for e in seen if e[2] == 1), None)
                check(bool(rt_on) and rt_on[0] <= 100, "aiming: RT goes down at once with Aim held")
                check(all(e[1] == 1 for e in seen), "aiming: LT stays as the player's Aim holds it")

        if setting_on:
            down, up = pair(5)
            if down is not None:
                samples = [(round(ms - down, 1), z) for ms, z in zoom_samples(game_text) if down - 5 <= ms <= up + 400]
                lt = next((e for e in around(edges, down, up + 600) if e[1] == 1), None)
                report["runs"][name]["zoom_after_down"] = samples
                print(f"drag: sight zoom factor over time (ms after finger down): {samples[:40]}")
                if lt and samples:
                    first = samples[0][1]
                    settled = samples[-1][1]
                    done = next((ms for ms, z in samples if abs(z - settled) < 0.002 * max(1.0, settled)), None)
                    report["runs"][name]["sights_ms"] = {"lt_at": lt[0], "zoom_from": first, "zoom_to": settled,
                                                          "settled_at": done}
                    print(f"  LT at {lt[0]} ms; zoom {first} -> {settled}, settled at {done} ms")
        if not arguments.keep:
            target.remove(scenario.game)
        else:
            target.stop(scenario.game)

    (out / "report.json").write_text(json.dumps(report, indent=1))
    print("\nproblems:" if problems else "\nall checks passed", *problems, sep="\n  ")
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()

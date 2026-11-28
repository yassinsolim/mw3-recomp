#!/usr/bin/env python3
"""Checks on a phone that "Aim when firing (touch)" holds the triggers in the right order.

    python tools/aim_fire.py --device <serial> --apks <folder of test apps> --out <folder> cod4-campaign
        [--suffix .ads] [--fps 30] [--keep]

It plays one of the visual QA scenarios (tools/visualqa/scenarios.py) up to the
point where the level is in play, with the on-screen controls on and
MW2_TRACE_LOOK=1, then presses the on-screen buttons with `adb shell input` and
sets three records side by side, all of them on the phone's uptime clock:

  * the finger: the debug build's `TouchTrigger` lines in logcat say when a
    trigger's finger came down and up (the time of the touch event itself),
  * the game's own reading of the pad: `trigger:` lines in game.log, written by
    runtime/android/input.cpp when a trigger's state changes (lt, rt, at ... ms),
  * the view: `look:` lines, with the sight's zoom factor and when the frame was made.

Runs, with the setting on:

  hold         Fire held for 1 s: LT first, RT one delay later, both let go together
  tap          Fire down and up inside the delay: the shot still goes out, for its minimum
  aiming       Aim toggled on first: Fire is a plain RT and Aim stays on afterwards
  drag         Fire held and dragged: the zoom factor against time says how long the sights take

and then the same two first with the setting off (Fire is a plain RT, LT never
moves). Pictures are taken with Fire held. The game polls the pad once a frame,
so every time the game saw is a frame late at most (33 ms at 30 frames a
second). The phone's lock is the caller's business, as is the test apps'
suffix; the app is removed afterwards. Exit status 1 when an order or a time is
wrong.
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

TRIGGER = re.compile(r"trigger: lt (\d) rt (\d) at ([\d.]+) ms")
LOOK = re.compile(r"look: .* zoom ([\d.]+) waited [\d.]+ ms at ([\d.]+) ms")
TOUCH = re.compile(r"TouchTrigger\S*:?\s+(lt|rt) (down|up) at (\d+) ms")

DELAY_MS = 200     # AimWhenFiring.DEFAULT_DELAY_MILLIS
MIN_SHOT_MS = 70   # AimWhenFiring.MIN_SHOT_MILLIS


def trigger_edges(text):
    """(ms, lt, rt) for each change of the triggers the game's poll saw."""
    return [(float(m.group(3)), int(m.group(1)), int(m.group(2))) for m in TRIGGER.finditer(text)]


def zoom_samples(text):
    return [(float(m.group(2)), float(m.group(1))) for m in LOOK.finditer(text)]


def fingers(text):
    """{'lt': [(down, up)...], 'rt': [...]} in ms: the times of the touch events on the trigger buttons."""
    open_at, pairs = {}, {"lt": [], "rt": []}
    for m in TOUCH.finditer(text):
        which, what, ms = m.group(1), m.group(2), float(m.group(3))
        if what == "down":
            open_at[which] = ms
        elif which in open_at:
            pairs[which].append((open_at.pop(which), ms))
    return pairs


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
    reload_x, reload_y = int(width * 0.828), int(height * 0.63)   # X
    report = {"device": arguments.device, "scenario": scenario.name, "fps": arguments.fps, "runs": {}}
    problems = []

    def check(ok, what):
        print(("  ok   " if ok else "  FAIL ") + what)
        if not ok:
            problems.append(what)

    def play(setting_on):
        target.provision(scenario.game, image, files, more_prefs={
            "touchControls": True, "fpsLimit": arguments.fps, "aimWhenFiring": setting_on,
            "touchLookDegreesPerCm": 13.0, "touchLookAiming": 1.0, "gyroMode": 0, "tapToFire": False})
        switches = scenario.switches()
        switches.update({"MW2_INPUT_SCRIPT": scenario.inputs, "MW2_RUN_SECONDS": str(int(scenario.play_from) + 150),
                         "MW2_CONSOLE": re.sub(r";[\d.]+:bots \d+", "", scenario.console), "MW2_TRACE_LOOK": "1"})
        target.start(scenario.game, scenario.title, switches).close()
        time.sleep(scenario.play_from + 6)

    def hold_fire(milliseconds, picture=None):
        # The shell is left running so that a picture can be taken while the finger is down.
        import subprocess
        shell = subprocess.Popen(
            [target.adb_path, "-s", arguments.device, "shell",
             f"input swipe {fire_x} {fire_y} {fire_x} {fire_y} {milliseconds}"],
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if picture:
            time.sleep(0.9)
            target.shell("screencap -p /sdcard/aim_fire.png")
            target.adb("pull", "/sdcard/aim_fire.png", str(out / picture))
            target.shell("rm /sdcard/aim_fire.png")
        shell.wait(timeout=60)
        time.sleep(0.8)

    def reload():
        # A clip is empty after a second of fire; the title reloads on X, not by itself.
        target.shell(f"input tap {reload_x} {reload_y}")
        time.sleep(4.0)

    for setting_on in (True, False):
        name = "on" if setting_on else "off"
        print(f"\n== Aim when firing {name} ==")
        target.install(scenario.game, arguments.apks)
        play(setting_on)
        target.adb("logcat", "-c")
        time.sleep(0.5)

        if setting_on:
            # First, with a full clip: a quick drag from Fire, so that the view moves from the first
            # frames and the sight's zoom factor can be read against time; up before the clip is empty.
            target.shell(f"input swipe {fire_x} {fire_y} {fire_x - 330} {fire_y + 60} 700")
            time.sleep(0.8)
            reload()
        hold_fire(1000, picture=f"hold-{name}.png")
        reload()
        target.shell(f"input swipe {fire_x} {fire_y} {fire_x} {fire_y} 30")   # a tap, inside the delay
        time.sleep(1.0)
        if setting_on:
            reload()
            target.shell(f"input tap {aim_x} {aim_y}")                       # Aim on (a toggle)
            time.sleep(1.0)
            hold_fire(1000, picture=f"aiming-{name}.png")                    # Fire with Aim already on
            target.shell(f"input tap {aim_x} {aim_y}")                       # Aim off
            time.sleep(1.0)
        touch_text = target.adb("logcat", "-d", "-s", "TouchTrigger:I")
        (out / f"touch-{name}.txt").write_text(touch_text)
        target.stop(scenario.game)
        log = out / f"game-{name}.log"
        target.pull_log(scenario.game, log)

        game_text = log.read_text(errors="replace")
        edges = trigger_edges(game_text)
        pairs = fingers(touch_text)
        report["runs"][name] = {"fingers": pairs, "trigger_edges": edges}
        print(f"fingers on Fire: {len(pairs['rt'])}, on Aim: {len(pairs['lt'])}; the game saw {len(edges)} trigger changes")

        fire = pairs["rt"]
        wanted = 4 if setting_on else 2
        check(len(fire) >= wanted, f"{name}: {wanted} presses of Fire were seen ({len(fire)})")
        labels = ["drag", "hold", "tap", "aiming: fire"] if setting_on else ["hold", "tap"]
        for label, (down, up) in zip(labels, fire):
            seen = around(edges, down, up + 600)
            print(f"{label}: finger down at 0, up at {up - down:.0f} ms; game saw (ms after down, lt, rt): {seen}")
            report["runs"][name][label] = {"up_ms": up - down, "edges": seen}
            lt_on = next((e for e in seen if e[1] == 1), None)
            rt_on = next((e for e in seen if e[2] == 1), None)
            off = next((e for e in seen if e[0] > 0 and e[2] == 0 and (e[1] == 0 or label == "aiming: fire")), None)
            if not setting_on:
                if label == "hold":
                    check(lt_on is None and rt_on is not None, "off: Fire is RT alone")
                continue
            if label == "hold" or label == "drag":
                check(bool(lt_on and rt_on and lt_on[0] <= rt_on[0]), f"{label}: LT is pressed before or with RT")
                if lt_on and rt_on:
                    gap = rt_on[0] - lt_on[0]
                    report["runs"][name][label].update({"lt_ms": lt_on[0], "rt_ms": rt_on[0], "lt_to_rt_ms": gap})
                    print(f"  LT {lt_on[0]:.0f} ms and RT {rt_on[0]:.0f} ms after the finger: first shot {rt_on[0]:.0f} ms after the touch")
                    check(DELAY_MS - 60 <= gap <= DELAY_MS + 110, f"{label}: RT {gap:.0f} ms after LT (delay {DELAY_MS}, one frame is {1000 // arguments.fps})")
                released = next((e for e in seen if e[0] > 0 and e[1] == 0 and e[2] == 0), None)
                check(bool(released) and released[0] >= (up - down) - 5, f"{label}: both let go together after the finger came up")
            elif label == "tap":
                released = next((e for e in seen if e[0] > 0 and e[1] == 0 and e[2] == 0), None)
                check(bool(rt_on), "tap: a tap shorter than the delay still shoots")
                check(bool(released) and rt_on is not None and released[0] > rt_on[0], "tap: both let go after the shot")
            elif label == "aiming: fire":
                check(bool(rt_on) and rt_on[0] <= 60, "aiming: RT goes down at once with Aim held")
                check(all(e[1] == 1 for e in seen), "aiming: LT stays as the player's Aim holds it")
                check(bool(off), "aiming: RT lets go with the finger")

        if setting_on and len(fire) >= 4:
            down, up = fire[0]
            # Within the press: the clip lasts about 0.8 s after the shot starts, so the later samples are a reload's.
            samples = [(round(ms - down, 1), z) for ms, z in zoom_samples(game_text) if down - 5 <= ms <= up]
            lt = next((e for e in around(edges, down, up + 600) if e[1] == 1), None)
            report["runs"][name]["zoom_after_down"] = samples
            print(f"drag: sight zoom factor over time (ms after the finger): {samples[:60]}")
            if lt and samples:
                settled = min(z for _, z in samples)
                first = next((ms for ms, z in samples if z < 0.999), None)
                done = next((ms for ms, z in samples if abs(z - settled) < 0.002 * max(1.0, settled)), None)
                report["runs"][name]["sights"] = {"lt_ms": lt[0], "zoom_hip": samples[0][1], "zoom_aiming": settled,
                                                  "zoom_starts_ms": first, "settled_ms": done}
                print(f"  LT at {lt[0]} ms; zoom {samples[0][1]} -> {settled}: starts {first} ms, settled {done} ms after the finger")
        if not arguments.keep:
            target.remove(scenario.game)

    (out / "report.json").write_text(json.dumps(report, indent=1))
    print("\nproblems:" if problems else "\nall checks passed", *problems, sep="\n  ")
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()

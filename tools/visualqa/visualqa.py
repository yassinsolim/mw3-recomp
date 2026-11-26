#!/usr/bin/env python3
"""Records a run of the game and checks every frame of it.

    visualqa.py suite --device thor --device flip --apks <folder>   every standard scenario (also: --suite)
    visualqa.py run <scenario> --device flip --apks <folder>        one of them
    visualqa.py windows <scenario> -- <game command>                the same from a Windows build's own frames
    visualqa.py analyse <recording or folder of frames>... [--log game.log]
    visualqa.py calibrate [calibration.json] --root <folder>        precision and recall on the labelled set
    visualqa.py list

It prints PASS or FAIL for each run with the faults found and where their
pictures were saved, and exits 1 when any run failed. docs/visualqa.md has
the checks, their numbers and how they were measured.

Needs Python 3.10, numpy and opencv-python (`pip install --user numpy opencv-python`).
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))

import cv2  # noqa: E402
import numpy as np  # noqa: E402

import detectors  # noqa: E402
import device as phone  # noqa: E402
import frames  # noqa: E402
import gamelog  # noqa: E402
import scenarios  # noqa: E402

# The most pictures kept for one run: a run that fails on every frame is
# explained by its first few.
MOST_PICTURES = 24
PICTURE_WIDTH = 640


def say(text):
    print(text, flush=True)


def save_pictures(source, findings, folder, stem):
    """Writes each finding's frame between the frames before and after it; returns {frame: file}."""
    wanted = [finding for finding in findings if finding.frame >= 0]
    # The failures first, then by time, so the limit drops remarks before faults.
    wanted.sort(key=lambda finding: (finding.severity != detectors.FAIL, finding.seconds))
    wanted = wanted[:MOST_PICTURES]
    rows = set()
    for finding in wanted:
        rows.update((finding.frame - 1, finding.frame, finding.frame + 1))
    pictures = frames.pictures_at(source, [row for row in rows if row >= 0])
    saved = {}
    folder.mkdir(parents=True, exist_ok=True)
    for finding in wanted:
        strip = []
        for row in (finding.frame - 1, finding.frame, finding.frame + 1):
            picture = pictures.get(row)
            if picture is None:
                continue
            height = int(picture.shape[0] * PICTURE_WIDTH / picture.shape[1])
            strip.append(cv2.resize(picture, (PICTURE_WIDTH, height), interpolation=cv2.INTER_AREA))
        if not strip:
            continue
        sheet = np.hstack(strip)
        cv2.putText(sheet, f"{finding.check} at {finding.seconds:.1f} s: before, the frame, after", (8, 22),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 255, 255), 2)
        path = folder / f"{stem}_{finding.check}_{finding.seconds:07.2f}s_f{finding.frame}.jpg"
        cv2.imwrite(str(path), sheet, [cv2.IMWRITE_JPEG_QUALITY, 80])
        saved[finding.frame] = path
    return saved


def measured(source, mask, cache, times=None):
    """A source's measurements, read from beside the report when they were taken before."""
    if cache and cache.is_file() and cache.stat().st_mtime >= Path(source).stat().st_mtime:
        return frames.Measurements.load(cache)
    began = time.monotonic()
    taken = frames.measure(frames.frames_of(source, times), mask)
    if cache:
        cache.parent.mkdir(parents=True, exist_ok=True)
        taken.save(cache)
    say(f"  measured {len(taken)} frames of {Path(source).name} in {time.monotonic() - began:.0f} s")
    return taken


def examine(recordings, out, log=None, dumps=None, mask="hud", assume_play=True, combat_from=None, play_from=0.0,
            expect_driver=None, thresholds=None, by_hand=False):
    """Checks a run's recordings (file, seconds of the run at its start), frames and log; returns the report."""
    thresholds = thresholds or detectors.Thresholds()
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    # Everything before the scenario's play is a menu or a loading screen by its own account.
    exempt = ((0.0, play_from),) if play_from else ()
    findings = []
    pictures = {}
    covered = -1.0
    for index, (recording, offset) in enumerate(recordings):
        # A folder of the game's own frames is placed by the log's lines of when each was written.
        times = log.dump_seconds if log is not None and Path(recording).is_dir() else None
        m = measured(recording, mask, out / f"measurements_{index}.npz", times)
        context = detectors.Context(log=log, offset=offset, assume_play=assume_play, combat_from=combat_from,
                                    exempt=exempt)
        found = detectors.from_pictures(m, context, thresholds)
        # Segments overlap by a few seconds; what the one before saw is not said twice.
        found = [finding for finding in found if finding.seconds > covered]
        if len(m):
            covered = offset + float(m.seconds[-1])
        saved = save_pictures(recording, found, out / "pictures", f"rec{index}")
        for finding in found:
            findings.append(finding)
            if finding.frame in saved:
                pictures[id(finding)] = saved[finding.frame]
    if dumps:
        # The game's own frames are exact and bare of the encoder, but seconds
        # apart: they answer the checks one picture can.
        m = measured(dumps, mask, out / "measurements_frames.npz", log.dump_seconds if log else None)
        context = detectors.Context(log=log, assume_play=assume_play, exempt=exempt)
        found = detectors.blocks(m, context, thresholds) + detectors.blank(m, context, thresholds)
        seen = {(finding.check, int(finding.seconds)) for finding in findings}
        found = [finding for finding in found if (finding.check, int(finding.seconds)) not in seen]
        saved = save_pictures(dumps, found, out / "pictures", "frame")
        for finding in found:
            finding.text += " (in the game's own frame)"
            findings.append(finding)
            if finding.frame in saved:
                pictures[id(finding)] = saved[finding.frame]
    if log is not None:
        findings += detectors.from_log(log, thresholds, expect_driver, exempt, by_hand)
    findings.sort(key=lambda finding: (finding.severity != detectors.FAIL, finding.seconds))
    report = {
        "verdict": detectors.verdict(findings),
        "findings": [dict(finding.as_dict(), picture=str(pictures.get(id(finding), ""))) for finding in findings],
        # What a recording kept for the labelled set needs to be checked again as it was here.
        "recordings": [{"file": Path(recording).name, "offset": round(offset, 2)} for recording, offset in recordings],
        "play_from": play_from, "combat_from": combat_from,
    }
    if log is not None:
        rates = [row.frames for row in log.seconds]
        report["log"] = {
            "driver": log.driver, "graphics": log.graphics, "finished": log.finished,
            "seconds_in_play": len(rates), "in_play_at": log.in_play_at,
            "frames_a_second": round(sum(rates) / len(rates), 1) if rates else 0,
            "longest_frame_ms": max((row.longest_ms for row in log.seconds), default=0),
            "stutters": len(log.stutters), "stalls": len(log.stalls),
        }
    return report


def write_report(report, out, name):
    out = Path(out)
    lines = [f"{report['verdict']}  {name}"]
    facts = report.get("log")
    if facts:
        lines.append(f"  log: {facts['frames_a_second']} frames a second over {facts['seconds_in_play']} s of play, "
                     f"the longest frame {facts['longest_frame_ms']} ms, driver {facts['driver'] or 'not said'}")
    for finding in report["findings"]:
        where = f"  -> {finding['picture']}" if finding["picture"] else ""
        lines.append(f"  {finding['severity']:4s} {finding['check']:13s} {finding['seconds']:7.1f} s  {finding['text']}{where}")
    if len(report["findings"]) == 0:
        lines.append("  nothing found")
    text = "\n".join(lines)
    (out / "report.txt").write_text(text + "\n", encoding="utf-8")
    (out / "report.json").write_text(json.dumps(dict(report, name=name), indent=1), encoding="utf-8")
    say(text)
    return text


# ---- runs on a phone


def record_on_device(scenario, name, serial, arguments):
    """One scenario on one phone, recorded; returns what checks it (a call), or its report when it was not run."""
    out = Path(arguments.out) / name / scenario.name
    out.mkdir(parents=True, exist_ok=True)
    target = phone.Device(serial, arguments.suffix)
    image = target.disc_image(scenario.game, scenarios.DISC_IMAGES[scenario.game])
    if image is None or not target.has(f"{target.owner_game(scenario.game)}/default.xex"):
        say(f"SKIP  {name}/{scenario.name}: the game or its disc image is not on this phone")
        return {"verdict": "SKIP", "findings": [], "reason": "the game is not on this phone"}
    level, heat = target.battery()
    if level < arguments.battery_floor:
        raise SystemExit(f"{name}: battery at {level}%: charge it before a run")
    say(f"{name}/{scenario.name}: {scenario.what} ({scenario.seconds} s; battery {level}%, {heat:.0f} C)")
    try:
        target.install(scenario.game, arguments.apks)
        target.provision(scenario.game, image, scenario.whole_game)
        switches = scenario.switches(f"{target.external(scenario.game)}/vqframes", arguments.dump_every)
        if scenario.whole_game:
            # The install disc's packages are read where the owner's app keeps them.
            switches.setdefault("MW2_DLC_DIR", f"/sdcard/Android/data/com.{scenario.game}.recomp/files/dlc")
        clock = target.start(scenario.game, scenario.title, switches)
        try:
            # The menus before play are not recorded: three minutes of a segment are better spent on the level.
            record_from = max(0.0, scenario.play_from - arguments.lead)
            time.sleep(max(0.0, record_from - 4.0))
            segments = target.record(scenario.seconds - record_from + 2, out, "recording")
            deadline = time.monotonic() + 45
            while target.running(scenario.game) and time.monotonic() < deadline:
                time.sleep(2)
        finally:
            clock.close()
        target.stop(scenario.game)
        log_file = target.pull_log(scenario.game, out / "game.log")
        dumps = target.pull_dumps(scenario.game, out / "frames") if arguments.dump_every else None
    finally:
        if not arguments.keep:
            target.remove(scenario.game)
    nought = clock.nought()
    if nought is None:
        raise RuntimeError("no line of the phone's log named a second of the run: the recording cannot be placed in it")

    def check():
        report = examine([(path, began - nought) for path, began in segments], out, log=gamelog.read(log_file),
                         dumps=dumps, mask="hud", combat_from=scenario.combat_from, play_from=scenario.play_from,
                         expect_driver=arguments.driver)
        write_report(report, out, f"{name}/{scenario.name}")
        return report

    return check


def command_suite(arguments, chosen=None):
    chosen = chosen or scenarios.SUITE
    devices = arguments.device or list(scenarios.DEVICES)
    reports = {}
    # A recording is checked while the next scenario plays: the phone's slot
    # is short and the checking needs nothing of the phone.
    with ThreadPoolExecutor(max_workers=1) as checking:
        for name in devices:
            if name not in scenarios.DEVICES:
                raise SystemExit(f"no device {name}: one of {', '.join(scenarios.DEVICES)}")
            lock = phone.DeviceLock(arguments.lock_dir, name, arguments.owner, "visualqa suite")
            with lock.held(say):
                for scenario in chosen:
                    if name not in scenario.devices and not arguments.any_device:
                        continue
                    # Install, the run, the pull and the removal, before the slot is over.
                    lock.room_for(scenario.seconds + 150, say=say)
                    try:
                        recorded = record_on_device(scenario, name, scenarios.DEVICES[name], arguments)
                    except (RuntimeError, subprocess.TimeoutExpired) as error:
                        say(f"FAIL  {name}/{scenario.name}: {error}")
                        recorded = {"verdict": "FAIL", "findings": [], "reason": str(error)}
                    reports[f"{name}/{scenario.name}"] = checking.submit(recorded) if callable(recorded) else recorded
        reports = {name: report if isinstance(report, dict) else report.result() for name, report in reports.items()}
    summary = Path(arguments.out) / f"suite_{'_'.join(devices)}.json"
    summary.write_text(json.dumps(reports, indent=1), encoding="utf-8")
    say("")
    for name, report in reports.items():
        faults = sorted({finding["check"] for finding in report["findings"] if finding["severity"] == detectors.FAIL})
        say(f"{report['verdict']:4s}  {name}  {', '.join(faults) or report.get('reason', '')}")
    failed = [name for name, report in reports.items() if report["verdict"] == "FAIL"]
    say(f"{len(reports) - len(failed)} of {len(reports)} runs without a failure; reports in {arguments.out}")
    return 1 if failed else 0


def command_run(arguments):
    return command_suite(arguments, [scenarios.named(arguments.scenario)])


# ---- a Windows build


def command_windows(arguments):
    """Runs a Windows build through a scenario and checks the frames it writes itself."""
    scenario = scenarios.named(arguments.scenario)
    out = Path(arguments.out).resolve() / "windows" / scenario.name
    dumped = out / "frames"
    dumped.mkdir(parents=True, exist_ok=True)
    for old in dumped.iterdir():
        old.unlink()
    log_file = out / "game.log"
    environment = dict(os.environ)
    environment.update(scenario.switches(str(dumped), arguments.dump_every))
    environment["MW2_LOG_FILE"] = str(log_file)
    # Each line with its time: the frames are placed in the run by the lines that say they were written.
    environment["MW2_LOG_TIME"] = "1"
    if not arguments.game:
        raise SystemExit("give the game's command after --: build-win\\mw2.exe mw2\\default.pe mw2\\game")
    say(f"windows/{scenario.name}: {scenario.what} ({scenario.seconds} s)")
    subprocess.run(arguments.game, env=environment, cwd=arguments.cwd, timeout=scenario.seconds + 180, check=False)
    log = gamelog.read(log_file) if log_file.is_file() else None
    # The frames are the recording here: a tenth of a second apart, so a
    # flash of one frame is seen one time in six and a patch only in a view
    # that stands, but what is seen is exact.
    report = examine([(dumped, 0.0)], out, log=log, mask="hud", play_from=scenario.play_from,
                     combat_from=scenario.combat_from)
    write_report(report, out, f"windows/{scenario.name}")
    # A thousand frames are gigabytes; the pictures of the faults are kept.
    shutil.rmtree(dumped)
    return 1 if report["verdict"] == "FAIL" else 0


# ---- recordings already made


def command_analyse(arguments):
    failed = False
    for source in arguments.source:
        source = Path(source)
        out = Path(arguments.out) / "analyse" / source.stem
        log = gamelog.read(arguments.log) if arguments.log else None
        # A recording made by hand has menus and fights in it, and without a
        # log that has a line a second nothing says where: its single-frame
        # patches are then remarked on, unless it is said to be calm play.
        placed = arguments.play or arguments.menu or (log is not None and bool(log.seconds))
        report = examine([(source, arguments.offset)], out, log=log, mask=arguments.mask,
                         assume_play=not arguments.menu, combat_from=None if placed else 0.0,
                         expect_driver=arguments.driver if log else None, by_hand=True)
        write_report(report, out, str(source))
        failed |= report["verdict"] == "FAIL"
    return 1 if failed else 0


def command_calibrate(arguments):
    """Runs the checks over the labelled set and prints which recordings they got right."""
    manifest = json.loads(Path(arguments.manifest).read_text(encoding="utf-8"))
    root = Path(arguments.root)
    counts = {"caught": 0, "missed": 0, "passed": 0, "false alarm": 0}
    absent = 0
    thresholds = detectors.Thresholds()
    for entry in manifest["recordings"]:
        source = root / entry["file"]
        if not source.exists():
            absent += 1
            say(f"  absent       {entry['file']}")
            continue
        cache = Path(arguments.out) / "calibrate" / (entry["file"].replace("/", "_") + f".{entry.get('mask', 'hud')}.npz")
        log = gamelog.read(root / entry["log"]) if entry.get("log") else None
        m = measured(source, entry.get("mask", "hud"), cache)
        play_from = entry.get("play_from", 0.0)
        context = detectors.Context(log=log, assume_play=not entry.get("menu", False), offset=entry.get("offset", 0.0),
                                    combat_from=entry.get("combat_from"),
                                    exempt=((0.0, play_from),) if play_from else ())
        if source.is_file() and source.suffix.lower() in (".png", ".jpg"):
            # One picture cannot show a fault lasting several frames: that the check sees it is its pass.
            failures = sorted({finding.check for finding in detectors.blocks(m, context, thresholds)})
        else:
            found = detectors.from_pictures(m, context, thresholds)
            failures = sorted({finding.check for finding in found if finding.severity == detectors.FAIL})
        wanted = entry.get("faults", [])
        if wanted:
            outcome = "caught" if set(wanted) & set(failures) else "missed"
        else:
            outcome = "false alarm" if failures else "passed"
        counts[outcome] += 1
        say(f"  {outcome:12s} {entry['file']}: labelled {', '.join(wanted) or 'clean'}; found {', '.join(failures) or 'nothing'}")
    bad = counts["caught"] + counts["missed"]
    flagged = counts["caught"] + counts["false alarm"]
    say(f"{counts['caught']} of {bad} faulty recordings caught, {counts['passed']} of "
        f"{counts['passed'] + counts['false alarm']} clean ones passed"
        + (f", {absent} not found under {root}" if absent else ""))
    say(f"recall {counts['caught'] / bad:.2f}, precision {counts['caught'] / flagged:.2f}" if bad and flagged else
        "no recall or precision: no faulty recording was found")
    return 1 if counts["missed"] or counts["false alarm"] else 0


def command_list(arguments):
    for scenario in scenarios.SUITE:
        say(f"{scenario.name:14s} {scenario.seconds:4d} s  {', '.join(scenario.devices):10s}  {scenario.what}")
    return 0


def main():
    here = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", default=os.environ.get("VISUALQA_OUT", "visualqa-out"),
                        help="where reports, recordings and pictures go (never inside the repository's tracked files)")
    commands = parser.add_subparsers(dest="command", required=True)

    def device_options(command):
        command.add_argument("--device", action="append", help="thor or flip; both when not given")
        command.add_argument("--apks", default=os.environ.get("VISUALQA_APKS", "."),
                             help="the folder of <game>.vq.apk and <game>-game.vq.apk")
        command.add_argument("--suffix", default=".vq", help="the test app's package suffix")
        command.add_argument("--lock-dir", default=os.environ.get("VISUALQA_LOCK_DIR"),
                             help="the folder of device locks; a run without one must say --no-lock")
        command.add_argument("--no-lock", action="store_true", help="the phone is nobody else's")
        command.add_argument("--owner", default=os.environ.get("VISUALQA_OWNER", os.environ.get("USERNAME", "visualqa")),
                             help="the name written into the lock")
        command.add_argument("--driver", choices=("bundled", "system"), default="bundled",
                             help="the Vulkan driver a run must have used: a run on another one fails")
        command.add_argument("--dump-every", type=int, default=240,
                             help="the game writes every so many of its frames itself as well (0: none)")
        command.add_argument("--lead", type=float, default=12.0, help="seconds recorded before play")
        command.add_argument("--battery-floor", type=int, default=20)
        command.add_argument("--keep", action="store_true", help="leave the test app on the phone after the run")
        command.add_argument("--any-device", action="store_true", help="also on a device the scenario is not listed for")

    suite = commands.add_parser("suite", help="every standard scenario")
    device_options(suite)
    run = commands.add_parser("run", help="one scenario")
    run.add_argument("scenario")
    device_options(run)
    windows = commands.add_parser("windows", help="one scenario on a Windows build")
    windows.add_argument("scenario")
    windows.add_argument("--cwd", default=".")
    # Every frame's picture would be exact and the run a slide show; one in
    # six keeps a flash of one frame visible in one run of six.
    windows.add_argument("--dump-every", type=int, default=6, help="the game writes every so many of its frames")
    windows.add_argument("game", nargs="*", help="after --: the game's command")
    analyse = commands.add_parser("analyse", help="recordings already made")
    analyse.add_argument("source", nargs="+")
    analyse.add_argument("--log")
    analyse.add_argument("--offset", type=float, default=0.0, help="seconds of the run at the recording's first frame")
    analyse.add_argument("--mask", choices=sorted(frames.MASKS), default="touch")
    analyse.add_argument("--menu", action="store_true", help="the recording is of menus: the play-only checks are left out")
    analyse.add_argument("--play", action="store_true",
                         help="the recording is of calm play throughout: a patch that differs for one frame is a fault")
    analyse.add_argument("--driver", choices=("bundled", "system"), default=None)
    calibrate = commands.add_parser("calibrate", help="precision and recall on the labelled set")
    calibrate.add_argument("manifest", nargs="?", default=str(here / "calibration.json"))
    calibrate.add_argument("--root", default=os.environ.get("VISUALQA_CALIBRATION", "."))
    commands.add_parser("list", help="the scenarios")

    # `--suite` reads as the flag the release notes name it by.
    argv = ["suite" if argument == "--suite" else argument for argument in sys.argv[1:]]
    arguments = parser.parse_args(argv)
    if arguments.command in ("suite", "run") and not arguments.lock_dir and not arguments.no_lock:
        raise SystemExit("name the folder of device locks (--lock-dir or VISUALQA_LOCK_DIR), or say --no-lock")
    handler = {"suite": command_suite, "run": command_run, "windows": command_windows, "analyse": command_analyse,
               "calibrate": command_calibrate, "list": command_list}[arguments.command]
    return handler(arguments)


if __name__ == "__main__":
    sys.exit(main())

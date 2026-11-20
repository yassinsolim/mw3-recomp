#!/usr/bin/env python3
"""Every translated shader, through Arm's Mali Offline Compiler.

No Mali device is on the bench, and the Android Emulator draws with the host's
GPU, so the one Mali compiler that can be run here is malioc (Arm Performance
Studio, free): the shader compiler of the Mali driver built for the PC. It
says whether a SPIR-V module compiles for a given Mali core, and what it
costs there -- cycles in each pipeline, work registers, and whether values
were spilled to the stack. It does not draw anything.

    malioc_shaders.py --translate <translate-shader> --malioc <malioc>
                      [--cores Mali-G57,Mali-G715,...] [--variants ...]
                      [--work <dir>] [--report <file.json>] <ucode dir or file>...

The microcode is what MW2_DUMP_SHADERS and zone-shaders --dump write
(vertex_<hash>.ucode / pixel_<hash>.ucode). Each is translated the ways the
runtime gives a device (pipeline.cpp, CreateDevice):

    vertex            the one vertex module
    subgroup-arena    pixel: one record a subgroup, float constants in the arena
    subgroup-uniform  pixel: one record a subgroup, float constants a uniform block
    pixel-uniform     pixel: a record a pixel (no fragment subgroup ballot),
                      float constants a uniform block

The report counts, for each core and variant: the modules malioc refused (each
named with malioc's own message), the ones whose work registers halve the
shader core's thread occupancy, the ones that spill to the stack, and the
cycles of the pipeline that bounds them. The exit status is 1 when malioc
refused any module.
"""
import argparse
import concurrent.futures
import json
import os
import statistics
import subprocess
import sys

VARIANTS = {
    "vertex": [],
    "subgroup-arena": [],
    "subgroup-uniform": ["--uniform-constants"],
    "pixel-uniform": ["--pixel-records", "--uniform-constants"],
}
DEFAULT_CORES = "Mali-G57,Mali-G78,Mali-G715,Immortalis-G925"


def is_pixel(path):
    name = os.path.basename(path)
    return "pixel" in name or name.startswith("ps_")


def translate(tool, ucode, variant, work):
    """The module's path, or the translator's reason for not making one."""
    stem = os.path.splitext(os.path.basename(ucode))[0]
    stage = "vert" if variant == "vertex" else "frag"
    out = os.path.join(work, variant, f"{stem}.{stage}.spv")
    if os.path.exists(out) and os.path.getsize(out):
        return out, None
    done = subprocess.run([tool, *VARIANTS[variant], ucode, out], capture_output=True, text=True)
    if done.returncode != 0 or not os.path.exists(out):
        text = (done.stdout + done.stderr).strip().splitlines()
        reason = text[-1] if text else f"exit {done.returncode}"
        # The line starts with the file's name, which is different for every shader.
        marker = "not translated:"
        if marker in reason:
            reason = reason[reason.index(marker) + len(marker):].strip()
        return None, reason
    return out, None


def compile_one(malioc, core, module):
    done = subprocess.run([malioc, "--vulkan", "--core", core, "--format", "json", module],
                          capture_output=True, text=True)
    try:
        report = json.loads(done.stdout)
    except json.JSONDecodeError:
        report = None
    if done.returncode != 0 or not report or report.get("schema", {}).get("name") != "performance":
        lines = [line.strip() for line in (done.stdout + done.stderr).splitlines() if line.strip()]
        errors = [line for line in lines if "error" in line.lower()]
        # malioc puts the reason on the line after "ERROR:".
        for index, line in enumerate(lines):
            if line.upper().startswith("ERROR") and index + 1 < len(lines):
                errors = [lines[index + 1]]
                break
        if report and report.get("schema", {}).get("name") == "error":
            errors = [error for shader in report.get("shaders", []) for error in shader.get("errors", [])] or errors
        return {"error": (errors[0] if errors else f"exit {done.returncode}")[:300]}
    shader = report["shaders"][0]
    if not shader.get("variants"):
        return {"error": json.dumps(shader.get("errors", shader.get("notes", "no variant")))[:300]}
    main = shader["variants"][0]
    properties = {p["name"]: p["value"] for p in main["properties"]}
    total = main["performance"]["total_cycles"]
    cycles = [c for c in total["cycle_count"] if c is not None]
    return {
        "driver": shader["driver"],
        "work_registers": properties.get("work_registers_used"),
        "occupancy": properties.get("thread_occupancy"),
        "uniform_registers": properties.get("uniform_registers_used"),
        "spill_bytes": properties.get("stack_spill_bytes", 0) if properties.get("has_stack_spilling") else 0,
        "bound": (total["bound_pipelines"] or [None])[0],
        "cycles": max(cycles) if cycles else None,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("inputs", nargs="+", help="folders of .ucode files, or files")
    parser.add_argument("--translate", required=True, help="the translate-shader executable")
    parser.add_argument("--malioc", required=True, help="the malioc executable")
    parser.add_argument("--cores", default=DEFAULT_CORES, help=f"comma-separated malioc cores ({DEFAULT_CORES})")
    parser.add_argument("--variants", default=",".join(VARIANTS), help="comma-separated, of: " + ", ".join(VARIANTS))
    parser.add_argument("--work", default="malioc-work", help="where the SPIR-V modules are written")
    parser.add_argument("--report", help="every module's numbers, as JSON")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    args = parser.parse_args()

    cores = [c for c in args.cores.split(",") if c]
    variants = [v for v in args.variants.split(",") if v]
    for variant in variants:
        if variant not in VARIANTS:
            parser.error(f"no variant {variant}")
        os.makedirs(os.path.join(args.work, variant), exist_ok=True)

    ucodes = []
    for entry in args.inputs:
        if os.path.isdir(entry):
            ucodes += sorted(os.path.join(entry, name) for name in os.listdir(entry) if name.endswith(".ucode"))
        else:
            ucodes.append(entry)
    if not ucodes:
        parser.error("no .ucode files")

    wanted = [(ucode, variant) for ucode in ucodes for variant in variants
              if (variant == "vertex") != is_pixel(ucode)]
    modules = {}       # (ucode, variant) -> module path
    untranslated = {}  # variant -> reason -> count
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        made = pool.map(lambda job: translate(args.translate, job[0], job[1], args.work), wanted)
        for (ucode, variant), (module, reason) in zip(wanted, made):
            if module:
                modules[(ucode, variant)] = module
            else:
                reasons = untranslated.setdefault(variant, {})
                reasons[reason] = reasons.get(reason, 0) + 1

    jobs = [(key, core) for key in modules for core in cores]
    results = {}       # variant -> core -> [(ucode, numbers)]
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        compiled = pool.map(lambda job: compile_one(args.malioc, job[1], modules[job[0]]), jobs)
        for ((ucode, variant), core), numbers in zip(jobs, compiled):
            results.setdefault(variant, {}).setdefault(core, []).append((os.path.basename(ucode), numbers))

    refused = 0
    print(f"{len(ucodes)} programs, {len(modules)} modules, {len(cores)} cores")
    for variant in variants:
        for reason, count in sorted(untranslated.get(variant, {}).items(), key=lambda item: -item[1]):
            print(f"  {variant}: {count} not translated ({reason})")
    print(f"{'variant':17} {'core':16} {'driver':13} {'modules':>7} {'refused':>7} {'half occupancy':>14}"
          f" {'spill':>6} {'cycles median':>13} {'p95':>7} {'max':>7}  bound by")
    for variant in variants:
        for core in cores:
            rows = results.get(variant, {}).get(core, [])
            good = [numbers for _, numbers in rows if "error" not in numbers]
            bad = [(name, numbers["error"]) for name, numbers in rows if "error" in numbers]
            refused += len(bad)
            if not rows:
                continue
            cycles = sorted(n["cycles"] for n in good if n["cycles"] is not None)
            bounds = {}
            for n in good:
                bounds[n["bound"]] = bounds.get(n["bound"], 0) + 1
            bound = ", ".join(f"{name} {count}" for name, count in sorted(bounds.items(), key=lambda item: -item[1]))
            print(f"{variant:17} {core:16} {good[0]['driver'] if good else '-':13} {len(rows):7} {len(bad):7}"
                  f" {sum(1 for n in good if (n['occupancy'] or 100) < 100):14}"
                  f" {sum(1 for n in good if n['spill_bytes']):6}"
                  f" {statistics.median(cycles) if cycles else 0:13.1f}"
                  f" {cycles[int(len(cycles) * 0.95)] if cycles else 0:7.1f}"
                  f" {cycles[-1] if cycles else 0:7.1f}  {bound}")
            # A core that lacks a capability refuses every module that names it:
            # each message once, with how many modules and the first of them.
            messages = {}
            for name, error in bad:
                messages.setdefault(error, []).append(name)
            for error, names in sorted(messages.items(), key=lambda item: -len(item[1])):
                print(f"    refused {len(names)} ({names[0]}{', ...' if len(names) > 1 else ''}): {error}")

    if args.report:
        with open(args.report, "w", encoding="utf-8", newline="\n") as out:
            json.dump({"cores": cores, "untranslated": untranslated,
                       "results": {variant: {core: dict(rows) for core, rows in by_core.items()}
                                   for variant, by_core in results.items()}}, out)
    return 1 if refused else 0


if __name__ == "__main__":
    sys.exit(main())

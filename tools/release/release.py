#!/usr/bin/env python3
"""The release of the four Android apps, from the private branch to published (docs/android-releases.md).

    python tools/release/release.py status
    python tools/release/release.py sync [--push]
    python tools/release/release.py tag
    python tools/release/release.py gates [--wait]
    python tools/release/release.py device-template
    python tools/release/release.py publish --device-report <report.json>
    python tools/release/release.py verify-public

Each step refuses to go on when a gate fails and says which. Nothing is
published by anything but `publish`, which runs the gates again on the drafts
as they are then and wants the report of the device gates (5 and 6), which a
person or an agent fills after trying the apps on the devices.

  sync     filters the private branch into the four public mirrors' histories
           (the paths docs/android-releases.md names), checks each for game
           files, for a blob over 20 MB and for anything of a game that is not
           released, and that the public main is an ancestor of the result;
           --push then pushes main, never forced.
  tag      pushes the next version's tag to each mirror: its workflow builds
           the app and drafts the release.
  gates    1 CI: every workflow on the mirror's main and the tag's release
             workflow ended well.
           2 tools/check_runtime_apk.py on the draft's APK; its certificate is
             the release key's; its package and version are the tag's.
           3 the Turnip package (android/fetch_deps.sh's SHA-256) and
             libadrenotools' libraries are inside the APK.
           4 every statement the published app took is taken by the draft, or
             the notes say in their first lines that the game code is built
             again; the toolchain release setup/toolchain.json names exists
             with that SHA-256 (publish makes it public before the apps).
           7 the notes (docs/releases/<game>-<tag>.txt): plain text, under
             1,450 characters, with a line on known issues.
  publish  toolchain first when it is still a draft, then the games, then
           verify-public: the download addresses answer with the drafts'
           bytes and each repository's latest release is the new tag.

Where things are: RELEASE_WORK (or --work) is the folder holding the mirrors'
clones (public-hist, mw2-public, cod4-public, aw-public; made when missing),
the Android SDK (toolchain/android-sdk) and out/release, where the drafts'
files and the reports go. Needs git, git-filter-repo and gh, logged in.
"""
import argparse
import hashlib
import io
import json
import os
import re
import subprocess
import sys
import time
import urllib.request
import zipfile
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPOSITORY / "tools"))
import check_runtime_apk  # noqa: E402

OWNER = "yassinsolim"
RELEASE_CERTIFICATE_SHA256 = "3aad5c3b3e4817007c659eb0b03e4480184aff631d13e0badbaa15f47ab36e06"
NOTES_LIMIT = 1450
LARGEST_BLOB = 20 * 1024 * 1024

# game: the mirror's clone, the README that becomes its README.md, the release workflow, the package, its titles.
GAMES = {
    "mw3": ("public-hist", "README.md", "android-release.yml", "com.mw3.recomp", ("mw3", "mw3mp")),
    "mw2": ("mw2-public", "README-MW2.md", "android-release-mw2.yml", "com.mw2.recomp", ("mw2", "mw2mp")),
    "cod4": ("cod4-public", "README-COD4.md", "android-release-cod4.yml", "com.cod4.recomp", ("cod4", "cod4mp")),
    "aw": ("aw-public", "README-AW.md", "android-release-aw.yml", "com.aw.recomp", ("aw", "awmp", "awmp17")),
}
READMES = [game[1] for game in GAMES.values()]

# What the private history has and the public ones must not (docs/android-releases.md).
DROPPED_FOLDERS = ["ppc/", "iw4L-cpp/", "build-tests/", "pgo/", "android/toolchain/build/",
                   "android/toolchain-probe/build/", "aw/"]
DROPPED_FILES = [
    "android/app/src/main/assets/shader_cache.bin",
    "android/app/src/main/res/drawable/banner_mw2.png",
    "android/app/src/main/res/drawable/ic_launcher_foreground.png",
    *[f"android/app/src/main/res/mipmap-{density}/ic_launcher{shape}.png"
      for density in ("hdpi", "mdpi", "xhdpi", "xxhdpi", "xxxhdpi") for shape in ("", "_round")],
    "docs/HOMELAB-RELAY-REQUEST.md", "docs/images/multiplayer-menu.png",
    "docs/images/readme/multiplayer-menu.jpg", "docs/images/readme/split-screen.jpg",
]
DROPPED_GLOBS = ["ppc_aw*"]
# Two earlier launcher icons, wherever they were in the tree.
DROPPED_BLOBS = ["1f2bbd4247228de79269766ed3f5850630b3955d", "acdce15999039b8a20bb8008717f0a126ea1d5d0"]
# The public copy of this private commit has one more newline at its message's end.
MESSAGE_WITH_NEWLINE = "be9b1af"
# No path of a game that is not released may be in a mirror, whatever merged into the branch.
UNRELEASED = re.compile(r"(?i)(^|[/_.-])bo3([/_.-]|$)")
GAME_PATH = re.compile(r"(?i)\.(xex|xexp|ff|ffm|iwd|iwi|bik|iso|god|pak|apk|aab|so|pe)$|^(aw|cod4|mw2|mw3|ppc[^/]*)/")
ADRENOTOOLS_LIBRARIES = ("libhook_impl.so", "libmain_hook.so", "libfile_redirect_hook.so", "libgsl_alloc_hook.so")
DEVICES = ("thor", "flip")
DEVICE_CHECKS = ("campaign", "multiplayer", "visual", "turnip", "fps")


class Refused(Exception):
    """A gate that did not pass."""


def run(*command, cwd=None, check=True, stdin=None):
    done = subprocess.run([str(part) for part in command], cwd=cwd, input=stdin, capture_output=True)
    out = (done.stdout + done.stderr).decode("utf-8", "replace").strip()
    if check and done.returncode != 0:
        raise Refused(f"{' '.join(str(part) for part in command[:6])} ...: {out[-1500:]}")
    return out


def gh_json(*command):
    return json.loads(run("gh", *command))


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as file:
        for block in iter(lambda: file.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def version_code(tag):
    major, minor, patch = (int(part) for part in tag[1:].split("."))
    if minor > 99 or patch > 99:
        raise Refused(f"{tag}: minor and patch must be below 100")
    return major * 10000 + minor * 100 + patch


class Release:
    def __init__(self, args):
        work = args.work or os.environ.get("RELEASE_WORK")
        if not work:
            raise Refused("RELEASE_WORK (or --work) has to name the folder with the mirrors' clones")
        self.work = Path(work)
        self.out = self.work / "out" / "release"
        self.out.mkdir(parents=True, exist_ok=True)
        self.branch = args.branch
        self.games = [game for game in GAMES if game in args.games.split(",")]
        self.state_path = self.out / "state.json"
        self.state = json.loads(self.state_path.read_text()) if self.state_path.exists() else {}
        self.asked = dict(pair.split("=") for pair in args.version)
        sdk = Path(os.environ.get("ANDROID_HOME") or self.work / "toolchain" / "android-sdk")
        tools = sorted((sdk / "build-tools").iterdir())[-1]
        self.aapt2 = tools / ("aapt2.exe" if os.name == "nt" else "aapt2")
        self.apksigner = tools / ("apksigner.bat" if os.name == "nt" else "apksigner")

    def save(self):
        self.state_path.write_text(json.dumps(self.state, indent=2))

    def repo(self, game):
        return f"{OWNER}/{game}-recomp"

    def clone(self, game):
        path = self.work / GAMES[game][0]
        if not path.exists():
            run("git", "clone", "-o", "public", f"https://github.com/{self.repo(game)}.git", path)
        return path

    # ---- what is published and what comes next ------------------------------
    def published(self, game):
        """The latest published version's tag (the camo packs and the build tools are pre-releases)."""
        return gh_json("api", f"repos/{self.repo(game)}/releases/latest")["tag_name"]

    def next_tag(self, game):
        if game in self.asked:
            return self.asked[game]
        if game in self.state.get("tags", {}):
            return self.state["tags"][game]
        major, minor, patch = (int(part) for part in self.published(game)[1:].split("."))
        return f"v{major}.{minor}.{patch + 1}"

    def status(self):
        print("private", self.branch, run("git", "rev-parse", "--short", self.branch, cwd=REPOSITORY))
        for game in self.games:
            was = self.state.get("published", {}).get(game) or self.published(game)
            main = gh_json("api", f"repos/{self.repo(game)}/commits/main")["sha"][:9]
            print(f"{game}: published {was}, next {self.next_tag(game)}, public main {main}")
        print("state:", json.dumps(self.state, indent=2))

    # ---- the mirrors --------------------------------------------------------
    def history_problems(self, clone, extra=()):
        """Game files, dropped paths, unreleased games and large blobs anywhere in refilter's history."""
        paths = set(filter(None, run("git", "log", "--format=", "--name-only", "-m", "--no-renames", "refilter",
                                     cwd=clone).splitlines()))
        bad = sorted(path for path in paths if path.startswith(tuple(DROPPED_FOLDERS)) or path in DROPPED_FILES
                     or path in extra or GAME_PATH.search(path) or UNRELEASED.search(path))
        objects = subprocess.run(["git", "rev-list", "--objects", "refilter"], cwd=clone, capture_output=True).stdout
        sizes = subprocess.run(["git", "cat-file", "--batch-check=%(objecttype) %(objectsize) %(objectname) %(rest)"],
                               cwd=clone, input=objects, capture_output=True).stdout.decode("utf-8", "replace")
        largest = 0
        for line in sizes.splitlines():
            kind, size, name, *rest = line.split(" ", 3)
            if kind != "blob":
                continue
            largest = max(largest, int(size))
            if int(size) > LARGEST_BLOB:
                bad.append(f"{rest[0] if rest else name}: a blob of {int(size) >> 20} MB")
            if name in DROPPED_BLOBS:
                bad.append(f"{name}: a blob that is dropped")
        return bad, largest

    def sync(self, push):
        private = run("git", "rev-parse", self.branch, cwd=REPOSITORY)
        base = self.clone("mw3")
        for game in self.games:
            if run("git", "status", "--porcelain", cwd=self.clone(game)):
                raise Refused(f"{self.clone(game)} has changes of its own")
        # The filter all four share, in Modern Warfare 3's clone.
        run("git", "fetch", "--no-tags", REPOSITORY, f"+{private}:refs/heads/private-android", cwd=base)
        run("git", "checkout", "-q", "-B", "refilter", "private-android", cwd=base)
        paths = []
        for path in DROPPED_FOLDERS + DROPPED_FILES:
            paths += ["--path", path]
        for glob in DROPPED_GLOBS:
            paths += ["--path-glob", glob]
        commit = run("git", "rev-parse", MESSAGE_WITH_NEWLINE, cwd=REPOSITORY)
        callback = (f'if commit.original_id == b"{commit}": '
                    'commit.message = commit.message.rstrip(b"\\n") + b"\\n\\n"')
        run("git", "filter-repo", "--force", "--refs", "refilter", "--invert-paths", *paths,
            "--commit-callback", callback, cwd=base)
        blobs = self.out / "dropped-blobs.txt"
        blobs.write_text("\n".join(DROPPED_BLOBS) + "\n")
        run("git", "filter-repo", "--force", "--refs", "refilter", "--strip-blobs-with-ids", blobs, cwd=base)
        shared = run("git", "rev-parse", "refilter", cwd=base)
        mirrors = {}
        for game in self.games:
            clone, readme = self.clone(game), GAMES[game][1]
            run("git", "fetch", "-q", "public", "--prune", cwd=clone)
            public = run("git", "rev-parse", "public/main", cwd=clone)
            extra = ()
            if game != "mw3":
                run("git", "fetch", "--no-tags", base, "refilter", cwd=clone)
                run("git", "checkout", "-q", "-B", "refilter", shared, cwd=clone)
                others = []
                for other in READMES:
                    if other != readme:
                        others += ["--path", other]
                run("git", "filter-repo", "--force", "--refs", "refilter", "--invert-paths", *others,
                    "--path-rename", f"{readme}:README.md", cwd=clone)
                extra = tuple(other for other in READMES if other != "README.md")
            new = run("git", "rev-parse", "refilter", cwd=clone)
            if subprocess.run(["git", "merge-base", "--is-ancestor", public, "refilter"], cwd=clone).returncode:
                raise Refused(f"{game}: public main {public[:9]} is not an ancestor of the filtered branch: "
                              "pushing it would rewrite public history")
            run(sys.executable, "tools/check_no_game_files.py", cwd=clone)
            bad, largest = self.history_problems(clone, extra)
            if bad:
                raise Refused(f"{game}: in the filtered history: {bad[:20]}")
            added = run("git", "rev-list", "--count", f"{public}..refilter", cwd=clone)
            print(f"{game}: {public[:9]} -> {new[:9]}, {added} commits, fast-forward, no game files, "
                  f"largest blob {largest >> 10} KB")
            mirrors[game] = new
        self.state.update(private=private, mirrors={**self.state.get("mirrors", {}), **mirrors})
        self.save()
        if push:
            for game in self.games:
                print(game, run("git", "push", "public", "refilter:main", cwd=self.clone(game)).splitlines()[-1])

    def tag(self):
        self.state.setdefault("published", {})
        self.state.setdefault("tags", {})
        for game in self.games:
            clone, tag = self.clone(game), self.next_tag(game)
            head = self.state.get("mirrors", {}).get(game)
            main = gh_json("api", f"repos/{self.repo(game)}/commits/main")["sha"]
            if head != main:
                raise Refused(f"{game}: public main is {main[:9]}, not the synced {str(head)[:9]}: sync --push first")
            was = self.published(game)
            if version_code(tag) <= version_code(was):
                raise Refused(f"{game}: {tag} is not above the published {was}")
            if run("git", "ls-remote", "--tags", "public", tag, cwd=clone):
                raise Refused(f"{game}: {tag} is already a tag in the public repository")
            run("git", "tag", "-f", tag, head, cwd=clone)
            run("git", "push", "public", f"refs/tags/{tag}", cwd=clone)
            self.state["published"][game], self.state["tags"][game] = was, tag
            self.save()
            print(f"{game}: {tag} at {head[:9]} (published: {was})")

    # ---- the gates ----------------------------------------------------------
    def wait_for_runs(self, game, sha, minutes):
        """Every workflow run for the commit, once none is running."""
        until = time.time() + minutes * 60
        while True:
            runs = gh_json("run", "list", "-R", self.repo(game), "--commit", sha, "-L", "30",
                           "--json", "name,status,conclusion,headBranch,workflowName,url")
            if runs and all(one["status"] == "completed" for one in runs):
                return runs
            if time.time() > until:
                return runs
            time.sleep(30)

    def gate_ci(self, game, tag, wait):
        sha = self.state["mirrors"][game]
        runs = self.wait_for_runs(game, sha, 100 if wait else 0)
        unfinished = [one["workflowName"] for one in runs if one["status"] != "completed"]
        if unfinished:
            raise Refused(f"gate 1, {game}: still running: {unfinished}")
        failed = [f"{one['workflowName']} on {one['headBranch']}: {one['conclusion']} {one['url']}"
                  for one in runs if one["conclusion"] not in ("success", "skipped")]
        if failed:
            raise Refused(f"gate 1, {game}: {failed}")
        names = {(one["workflowName"], one["headBranch"]) for one in runs if one["conclusion"] == "success"}
        workflow = run("git", "show", f"{sha}:.github/workflows/{GAMES[game][2]}", cwd=self.clone(game))
        release_name = re.search(r"(?m)^name: (.+)$", workflow).group(1).strip()
        if (release_name, tag) not in names:
            raise Refused(f"gate 1, {game}: no successful run of '{release_name}' for {tag}")
        if not any(branch == "main" for _, branch in names):
            raise Refused(f"gate 1, {game}: no successful run on main for {sha[:9]}")
        return sorted(f"{name} ({branch})" for name, branch in names)

    def draft(self, game, tag):
        """The release's assets, downloaded; (its record, the APK, its statements)."""
        release = gh_json("release", "view", tag, "-R", self.repo(game),
                          "--json", "isDraft,isPrerelease,assets,body,tagName,url")
        folder = self.out / "drafts" / f"{game}-{tag}"
        folder.mkdir(parents=True, exist_ok=True)
        run("gh", "release", "download", tag, "-R", self.repo(game), "-D", folder, "--clobber")
        apk = folder / f"{game}-recomp-{tag}.apk"
        abi = folder / f"{game}-recomp-{tag}-game-abi.txt"
        if not apk.exists() or not abi.exists():
            raise Refused(f"{game}: the release {tag} has no {apk.name} or no {abi.name}")
        return release, apk, abi

    def gate_apk(self, game, tag, apk):
        found = list(check_runtime_apk.problems(str(apk)))
        if found:
            raise Refused(f"gate 2, {game}: {found}")
        environment = dict(os.environ)
        java = self.work / "mw3-android" / ".toolchain" / "jdk17"
        if "JAVA_HOME" not in environment and java.exists():
            environment["JAVA_HOME"] = str(java)
        certs = subprocess.run([str(self.apksigner), "verify", "--print-certs", str(apk)], capture_output=True,
                               text=True, env=environment, shell=os.name == "nt")
        signed = re.search(r"Signer #1 certificate SHA-256 digest: (\w+)", certs.stdout)
        if certs.returncode or not signed or signed.group(1) != RELEASE_CERTIFICATE_SHA256:
            raise Refused(f"gate 2, {game}: not signed with the release key: {certs.stdout[-300:]}{certs.stderr[-300:]}")
        badging = run(self.aapt2, "dump", "badging", apk).splitlines()[0]
        package = re.search(r"name='([^']+)' versionCode='(\d+)' versionName='([^']+)'", badging)
        if not package or package.group(1) != GAMES[game][3] or int(package.group(2)) != version_code(tag):
            raise Refused(f"gate 2, {game}: the APK is {badging}, not {GAMES[game][3]} {version_code(tag)}")
        return package.group(3)

    def gate_driver(self, game, apk):
        fetch = (REPOSITORY / "android" / "fetch_deps.sh").read_text(encoding="utf-8")
        wanted = re.search(r'(?m)^TURNIP_SHA256="(\w+)"', fetch).group(1)
        with zipfile.ZipFile(apk) as package:
            names = package.namelist()
            drivers = [name for name in names if name.startswith("assets/drivers/") and name.endswith(".zip")]
            if not drivers:
                raise Refused(f"gate 3, {game}: no Turnip package under assets/drivers/")
            for name in drivers:
                data = package.read(name)
                if hashlib.sha256(data).hexdigest() != wanted:
                    raise Refused(f"gate 3, {game}: {name} is not the Turnip package android/fetch_deps.sh names")
                inside = zipfile.ZipFile(io.BytesIO(data)).namelist()
                if "meta.json" not in inside or not any(one.endswith(".so") for one in inside):
                    raise Refused(f"gate 3, {game}: {name} has no driver in it: {inside}")
            for library in ADRENOTOOLS_LIBRARIES:
                entry = f"lib/arm64-v8a/{library}"
                if entry not in names or package.read(entry)[:4] != b"\x7fELF":
                    raise Refused(f"gate 3, {game}: libadrenotools' {library} is not in the APK")
            # The hook libraries alone load nothing: the runtime's own library has to call into libadrenotools.
            for title in GAMES[game][4]:
                if b"adrenotools" not in package.read(f"lib/arm64-v8a/lib{title}.so"):
                    raise Refused(f"gate 3, {game}: lib{title}.so was built without libadrenotools")
        return drivers[0].rsplit("/", 1)[1]

    def gate_abi(self, game, tag, abi, notes):
        was = self.state["published"][game]
        folder = self.out / "published" / f"{game}-{was}"
        folder.mkdir(parents=True, exist_ok=True)
        name = f"{game}-recomp-{was}-game-abi.txt"
        if not (folder / name).exists():
            run("gh", "release", "download", was, "-R", self.repo(game), "-D", folder, "-p", name)
        before = set((folder / name).read_text().split("\n")) - {""}
        after = set(abi.read_text().split("\n")) - {""}
        if {line.split()[0] for line in after} != set(GAMES[game][4]):
            raise Refused(f"gate 4, {game}: statements for {sorted(after)}, wanted one for each of {GAMES[game][4]}")
        stranded = sorted(before - after)
        if stranded and not re.search(r"(?i)built again|rebuil", notes[:400]):
            raise Refused(f"gate 4, {game}: the published {was} took {stranded} and {tag} does not, and the notes' "
                          "first lines do not say that the game code is built again")
        return "the published app's game code is taken as it is" if not stranded else \
            f"game code of {[line.split()[0] for line in stranded]} is built again (the notes say so)"

    def gate_toolchain(self):
        """The build tools the apps name: there, with the SHA-256 named. Returns its tag when still a draft."""
        named = {}
        for game in self.games:
            text = run("git", "show", f"{self.state['mirrors'][game]}:setup/toolchain.json", cwd=self.clone(game))
            named[game] = json.loads(text)
        first = named[self.games[0]]
        if any(one != first for one in named.values()):
            raise Refused(f"gate 4: the mirrors name different build tools: {named}")
        repo, tag = first["release"]["repo"], first["release"]["tag"]
        releases = gh_json("api", f"repos/{repo}/releases?per_page=100")
        release = next((one for one in releases if one["tag_name"] == tag or one["name"].startswith(f"Build tools {first['version']} ")), None)
        if release is None:
            raise Refused(f"gate 4: {repo} has no release {tag}: run Build tools package and name its hash first")
        asset = next((one for one in release["assets"] if one["name"] == first["asset"]), None)
        digest = (asset or {}).get("digest") or ""
        if not asset or (digest and digest != f"sha256:{first['sha256']}"):
            raise Refused(f"gate 4: {tag}'s {first['asset']} is not the one setup/toolchain.json names ({digest})")
        if not digest:
            folder = self.out / "toolchain"
            folder.mkdir(exist_ok=True)
            run("gh", "release", "download", tag, "-R", repo, "-D", folder, "-p", first["asset"], "--clobber")
            if sha256_of(folder / first["asset"]) != first["sha256"]:
                raise Refused(f"gate 4: {tag}'s {first['asset']} is not the one setup/toolchain.json names")
        print(f"toolchain: {tag} in {repo}, {'a draft: published first' if release['draft'] else 'published'}, "
              f"SHA-256 as named")
        return (repo, tag, first) if release["draft"] else (repo, None, first)

    def notes(self, game, tag):
        path = REPOSITORY / "docs" / "releases" / f"{game}-{tag}.txt"
        if not path.exists():
            raise Refused(f"gate 7, {game}: {path} is missing")
        text = path.read_text(encoding="utf-8").strip() + "\n"
        problems = []
        if len(text) >= NOTES_LIMIT:
            problems.append(f"{len(text)} characters, the app shows fewer than {NOTES_LIMIT}")
        if re.search(r"(?m)^#+ |\*\*|`|\]\(|<[a-z]+>|^\s*\* ", text):
            problems.append("not plain text (Markdown or HTML in it)")
        if not re.search(r"(?im)^known issues", text):
            problems.append("no 'Known issues' line")
        if tag not in text:
            problems.append(f"does not name {tag}")
        if problems:
            raise Refused(f"gate 7, {game}: the notes: {'; '.join(problems)}")
        return text

    def gates(self, wait):
        """Gates 1 to 4 and 7 on the drafts as they are now; the report publish compares with."""
        toolchain = self.gate_toolchain()
        report = {"toolchain": toolchain[1], "games": {}}
        for game in self.games:
            tag = self.state["tags"][game]
            notes = self.notes(game, tag)
            ci = self.gate_ci(game, tag, wait)
            release, apk, abi = self.draft(game, tag)
            name = self.gate_apk(game, tag, apk)
            driver = self.gate_driver(game, apk)
            abi_result = self.gate_abi(game, tag, abi, notes)
            report["games"][game] = {"tag": tag, "apk": str(apk), "apk_sha256": sha256_of(apk), "version": name,
                                     "draft": release["isDraft"], "ci": ci, "driver": driver, "abi": abi_result,
                                     "notes_characters": len(notes), "statements": abi.read_text().split()}
            print(f"{game} {tag}: PASS gates 1-4 and 7: {name}; CI {ci}; 0 recompiled bodies, no game files, "
                  f"release key; {driver} and libadrenotools inside; {abi_result}; notes {len(notes)} characters")
        (self.out / "gates.json").write_text(json.dumps(report, indent=2))
        return report, toolchain

    # ---- the device gates' report and publishing -----------------------------
    def device_template(self):
        report = {"note": "Gates 5 and 6. Every 'pass' has to be true and every 'evidence' a file or folder that "
                          "exists; 'apk_sha256' is the draft's APK, or 'equivalent' says what was tried in its place.",
                  "games": {}}
        for game in self.games:
            devices = {device: {check: {"pass": False, "evidence": "", "result": ""} for check in DEVICE_CHECKS}
                       for device in DEVICES}
            report["games"][game] = {
                "apk_sha256": "", "equivalent": "", "devices": devices,
                "update": {"pass": False, "from": self.state.get("published", {}).get(game, ""), "saves_kept": False,
                           "settings_kept": False, "game_code": "", "evidence": ""}}
        print(json.dumps(report, indent=2))

    def device_gates(self, path, gates):
        report = json.loads(Path(path).read_text(encoding="utf-8"))
        problems = []
        for game in self.games:
            entry = report.get("games", {}).get(game)
            if not entry:
                problems.append(f"{game}: not in the report")
                continue
            if entry.get("apk_sha256") != gates["games"][game]["apk_sha256"] and not entry.get("equivalent"):
                problems.append(f"{game}: tried another APK than the draft's and does not say what")
            for device in DEVICES:
                for check in DEVICE_CHECKS:
                    one = entry.get("devices", {}).get(device, {}).get(check, {})
                    if one.get("pass") is not True or not Path(one.get("evidence", "") or "?").exists():
                        problems.append(f"{game}, {device}, {check}: not passed, or no evidence")
            update = entry.get("update", {})
            if not all(update.get(key) is True for key in ("pass", "saves_kept", "settings_kept")) or \
                    not update.get("game_code") or not Path(update.get("evidence", "") or "?").exists():
                problems.append(f"{game}: the update from {self.state['published'][game]}: not passed, or no evidence")
        if problems:
            raise Refused("gates 5 and 6: " + "; ".join(problems))
        print("gates 5 and 6: PASS by", path)

    def publish(self, device_report):
        gates, (toolchain_repo, toolchain_tag, toolchain) = self.gates(wait=False)
        self.device_gates(device_report, gates)
        if toolchain_tag:
            run("gh", "release", "edit", toolchain_tag, "-R", toolchain_repo, "--draft=false", "--prerelease", "--latest=false")
            print(f"published {toolchain_repo} {toolchain_tag}")
        for game in self.games:
            tag = self.state["tags"][game]
            notes = self.out / f"notes-{game}.txt"
            notes.write_text(self.notes(game, tag), encoding="utf-8", newline="\n")
            run("gh", "release", "edit", tag, "-R", self.repo(game), "--notes-file", notes, "--draft=false",
                "--prerelease=false", "--latest")
            print(f"published https://github.com/{self.repo(game)}/releases/tag/{tag}")
        self.verify_public()

    def verify_public(self):
        """What a player's app and setup fetch: the latest tag, the APK and statements by address, the build tools."""
        gates = json.loads((self.out / "gates.json").read_text())
        problems = []

        def fetched(url):
            digest, size = hashlib.sha256(), 0
            request = urllib.request.Request(url, headers={"User-Agent": "release.py"})
            with urllib.request.urlopen(request, timeout=120) as response:
                for block in iter(lambda: response.read(1 << 20), b""):
                    digest.update(block)
                    size += len(block)
            return digest.hexdigest(), size

        for game in self.games:
            tag, repo = self.state["tags"][game], self.repo(game)
            latest = json.loads(urllib.request.urlopen(urllib.request.Request(
                f"https://api.github.com/repos/{repo}/releases/latest", headers={"User-Agent": "release.py"}),
                timeout=60).read())
            if latest["tag_name"] != tag or latest["draft"] or latest["prerelease"]:
                problems.append(f"{game}: latest is {latest['tag_name']}, not {tag}")
            if len(latest.get("body") or "") >= NOTES_LIMIT:
                problems.append(f"{game}: the published notes have {len(latest['body'])} characters")
            for name, wanted in ((f"{game}-recomp-{tag}.apk", gates["games"][game]["apk_sha256"]),
                                 (f"{game}-recomp-{tag}-game-abi.txt", None)):
                for url in (f"https://github.com/{repo}/releases/download/{tag}/{name}",
                            f"https://github.com/{repo}/releases/latest/download/{name}"):
                    try:
                        digest, size = fetched(url)
                    except Exception as error:  # noqa: BLE001 - whatever stops a player's download stops this
                        problems.append(f"{url}: {error}")
                        continue
                    if wanted and digest != wanted:
                        problems.append(f"{url}: not the APK the gates passed")
                    print(f"{url}: {size} bytes" + (", the gates' APK" if wanted else ""))
        named = json.loads((REPOSITORY / "setup" / "toolchain.json").read_text())
        url = (f"https://github.com/{named['release']['repo']}/releases/download/{named['release']['tag']}/"
               f"{named['asset']}")
        try:
            digest, size = fetched(url)
            if digest != named["sha256"]:
                problems.append(f"{url}: not the SHA-256 setup/toolchain.json names")
            print(f"{url}: {size} bytes, the SHA-256 named")
        except Exception as error:  # noqa: BLE001
            problems.append(f"{url}: {error}")
        if problems:
            raise Refused("after publishing: " + "; ".join(problems))
        print("public: every download answers and every 'latest' is the new tag")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("step", choices=("status", "sync", "tag", "gates", "device-template", "publish", "verify-public"))
    parser.add_argument("--work", help="the folder with the mirrors' clones and the SDK (RELEASE_WORK)")
    parser.add_argument("--branch", default="origin/android", help="the private branch to release")
    parser.add_argument("--games", default=",".join(GAMES))
    parser.add_argument("--version", action="append", default=[], metavar="GAME=vX.Y.Z",
                        help="a tag other than the published one's patch plus one")
    parser.add_argument("--push", action="store_true", help="sync: push each mirror's main")
    parser.add_argument("--wait", action="store_true", help="gates: wait for the workflows to end")
    parser.add_argument("--device-report", help="publish: the filled report of gates 5 and 6")
    args = parser.parse_args()
    try:
        release = Release(args)
        if args.step == "status":
            release.status()
        elif args.step == "sync":
            release.sync(args.push)
        elif args.step == "tag":
            release.tag()
        elif args.step == "gates":
            release.gates(args.wait)
        elif args.step == "device-template":
            release.device_template()
        elif args.step == "publish":
            if not args.device_report:
                raise Refused("publish wants --device-report: the device gates (5 and 6) are not this script's to pass")
            release.publish(args.device_report)
        else:
            release.verify_public()
    except Refused as refused:
        print(f"REFUSED: {refused}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

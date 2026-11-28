#!/usr/bin/env python3
"""The release of the four Android apps, from the private branch to published (docs/android-releases.md).

    python tools/release/release.py status
    python tools/release/release.py sync [--push]
    python tools/release/release.py tag
    python tools/release/release.py withdraw
    python tools/release/release.py gates [--wait]
    python tools/release/release.py published-apk
    python tools/release/release.py device-template
    python tools/release/release.py publish --device-report <report.json>
    python tools/release/release.py verify-public
    python tools/release/release.py announce [--dry-run]
    python tools/release/release.py drivers [--upload]

Each step refuses to go on when a gate fails and says which. Nothing is
published by anything but `publish`, which runs the gates again on the drafts
as they are then and wants the report of the device gates (5 and 6), which a
person or an agent fills after trying the apps on the devices. The update of
gate 6 is tried as a player has it: the previous published APK, the game code
built on the device by that app, and the new APK put over it with
`pm install -r` from a file on the device; the report says so and publish
checks what it can (the old APK's SHA-256, whether a rebuild ran as the notes
say it would, the game-code line after it).

  sync     filters the private branch into the four public mirrors' histories
           (the paths docs/android-releases.md names), checks each for game
           files, for a blob over 20 MB and for a path the filter drops, and
           that the public main is an ancestor of the result; --push then
           pushes main, never forced.
  tag      pushes the next version's tag to each mirror: its workflow builds
           the app and drafts the release.
  withdraw takes a tag and its draft back, for a release that is to be cut
           from a later commit after all; never one that was published.
  gates    1 CI: every workflow on the mirror's main and the tag's release
             workflow ended well.
           2 tools/check_runtime_apk.py on the draft's APK; its certificate is
             the release key's; its package and version are the tag's.
           3 the three Turnip packages (the two community ones, for the 6xx/7xx
             and the 8xx, by android/fetch_deps.sh's SHA-256s, the project's
             API 29 build by build_turnip.sh's Mesa commit) and libadrenotools'
             libraries are inside the APK.
           4 every statement the published app took is taken by the draft, or
             the notes say in their first lines that the game code is built
             again; the toolchain release setup/toolchain.json names exists
             with that SHA-256 (publish makes it public before the apps), and
             its package was built from the recompiler's patch and commit the
             mirror has (its assets/toolchain/toolchain.json).
           7 the notes (docs/releases/<game>-<tag>.txt): plain text, under
             1,450 characters, with a line on known issues.
  published-apk
           downloads the previous published release's APK from GitHub
           Releases (what a player's phone updates from), checks it as gate 2
           does, and prints its path and SHA-256: the old app of gate 6.
  announce the Discord Releases message of each game's latest published release
           (tools/release/announce.py): --dry-run prints it and sends nothing.
           The message is sent by .github/workflows/announce-release.yml when a
           release is published, so this step is for a look beforehand (on the
           last release) or a resend; it needs no RELEASE_WORK, and sending
           takes the webhook from DISCORD_RELEASES_WEBHOOK or --webhook-file.
  drivers  the graphics driver catalog the apps ask for once a day
           (android/drivers/catalog.json, docs/android.md "The driver
           catalog"): checks it (tools/driver_catalog.py check, and verify:
           every package downloaded and hashed), refuses a version that is not
           newer than the published one, and prints exactly what would be
           public: the repository, the release, the one file, its SHA-256 and
           each driver it names. Nothing is published without --upload, which
           makes the pre-release `drivers` of mw3-recomp when there is none
           and puts driver-catalog.json on it, then reads it back by the
           address the apps use. It publishes on a public repository: the
           owner says when. Needs no RELEASE_WORK.
  publish  toolchain first when it is still a draft, then the games, then
           verify-public: the download addresses answer with the drafts'
           bytes and each repository's latest release is the new tag. Last,
           the Discord bot's cases the release provably fixed are told
           "Fixed in <game> <tag>" in their threads (botctl fixed-in, below).
           --dry-run runs the gates, publishes nothing and prints those cases.
  fixed-in the last part of publish alone, for the tags of this release: the
           Discord bot is given each game's repository, tag and the commit
           the tag is at, and tells the open cases whose issue was closed by
           a commit in that tag's history, each thread once. --dry-run prints
           them and posts nothing. It never fails or holds up a release: the
           bot is stopped after BOT_SECONDS and a failure is one warning.

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
import tempfile
import time
import urllib.request
import zipfile
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPOSITORY / "tools"))
import check_runtime_apk  # noqa: E402

sys.path.insert(0, str(Path(__file__).resolve().parent))
import announce  # noqa: E402

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
                   "android/toolchain-probe/build/", "aw/", "bo3/"]
DROPPED_FILES = [
    "android/app/src/main/assets/shader_cache.bin",
    "android/app/src/main/res/drawable/banner_mw2.png",
    "android/app/src/main/res/drawable/ic_launcher_foreground.png",
    *[f"android/app/src/main/res/mipmap-{density}/ic_launcher{shape}.png"
      for density in ("hdpi", "mdpi", "xhdpi", "xxhdpi", "xxxhdpi") for shape in ("", "_round")],
    "docs/HOMELAB-RELAY-REQUEST.md", "docs/images/multiplayer-menu.png",
    "docs/images/readme/multiplayer-menu.jpg", "docs/images/readme/split-screen.jpg",
]
DROPPED_GLOBS = ["ppc_aw*", "ppc_bo3*"]
# Two earlier launcher icons, wherever they were in the tree.
DROPPED_BLOBS = ["1f2bbd4247228de79269766ed3f5850630b3955d", "acdce15999039b8a20bb8008717f0a126ea1d5d0"]
# The public copy of this private commit has one more newline at its message's end.
MESSAGE_WITH_NEWLINE = "be9b1af"
GAME_PATH = re.compile(r"(?i)\.(xex|xexp|ff|ffm|iwd|iwi|bik|iso|god|pak|apk|aab|so|pe)$|^(aw|bo3|cod4|mw2|mw3|ppc[^/]*)/")
# A game that is not released is kept out of the mirrors whole (docs/android-releases.md, Games that are not
# released): tools/release/unreleased.json, which the filter drops with the rest, names its files, the words
# that give it away and the files that had such a word in public before. In a file the mirrors keep, its
# lines stand between two comment lines, the marks private-begin and private-end with an @ before each, or
# end in the comment @ private-line (no space); a comment line that starts with @ public: (no space) holds
# the line the mirrors have in their place.
UNRELEASED = "tools/release/unreleased.json"
PRIVATE_REGION = re.compile(rb"^[ \t]*(?://|#|<!--) ?@private-(begin|end)\b")
PRIVATE_LINE = re.compile(rb"(?://|#) ?@private-line[ \t]*\r?\n?$")
PUBLIC_LINE = re.compile(rb"^[ \t]*(?://|#) ?@public: ?(.*)$", re.S)
NOT_RELEASED = b"Work on a game that is not released.\n"
ADRENOTOOLS_LIBRARIES = ("libhook_impl.so", "libmain_hook.so", "libfile_redirect_hook.so", "libgsl_alloc_hook.so")
DEVICES = ("thor", "flip")
DEVICE_CHECKS = ("campaign", "multiplayer", "visual", "turnip", "fps")
# The Discord bot's command on this PC (yassinsolim/projects-bot, its docs/discord-bot.md); PROJECTS_BOT moves it.
BOTCTL = Path(os.environ.get("PROJECTS_BOT", Path.home() / "projects-bot")) / "botctl.cmd"
BOT_SECONDS = 300  # for one game's cases
DISCORD_NAMES = {"mw3": "MW3", "mw2": "MW2", "cod4": "CoD4", "aw": "AW"}


class Refused(Exception):
    """A gate that did not pass."""


def run(*command, cwd=None, check=True, stdin=None):
    done = subprocess.run([str(part) for part in command], cwd=cwd, input=stdin, capture_output=True)
    out = (done.stdout + done.stderr).decode("utf-8", "replace").strip()
    if check and done.returncode != 0:
        raise Refused(f"{' '.join(str(part) for part in command[:6])} ...: {out[-1500:]}")
    return out


def bounded(command, seconds, cwd=None):
    """The exit code and output of a command that may not hold the release up: over time, it and its children are killed.

    The output goes to a file, not a pipe: a child that outlives the kill cannot keep this waiting for a pipe's end.
    """
    with tempfile.TemporaryFile() as output:
        process = subprocess.Popen([str(part) for part in command], cwd=cwd, stdin=subprocess.DEVNULL, stdout=output,
                                   stderr=subprocess.DEVNULL)
        try:
            code = process.wait(timeout=seconds)
        except subprocess.TimeoutExpired:
            try:
                if os.name == "nt":  # botctl.cmd is cmd.exe, then the launcher, then Python: all of them go
                    subprocess.run(["taskkill", "/F", "/T", "/PID", str(process.pid)], capture_output=True, timeout=30)
            finally:
                process.kill()
                process.wait(timeout=30)
            raise
        output.seek(0)
        return code, output.read().decode("utf-8", "replace").strip()


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


def git_blob(clone, commit, path):
    """A file of a commit as it is stored, byte for byte (run() strips the output)."""
    return subprocess.run(["git", "show", f"{commit}:{path}"], cwd=clone, capture_output=True, check=True).stdout


def toolchain_manifest(package):
    """What the build tools' package says it holds (tools/toolchain/package.py writes it)."""
    with zipfile.ZipFile(package) as apk:
        return json.loads(apk.read("assets/toolchain/toolchain.json"))


def toolchain_mismatches(manifest, named, patch, setup):
    """How the package setup/toolchain.json names (`named`) differs from what this release recompiles with:
    the recompiler's patch (hashed without carriage returns, as package.py and the app's build kit hash it),
    XenonRecomp's commit in setup.ps1, and the version. A package made from another patch builds other game
    code on the phone than the app expects, and the app refuses it."""
    problems = []
    built = manifest.get("recompiler", {})
    patch_hash = hashlib.sha256(patch.replace(b"\r", b"")).hexdigest()
    if built.get("patchSha256") != patch_hash:
        problems.append(f"patches/xenonrecomp-mw2.patch is {patch_hash[:12]}, the package's {str(built.get('patchSha256'))[:12]}")
    commit = re.search(rb"XenonCommit\s*=\s*'([0-9a-f]+)'", setup)
    commit = commit.group(1).decode() if commit else None
    if built.get("commit") != commit:
        problems.append(f"setup.ps1 names XenonRecomp {commit}, the package {built.get('commit')}")
    if str(manifest.get("version")) != str(named["version"]):
        problems.append(f"setup/toolchain.json says version {named['version']}, the package {manifest.get('version')}")
    return problems


def public_text(data):
    """A file as the mirrors have it: the marked lines of a game that is not released taken out."""
    if (b"@private-" not in data and b"@public:" not in data) or b"\0" in data:
        return data
    kept, private = [], False
    for line in data.splitlines(keepends=True):
        region = PRIVATE_REGION.match(line)
        if region:
            if private == (region.group(1) == b"begin"):
                raise Refused(f"a file's {line.strip().decode('utf-8', 'replace')!r} is out of place")
            private = not private
        elif not private and not PRIVATE_LINE.search(line):
            public = PUBLIC_LINE.match(line)
            kept.append(public.group(1) if public else line)
    if private:
        raise Refused("a file's private region is not closed")
    return b"".join(kept)


def public_message(message, mention):
    """A commit's message as the mirrors have it. One that names a game that is not released is replaced by
    what follows its 'Public:' line, the part of the commit the mirrors get, or by a line that says so."""
    if not mention or not re.search(mention.encode(), message):
        return message
    public = re.search(rb"(?ms)^Public: *(.*)\Z", message)
    text = public.group(1).strip() + b"\n" if public and public.group(1).strip() else NOT_RELEASED
    if re.search(mention.encode(), text):
        raise Refused(f"a commit's Public: text names a game that is not released: {text[:200]!r}")
    return text


def unreleased(repository, commit):
    """tools/release/unreleased.json of a commit: {"paths", "globs", "mention", "published"}, or nothing."""
    found = subprocess.run(["git", "show", f"{commit}:{UNRELEASED}"], cwd=repository, capture_output=True)
    return json.loads(found.stdout) if found.returncode == 0 else {}


def filter_private(clone, private, repository=REPOSITORY):
    """The clone's branch refilter, made of the private commit: the history every mirror shares, without what
    is the project's own and without the games that are not released. Returns what unreleased.json said."""
    hidden = unreleased(repository, private)
    run("git", "fetch", "--no-tags", repository, f"+{private}:refs/heads/private-android", cwd=clone)
    run("git", "checkout", "-q", "-B", "refilter", "private-android", cwd=clone)
    paths = []
    for path in DROPPED_FOLDERS + DROPPED_FILES + hidden.get("paths", []) + ([UNRELEASED] if hidden else []):
        paths += ["--path", path]
    for glob in DROPPED_GLOBS + hidden.get("globs", []):
        paths += ["--path-glob", glob]
    commit = run("git", "rev-parse", MESSAGE_WITH_NEWLINE, cwd=repository)
    callback = (f'if commit.original_id == b"{commit}": '
                'commit.message = commit.message.rstrip(b"\\n") + b"\\n\\n"')
    marked = []
    if hidden:
        here = f"import sys; sys.path.insert(0, {str(Path(__file__).resolve().parent)!r}); import release; "
        callback += f'\n{here}commit.message = release.public_message(commit.message, {hidden["mention"]!r})'
        marked = ["--blob-callback", f"{here}blob.data = release.public_text(blob.data)"]
    run("git", "filter-repo", "--force", "--refs", "refilter", "--invert-paths", *paths,
        "--commit-callback", callback, *marked, cwd=clone)
    return hidden


def unreleased_problems(clone, hidden, since=None):
    """Where refilter still has a game that is not released: a path, a commit's message, a text file. `since`
    is the public main the history goes on from: what is older was looked at when it was pushed."""
    if not hidden:
        return []
    mention = re.compile(hidden["mention"].encode())
    published = set(hidden.get("published", []))
    span = ["refilter"] + ([f"^{since}"] if since else [])
    bad = []
    for line in run("git", "log", "--format=%h %B%x00", *span, cwd=clone).split("\0"):
        if mention.search(line.encode()):
            bad.append(f"commit {line.split()[0]}: its message")
    objects = subprocess.run(["git", "rev-list", "--objects", *span], cwd=clone, capture_output=True).stdout
    names = dict(line.split(" ", 1) for line in objects.decode("utf-8", "replace").splitlines() if " " in line)
    bad += sorted(f"{path}: its name" for path in set(names.values()) if mention.search(path.encode()))
    with tempfile.TemporaryFile() as wanted:
        wanted.write("".join(name + "\n" for name in names).encode())
        wanted.seek(0)
        with subprocess.Popen(["git", "cat-file", "--batch"], cwd=clone, stdin=wanted, stdout=subprocess.PIPE) as batch:
            while header := batch.stdout.readline():
                name, kind, size = header.split()
                data = batch.stdout.read(int(size))
                batch.stdout.read(1)
                path = names[name.decode()]
                if kind != b"blob" or path in published or b"\0" in data[:8000]:
                    continue
                found = mention.search(data)
                if found:
                    line = data.count(b"\n", 0, found.start()) + 1
                    bad.append(f"{path}:{line} ({name.decode()[:9]}): {found.group(0).decode('utf-8', 'replace')!r}")
                elif public_text(data) != data:
                    bad.append(f"{path} ({name.decode()[:9]}): a private mark is left")
    return bad


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
        """Game files, dropped paths and large blobs anywhere in refilter's history."""
        paths = set(filter(None, run("git", "log", "--format=", "--name-only", "-m", "--no-renames", "refilter",
                                     cwd=clone).splitlines()))
        bad = sorted(path for path in paths if path.startswith(tuple(DROPPED_FOLDERS)) or path in DROPPED_FILES
                     or path in extra or GAME_PATH.search(path))
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
        hidden = filter_private(base, private)
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
            bad = unreleased_problems(clone, hidden, public)
            if bad:
                raise Refused(f"{game}: of a game that is not released, in the filtered history: {bad[:20]}")
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

    def withdraw(self):
        """Takes the tags pushed by `tag` back, with their drafts; a published release stays for ever."""
        for game in self.games:
            clone, tag = self.clone(game), self.state.get("tags", {}).get(game)
            if not tag:
                continue
            # A workflow still building for the tag would draft the release again after it is gone.
            for one in gh_json("run", "list", "-R", self.repo(game), "--branch", tag, "--json", "databaseId,status"):
                if one["status"] != "completed":
                    run("gh", "run", "cancel", str(one["databaseId"]), "-R", self.repo(game), check=False)
            releases = gh_json("api", f"repos/{self.repo(game)}/releases?per_page=30")
            release = next((one for one in releases if one["tag_name"] == tag), None)
            if release and not release["draft"]:
                raise Refused(f"{game}: {tag} is published: players have it, and it is not taken back")
            if release:
                run("gh", "api", "-X", "DELETE", f"repos/{self.repo(game)}/releases/{release['id']}")
            if run("git", "ls-remote", "--tags", "public", tag, cwd=clone):
                run("git", "push", "public", f":refs/tags/{tag}", cwd=clone)
            run("git", "tag", "-d", tag, cwd=clone, check=False)
            print(f"{game}: {tag} withdrawn" + (" with its draft" if release else ""))

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
        # The three Turnip packages android/fetch_deps.sh puts in the app: the two community ones (the 6xx/7xx's and
        # the 8xx's) by their SHA-256s, and the project's own build for Android 10 (android/build_turnip.sh, built by
        # the release workflow, so not hashed) by the Mesa commit its meta.json names, the check fetch_deps.sh makes.
        fetch = (REPOSITORY / "android" / "fetch_deps.sh").read_text(encoding="utf-8")
        hashed = {}
        for prefix in ("TURNIP", "TURNIP8"):
            asset = re.search(rf'(?m)^{prefix}_ASSET=".*/drivers/([^"/]+)"', fetch).group(1)
            hashed["assets/drivers/" + asset] = re.search(rf'(?m)^{prefix}_SHA256="(\w+)"', fetch).group(1)
        own = "assets/drivers/" + re.search(r'(?m)^TURNIP29_ASSET=".*/drivers/([^"/]+)"', fetch).group(1)
        build = (REPOSITORY / "android" / "build_turnip.sh").read_text(encoding="utf-8")
        mesa = re.search(r'(?m)^MESA_COMMIT="(\w+)"', build).group(1)[:10]
        with zipfile.ZipFile(apk) as package:
            names = package.namelist()
            drivers = [name for name in names if name.startswith("assets/drivers/") and name.endswith(".zip")]
            for name in (*hashed, own):
                if name not in drivers:
                    raise Refused(f"gate 3, {game}: no {name}")
            for name in drivers:
                data = package.read(name)
                inside = zipfile.ZipFile(io.BytesIO(data))
                if "meta.json" not in inside.namelist() or not any(one.endswith(".so") for one in inside.namelist()):
                    raise Refused(f"gate 3, {game}: {name} has no driver in it: {inside.namelist()}")
                if name in hashed:
                    if hashlib.sha256(data).hexdigest() != hashed[name]:
                        raise Refused(f"gate 3, {game}: {name} is not the Turnip package android/fetch_deps.sh names")
                elif name == own:
                    if mesa not in json.loads(inside.read("meta.json")).get("driverVersion", ""):
                        raise Refused(f"gate 3, {game}: {name} is not built from Mesa {mesa} "
                                      "(android/build_turnip.sh)")
                else:
                    raise Refused(f"gate 3, {game}: {name} is a driver package android/fetch_deps.sh does not name")
            for library in ADRENOTOOLS_LIBRARIES:
                entry = f"lib/arm64-v8a/{library}"
                if entry not in names or package.read(entry)[:4] != b"\x7fELF":
                    raise Refused(f"gate 3, {game}: libadrenotools' {library} is not in the APK")
            # The hook libraries alone load nothing: the runtime's own library has to call into libadrenotools.
            for title in GAMES[game][4]:
                if b"adrenotools" not in package.read(f"lib/arm64-v8a/lib{title}.so"):
                    raise Refused(f"gate 3, {game}: lib{title}.so was built without libadrenotools")
        return ", ".join(name.rsplit("/", 1)[1] for name in (*hashed, own))

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
        folder = self.out / "toolchain" / tag
        folder.mkdir(parents=True, exist_ok=True)
        package = folder / first["asset"]
        if not package.exists() or sha256_of(package) != first["sha256"]:
            run("gh", "release", "download", tag, "-R", repo, "-D", folder, "-p", first["asset"], "--clobber")
            if sha256_of(package) != first["sha256"]:
                raise Refused(f"gate 4: {tag}'s {first['asset']} is not the one setup/toolchain.json names")
        for game in self.games:
            mirror, clone = self.state["mirrors"][game], self.clone(game)
            problems = toolchain_mismatches(toolchain_manifest(package), first,
                                            git_blob(clone, mirror, "patches/xenonrecomp-mw2.patch"),
                                            git_blob(clone, mirror, "setup/setup.ps1"))
            if problems:
                raise Refused(f"gate 4, {game}: {tag} was built from other recompiler inputs than this release's: "
                              f"{'; '.join(problems)}. Raise TOOLCHAIN_VERSION, run Build tools package and name "
                              "the new release in setup/toolchain.json (docs/android-releases.md, The toolchain release)")
        print(f"toolchain: {tag} in {repo}, {'a draft: published first' if release['draft'] else 'published'}, "
              f"SHA-256 as named, built from this release's recompiler and patch")
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
    def published_apk(self, game):
        """The published release's APK as GitHub Releases serves it: what a player's phone downloads and updates from."""
        was = self.state.get("published", {}).get(game) or self.published(game)
        folder = self.out / "published" / f"{game}-{was}"
        folder.mkdir(parents=True, exist_ok=True)
        apk = folder / f"{game}-recomp-{was}.apk"
        if not apk.exists():
            run("gh", "release", "download", was, "-R", self.repo(game), "-D", folder, "-p", apk.name)
        self.gate_apk(game, was, apk)
        return apk

    def device_template(self):
        report = {"note": "Gates 5 and 6. Every 'pass' has to be true and every 'evidence' a file or folder that "
                          "exists; 'apk_sha256' is the draft's APK, or 'equivalent' says what was tried in its place. "
                          "The update is the player's (docs/android-releases.md, The update as a player does it).",
                  "games": {}}
        for game in self.games:
            devices = {device: {check: {"pass": False, "evidence": "", "result": ""} for check in DEVICE_CHECKS}
                       for device in DEVICES}
            report["games"][game] = {
                "apk_sha256": "", "equivalent": "", "devices": devices,
                "update": {
                    "pass": False, "from": self.state.get("published", {}).get(game, ""),
                    # The published APK, from `published-apk`, installed first; or, on a device that has the owner's
                    # app, the package suffix the previous version was built and signed under instead.
                    "from_apk_sha256": "", "test_package": "",
                    "built_on_device": False,
                    # The command that put the new app over the old: "pm install -r", from a file on the device.
                    "installed_with": "",
                    "saves_kept": False, "settings_kept": False,
                    # Whether a build ran after the update, and the report's "game code:" line from after it.
                    "game_code_rebuilt": None, "game_code": "",
                    "minutes": 0, "evidence": "",
                    # The fast check kept before it: the new app over the old with `adb install -r` on game code from a PC.
                    "precheck": {"pass": False, "evidence": ""}}}
        print(json.dumps(report, indent=2))

    def update_problems(self, game, update, gates):
        """What is wrong with the report of gate 6, the update as a player does it."""
        problems = []
        if not all(update.get(key) is True for key in ("pass", "saves_kept", "settings_kept", "built_on_device")):
            problems.append("not passed, or the old app did not build the game code on the device")
        if not Path(update.get("evidence", "") or "?").exists():
            problems.append("no evidence")
        if not Path(update.get("precheck", {}).get("evidence", "") or "?").exists() or \
                update.get("precheck", {}).get("pass") is not True:
            problems.append("the fast pre-check (adb install -r over game code from a PC) is not passed, or has no evidence")
        if update.get("installed_with") != "pm install -r":
            problems.append("not put over the old app with pm install -r from a file on the device")
        # A phone with the owner's app on it cannot take the real package: the test package says what stood in.
        if not update.get("test_package") and update.get("from_apk_sha256") != sha256_of(self.published_apk(game)):
            problems.append("the old app is not the published APK as GitHub Releases serves it")
        # Gate 4 says whether the notes promise a rebuild: the update has to do what they say, and nothing else.
        rebuild_due = "is built again" in gates["games"][game]["abi"]
        if update.get("game_code_rebuilt") is not rebuild_due:
            problems.append(f"the game code was {'not ' if rebuild_due else ''}rebuilt after the update, "
                            f"and the notes say it {'is' if rebuild_due else 'is not'} to be")
        line = update.get("game_code", "")
        if not line or "MISSING" in line or (not rebuild_due and "rebuild is due" in line):
            problems.append("the report's game-code line after the update is missing, or says MISSING or a rebuild is due")
        return problems

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
            for found in self.update_problems(game, entry.get("update", {}), gates):
                problems.append(f"{game}: the update from {self.state['published'][game]}: {found}")
        if problems:
            raise Refused("gates 5 and 6: " + "; ".join(problems))
        print("gates 5 and 6: PASS by", path)

    def publish(self, device_report, dry_run=False):
        gates, (toolchain_repo, toolchain_tag, toolchain) = self.gates(wait=False)
        self.device_gates(device_report, gates)
        if dry_run:
            for game in self.games:
                print(f"dry run: {self.repo(game)} {self.state['tags'][game]} passes the gates and is not published")
            self.tell_discord(dry_run=True)
            return
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
        self.tell_discord()

    def tell_discord(self, dry_run=False):
        """'Fixed in <game> <tag>; please update' in the Discord threads of the cases this release provably fixed.

        The bot gets the evidence and decides: the repository, the tag and the commit the tag is at in the mirror's
        clone. It tells nobody unless GitHub has that tag at that commit in a published full release, and tells a
        case only when the commit that closed its issue is in the tag's history. A dry run posts nothing. The
        release is out by now: a bot that is missing, fails or hangs is a warning and nothing more.
        """
        if not BOTCTL.is_file():
            print(f"discord: no bot at {BOTCTL}, no 'fixed in' replies")
            return
        for game in self.games:
            tag = self.state["tags"][game]
            try:
                code, commit = bounded(("git", "rev-parse", "--verify", "--quiet", f"refs/tags/{tag}^{{commit}}"), 60,
                                       cwd=self.work / GAMES[game][0])
                if code != 0 or not re.fullmatch(r"[0-9a-f]{40}", commit):
                    print(f"discord: WARNING: {game}: no 'fixed in' replies: {tag} is not a tag in the mirror's clone")
                    continue
                code, out = bounded((BOTCTL, "fixed-in", "--repo", self.repo(game), "--name", DISCORD_NAMES[game],
                                     "--version", tag, "--commit", commit, *(["--dry-run"] if dry_run else [])), BOT_SECONDS)
            except subprocess.TimeoutExpired as late:
                print(f"discord: WARNING: {game}: the bot was stopped after {late.timeout} s; `release.py fixed-in` "
                      "tells the cases it did not reach")
                continue
            except OSError as error:
                print(f"discord: WARNING: {game}: no 'fixed in' replies: the bot did not start ({type(error).__name__})")
                continue
            for line in out.splitlines():
                print("discord:", line)
            if code != 0:
                print(f"discord: WARNING: {game}: the bot ended with {code}; `release.py fixed-in` tries again")

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


# ---- the driver catalog's public copy ---------------------------------------
DRIVERS_REPOSITORY = f"{OWNER}/mw3-recomp"
DRIVERS_TAG = "drivers"
DRIVERS_ASSET = "driver-catalog.json"
DRIVERS_URL = f"https://github.com/{DRIVERS_REPOSITORY}/releases/download/{DRIVERS_TAG}/{DRIVERS_ASSET}"
DRIVERS_NOTES = (
    "The catalog of graphics drivers the Android apps choose from (android/drivers/catalog.json in the source): "
    "names, where each package is published by its own project, and its SHA-256. The apps fetch this one file once a "
    "day; no driver is hosted here.")


def drivers(upload):
    catalog = REPOSITORY / "android" / "drivers" / "catalog.json"
    tool = REPOSITORY / "tools" / "driver_catalog.py"
    run(sys.executable, tool, "check")
    run(sys.executable, tool, "verify")
    text = catalog.read_bytes()
    ours = json.loads(text)
    try:
        with urllib.request.urlopen(DRIVERS_URL, timeout=60) as answer:
            published = answer.read()
    except OSError:
        published = None
    exists = subprocess.run(["gh", "release", "view", DRIVERS_TAG, "--repo", DRIVERS_REPOSITORY],
                            capture_output=True).returncode == 0
    print(f"repository  {DRIVERS_REPOSITORY} (public)")
    print(f"release     {DRIVERS_TAG}: " + ("exists" if exists else
                                              "to be made, a pre-release titled \"Graphics driver catalog\", never 'latest'"))
    print(f"file        {DRIVERS_ASSET}, {len(text)} bytes, SHA-256 {hashlib.sha256(text).hexdigest()}")
    print(f"address     {DRIVERS_URL}")
    print(f"version     {ours['version']}" + (
        "; nothing is published there yet" if published is None else f"; published now: {json.loads(published)['version']}"))
    print(f"notes       {DRIVERS_NOTES}")
    print("it names:")
    for driver in ours["drivers"]:
        where = driver.get("url") or "(no address: the apps carry it)"
        print(f"  {driver['id']}: {where}, SHA-256 {driver['sha256']}, "
              + ("chosen by the app" if driver.get("auto") else "by hand only"))
    print("no driver package, no app and no game file is uploaded: the one JSON file above")
    if published == text:
        print("the published catalog is this one already")
        return
    if published is not None and json.loads(published)["version"] >= ours["version"]:
        raise Refused(f"the published catalog is version {json.loads(published)['version']}, this one {ours['version']}: "
                      "an app keeps the newer of the two, so a changed catalog needs a higher version")
    if not upload:
        print("nothing published: --upload does it, when the owner has said so")
        return
    with tempfile.TemporaryDirectory() as folder:
        file = Path(folder) / DRIVERS_ASSET
        file.write_bytes(text)
        if not exists:
            run("gh", "release", "create", DRIVERS_TAG, "--repo", DRIVERS_REPOSITORY, "--title", "Graphics driver catalog",
                "--notes", DRIVERS_NOTES, "--prerelease", "--latest=false")
        run("gh", "release", "upload", DRIVERS_TAG, file, "--repo", DRIVERS_REPOSITORY, "--clobber")
    for _ in range(12):
        try:
            with urllib.request.urlopen(DRIVERS_URL, timeout=60) as answer:
                if answer.read() == text:
                    print(f"published: {DRIVERS_URL} answers with this catalog")
                    return
        except OSError:
            pass
        time.sleep(10)
    raise Refused(f"{DRIVERS_URL} does not answer with the catalog that was uploaded")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("step", choices=("status", "sync", "tag", "withdraw", "gates", "published-apk", "device-template",
                                         "publish", "verify-public", "announce", "fixed-in", "drivers"))
    parser.add_argument("--work", help="the folder with the mirrors' clones and the SDK (RELEASE_WORK)")
    parser.add_argument("--branch", default="origin/android", help="the private branch to release")
    parser.add_argument("--games", default=",".join(GAMES))
    parser.add_argument("--version", action="append", default=[], metavar="GAME=vX.Y.Z",
                        help="a tag other than the published one's patch plus one")
    parser.add_argument("--push", action="store_true", help="sync: push each mirror's main")
    parser.add_argument("--wait", action="store_true", help="gates: wait for the workflows to end")
    parser.add_argument("--dry-run", action="store_true",
                        help="announce, fixed-in: print the messages, send nothing; publish: the gates only, nothing "
                             "published or sent")
    parser.add_argument("--webhook-file", help="announce: a file holding the webhook, instead of DISCORD_RELEASES_WEBHOOK")
    parser.add_argument("--device-report", help="publish: the filled report of gates 5 and 6")
    parser.add_argument("--upload", action="store_true", help="drivers: publish the catalog (the owner's decision)")
    args = parser.parse_args()
    try:
        if args.step == "announce":
            for game in args.games.split(","):
                announce.announce(f"{OWNER}/{game}-recomp", None, args.dry_run, args.webhook_file)
            return 0
        if args.step == "drivers":
            drivers(args.upload)
            return 0
        if args.dry_run and args.step not in ("publish", "fixed-in"):
            raise Refused(f"{args.step} has no dry run")
        release = Release(args)
        if args.step == "status":
            release.status()
        elif args.step == "sync":
            release.sync(args.push)
        elif args.step == "tag":
            release.tag()
        elif args.step == "withdraw":
            release.withdraw()
        elif args.step == "gates":
            release.gates(args.wait)
        elif args.step == "published-apk":
            for game in release.games:
                apk = release.published_apk(game)
                print(f"{game}: {apk} {sha256_of(apk)}")
        elif args.step == "device-template":
            release.device_template()
        elif args.step == "publish":
            if not args.device_report:
                raise Refused("publish wants --device-report: the device gates (5 and 6) are not this script's to pass")
            release.publish(args.device_report, args.dry_run)
        elif args.step == "fixed-in":
            if not all(game in release.state.get("tags", {}) for game in release.games):
                raise Refused("fixed-in is for the tags of this release: there are none in out/release/state.json")
            release.tell_discord(args.dry_run)
        else:
            release.verify_public()
    except (Refused, announce.Refused) as refused:
        print(f"REFUSED: {refused}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

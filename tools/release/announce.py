#!/usr/bin/env python3
"""Tells the Discord server's Releases channel about a published release.

    python tools/release/announce.py --game mw3 --dry-run          # the latest published release, printed
    python tools/release/announce.py --game mw3 --tag v0.1.11      # sent
    python tools/release/announce.py --repo yassinsolim/aw-recomp --tag v0.1.4 --dry-run

The message is short: the game, the version, whether the game code is built
again, the three to five headline changes from the release's notes (the New
section; Known issues are left out), the release's link and where to ask for
help. The notes are the published release's body, as GitHub serves it, so the
message says what the players' app says.

It is sent by .github/workflows/announce-release.yml when a release is
published, in each of the four public repositories, with the webhook in the
secret DISCORD_RELEASES_WEBHOOK. A draft or a pre-release (the camo packs, the
build tools) is refused. --dry-run prints the message and sends nothing, and
needs no webhook. To send by hand, the webhook is read from the environment
variable DISCORD_RELEASES_WEBHOOK or from the file --webhook-file names; its
address is never printed.
"""
import argparse
import json
import os
import re
import sys
import urllib.error
import urllib.request
from pathlib import Path

OWNER = "yassinsolim"
GAMES = ("mw3", "mw2", "cod4", "aw")
DISCORD = "https://yassin.app/discord"  # forwards to the server's invite
SECRET = "DISCORD_RELEASES_WEBHOOK"
HEADLINES = 5
HEADLINE_LIMIT = 230
USER_AGENT = "mw-recomp-release-announce (https://github.com/yassinsolim/mw3-recomp)"


class Refused(Exception):
    """Nothing to announce, or not to be announced."""


def github(path):
    request = urllib.request.Request(f"https://api.github.com/{path}", headers={
        "User-Agent": USER_AGENT, "Accept": "application/vnd.github+json",
        **({"Authorization": f"Bearer {os.environ['GITHUB_TOKEN']}"} if os.environ.get("GITHUB_TOKEN") else {})})
    try:
        with urllib.request.urlopen(request, timeout=60) as response:
            return json.loads(response.read())
    except urllib.error.HTTPError as error:
        raise Refused(f"GitHub answered {error.code} for {path}") from None


def shortened(text, limit=HEADLINE_LIMIT):
    """The text, or its start up to a sentence's or a word's end when it is longer."""
    text = " ".join(text.split())
    if len(text) <= limit:
        return text
    cut = text[:limit]
    sentence = max(cut.rfind(". "), cut.rfind(": "))
    if sentence > limit // 2:
        return cut[:sentence + 1] if cut[sentence] == "." else cut[:sentence] + " ..."
    return cut.rsplit(" ", 1)[0].rstrip(".,;:") + " ..."


def headlines(notes):
    """The bullets of every block of the notes but Known issues, in order, at most HEADLINES."""
    found = []
    for block in re.split(r"\n\s*\n", notes.replace("\r\n", "\n")):
        lines = [line.strip() for line in block.strip().splitlines()]
        if not lines or re.match(r"(?i)known issues", lines[0]):
            continue
        for line in lines:
            if line.startswith(("- ", "* ")):
                found.append(shortened(line[2:]))
    return found[:HEADLINES]


def introduction(notes):
    """The first sentence of the paragraph after the title: whether the game code is built again."""
    blocks = [block.strip() for block in re.split(r"\n\s*\n", notes.replace("\r\n", "\n")) if block.strip()]
    if len(blocks) < 2 or blocks[1].startswith(("- ", "* ")) or "\n- " in blocks[1]:
        return ""
    match = re.match(r"(.+?[.!?])(\s|$)", " ".join(blocks[1].split()))
    return shortened(match.group(1) if match else blocks[1], 300)


def message(game_title, tag, notes, url):
    """The Discord message for a release: its title (game and version), what changed and where to get it."""
    lines = [f"**{game_title} {tag}** is out."]
    intro = introduction(notes)
    if intro:
        lines.append(intro)
    changes = headlines(notes)
    if changes:
        lines += ["", "What's new:"] + [f"- {change}" for change in changes]
    lines += ["", f"Get it: {url}", f"Help, news and requests: {DISCORD}. For bugs, attach your log (LOG -> SHARE)."]
    # Five bullets of at most HEADLINE_LIMIT characters and the introduction stay far below Discord's limit.
    return "\n".join(lines)


def game_title(release):
    """'Modern Warfare 3' from the release's name 'Modern Warfare 3 for Android v0.1.11'."""
    return re.sub(r"\s+for Android\b.*$", "", release["name"] or release["tag_name"]).strip()


def webhook(path):
    address = (Path(path).read_text(encoding="utf-8") if path else os.environ.get(SECRET, "")).strip()
    if not address.startswith("https://discord.com/api/webhooks/") and \
            not address.startswith("https://discordapp.com/api/webhooks/"):
        raise Refused(f"no webhook: set {SECRET} or give --webhook-file")
    return address


def send(address, text):
    body = json.dumps({"content": text, "allowed_mentions": {"parse": []}}).encode()
    request = urllib.request.Request(address, data=body, method="POST", headers={
        "Content-Type": "application/json", "User-Agent": USER_AGENT})
    try:
        urllib.request.urlopen(request, timeout=60).read()
    except urllib.error.HTTPError as error:  # not the error itself: it could carry the address
        raise Refused(f"Discord answered {error.code}") from None
    except urllib.error.URLError as error:
        raise Refused(f"Discord could not be reached: {error.reason}") from None


def announce(repo, tag, dry_run, webhook_file=None):
    release = github(f"repos/{repo}/releases/tags/{tag}" if tag else f"repos/{repo}/releases/latest")
    if release["draft"] or release["prerelease"]:
        raise Refused(f"{repo} {release['tag_name']} is a draft or a pre-release: not announced")
    text = message(game_title(release), release["tag_name"], release.get("body") or "", release["html_url"])
    if dry_run:
        print(text)
        print(f"\n[dry run: {len(text)} characters, nothing sent]")
        return text
    send(webhook(webhook_file), text)
    print(f"announced {repo} {release['tag_name']}")
    return text


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--game", choices=GAMES, help="the game, for its public repository")
    parser.add_argument("--repo", help="OWNER/NAME of the public repository, instead of --game")
    parser.add_argument("--tag", help="the release's tag; the latest published release when not given")
    parser.add_argument("--dry-run", action="store_true", help="print the message, send nothing")
    parser.add_argument("--webhook-file", help=f"a file holding the webhook, instead of {SECRET}")
    args = parser.parse_args()
    try:
        if bool(args.game) == bool(args.repo):
            raise Refused("give --game or --repo")
        announce(args.repo or f"{OWNER}/{args.game}-recomp", args.tag, args.dry_run, args.webhook_file)
    except Refused as refused:
        print(f"REFUSED: {refused}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

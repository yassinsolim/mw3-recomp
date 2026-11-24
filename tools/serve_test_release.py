#!/usr/bin/env python3
"""Serve one made-up release of the Android app, to test updating without publishing.

    python tools/serve_test_release.py <newer.apk> [--game mw3|mw2|cod4] [--tag v1.0.1] [--port 8765]
                                       [--toolchain recomp-toolchain.apk]

It answers as GitHub's list of releases does (the fields the app reads), with
the APK as the release's asset, on this machine only. A debug build made with
MW2_UPDATE_API=http://127.0.0.1:8765/releases.json asks it instead of GitHub,
through `adb reverse tcp:8765 tcp:8765` (docs/android-releases.md). --game is
the app being tested: each takes only an asset named for its own game.

--toolchain serves the build tools' package as well, for a debug build made
with MW2_TOOLCHAIN_URL=http://127.0.0.1:8765/recomp-toolchain.apk: the app
still takes only the file whose SHA-256 setup/toolchain.json names, so this
tries a drafted toolchain release's own APK before it is published.
"""
import argparse
import http.server
import json
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("apk", type=Path)
    parser.add_argument("--game", choices=("mw3", "mw2", "cod4"), default="mw3")
    parser.add_argument("--tag", default="v1.0.1")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--toolchain", type=Path)
    parser.add_argument("--notes", default="A test release.\n\n- Changelog line one\n- Changelog line two")
    args = parser.parse_args()
    apk = args.apk.read_bytes()
    toolchain = args.toolchain.read_bytes() if args.toolchain else None
    name = f"{args.game}-recomp-{args.tag}.apk"
    releases = json.dumps([{
        "tag_name": args.tag,
        "draft": False,
        "html_url": f"http://127.0.0.1:{args.port}/",
        "body": args.notes,
        "assets": [{"name": name, "browser_download_url": f"http://127.0.0.1:{args.port}/{name}"}],
    }]).encode()

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            if self.path.startswith("/releases.json"):
                body, kind = releases, "application/json"
            elif self.path == f"/{name}":
                body, kind = apk, "application/vnd.android.package-archive"
            elif self.path == "/recomp-toolchain.apk" and toolchain is not None:
                body, kind = toolchain, "application/vnd.android.package-archive"
            else:
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header("Content-Type", kind)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    http.server.ThreadingHTTPServer(("127.0.0.1", args.port), Handler).serve_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

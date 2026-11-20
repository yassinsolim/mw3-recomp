# Releasing the Android app (Modern Warfare 3)

The app is the runtime without the game's code ([game-library.md](game-library.md)),
so it can be built on a runner that has no game file and published. Installed
copies look at the public repository's releases when they start and from
**Check for updates**, show the release's notes, download the APK and hand it
to Android's installer, which puts it over the installed app. The game
package, the game files and the saves are not touched.

## Once: the key and the secrets

Every release has to be signed with the same key: Android refuses an update
signed with another, and a lost key means every player uninstalling.

- The keystore is **outside the repository**, never committed:
  `C:\Users\solim\mw3-android-work\keys\mw3-release.jks` (alias
  `mw3-release`), its passwords in `mw3-release.properties` beside it and its
  base64 in `mw3-release.jks.base64`. Keep a copy somewhere else as well.
  Its certificate's SHA-256 is in the workflow
  (`RELEASE_CERTIFICATE_SHA256`), which fails on any other.
- In the public repository (Settings, Secrets and variables, Actions) set
  `MW3_KEYSTORE_BASE64` (the contents of the `.base64` file),
  `MW3_KEYSTORE_PASSWORD`, `MW3_KEY_ALIAS` (`mw3-release`) and
  `MW3_KEY_PASSWORD`.

## Each release

1. The public repository's default branch has the commit to release.
2. Push a tag `vMAJOR.MINOR.PATCH` there (minor and patch below 100). The
   version code is `MAJOR*10000 + MINOR*100 + PATCH`, and has to be above the
   last release's and above 16, what the apps built by setup carried.
3. `.github/workflows/android-release.yml` builds the app with
   `-DMW2_SPLIT=runtime`, runs `tools/check_runtime_apk.py` on it (no
   recompiled code, no game files), checks the signature and drafts a release
   with `mw3-recomp-<tag>.apk` and `mw3-recomp-<tag>-game-abi.txt`.
4. Read the draft, write the notes (the app shows the first 1500 characters
   as the changelog; say so when the game code has to be rebuilt) and
   publish. Drafts are invisible to the apps.

## Trying the update without publishing

A debug build can be pointed at a list of releases served from the PC:

    set MW2_UPDATE_API=http://127.0.0.1:8765/releases.json
    set MW2_VERSION_CODE=10000        (and 10001 for the newer one)
    gradlew :app:assembleMw3Tu0Debug
    python tools/serve_test_release.py newer.apk --tag v1.0.1
    adb reverse tcp:8765 tcp:8765

Install the older build, start it, and it offers the newer one.

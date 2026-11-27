# Privacy

The app has no accounts, no adverts and no analytics. It talks to the
internet for these things, and the last of them only if you say yes:

| What | When | What leaves the phone |
|---|---|---|
| Update check | When the launcher opens | A request to GitHub for the list of releases. Nothing about you or the phone. |
| Build tools download | Once, when the game code is built on the phone | A download from GitHub. |
| Online play | When you play online | Game traffic through the relay you chose (the project's is `mw3.yassin.app`), as with any game server: it sees your IP address while you are connected and logs it with the time you joined and left. Other players see the relay's address, never yours. |
| Is the report server there? | At most once a day, when the launcher opens, from your first finished session until you have answered the question below; and each time you turn performance reports on in Settings | An empty request to `mw3.yassin.app`, so that you are not asked to send reports to a server that is not there. It carries nothing, and that server writes no access log. |
| **Performance reports** | After a session, **only if you turned them on** | The report described below. |

Your game files, disc image, saves and settings never leave the phone. A
log leaves it only when you press Share yourself.

## Performance reports

They are **off unless you say yes**. The app asks once, after your first
finished session, on one screen that lists what is sent and offers Yes, No
and an example. You can change your answer at any time in **Settings,
Network, Performance reports**, where **Reports sent** shows every report
that left this phone, as it was sent. Until you have said yes, nothing is
measured for a report. Until you have answered, nothing is stored but a
mark that a session was played, which is what the question waits for;
once you have answered, that mark is not written again.

They exist so the default settings can be tuned for each chip: a phone
nobody on the project owns can only be tuned from what it reports. The
results are public at <https://mw3.yassin.app/perf>.

### What a report holds

Exactly these fields, and nothing else:

| Field | Example |
|---|---|
| Phone model | `AYN Thor` |
| Chip and board | `QTI SM8550, board kalama` |
| Graphics chip (GPU) | `Turnip Adreno (TM) 740` |
| Graphics driver name and version | `turnip Mesa driver`, `Mesa 25.3.0` |
| Android version | `13` |
| App version | `0.1.7` |
| Game and game version | `mw3`, `disc` |
| Mode and map | `multiplayer`, `mp_dome` |
| Session length in minutes | `23` |
| Average FPS and 1% low | `58.6`, `41.2` |
| What held the frame rate back | `gpu` or `cpu` |
| Graphics settings in use: preset, scene scale, shadows, FPS limit, multiplier | `automatic`, `70`, `auto`, `60`, `1` |
| Hottest thermal status the phone reported (0 to 6) | `2` |
| A random install number | 32 random hex digits, **replaced every month** |

The random number lets reports from one month be counted as one phone's, so
that one keen player does not count as fifty. It is made on the phone from
random bytes, is not derived from anything about you or the device, and a
new one is made each calendar month, so two months cannot be linked.

### What is never sent

No name, account or e-mail. No file names or folder paths. Nothing about
your disc or game files, and no hashes of them. No location. No contacts.
No advertising ID, serial number or other device identifier. No player
name, and nothing about who you played with.

The app builds a report from the list above and from nothing else, and the
server refuses a report that has any other field.

### What the server does

- **It stores no IP address.** Your address is used only to limit how many
  reports one address can send (four at once, then six an hour). For that
  it is held in memory as a number scrambled with a key that exists only
  while the server runs. It is never written to the database or to a log,
  and the web server in front writes no access log.
- A stored report has the **day** it arrived, not the time.
- Reports are capped at 2,048 bytes and anything malformed is refused.
- Reports older than **12 months** are deleted.
- The public page shows a row only for groups of **at least 5 reports from
  at least 5 different install numbers**, as medians, so no single
  player's session can be read from it.
- Reports are sent over HTTPS only.

How it is built and run: [docs/perf-reports.md](docs/perf-reports.md). The
server's source is in [`relay/src/perf.rs`](relay/src/perf.rs) and the
app's in [`PerfReports.kt`](android/app/src/main/java/com/mw2/recomp/PerfReports.kt).

### Changing your mind

Turn the switch off in Settings and nothing more is sent; reports that
were waiting to be sent are deleted from the phone. Reports already sent
carry nothing that says which are yours, so they cannot be picked out and
removed one by one; they are deleted when they are 12 months old.

Questions: [open an issue](https://github.com/yassinsolim/mw3-recomp/issues/new/choose).

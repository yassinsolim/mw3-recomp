# Performance reports

Players who say yes send a short anonymous report after each session, and
a public page shows what each graphics chip sustains, so the defaults can
be tuned for chips nobody on the project owns. What a report holds and
what is never sent is in [PRIVACY.md](../PRIVACY.md), which is the promise;
this page is how it is kept, and how the server is put up and taken down.

| Part | Where |
|---|---|
| The run's numbers (frames a second, 1% low, what held it back, the map) | `runtime/adaptive.h` (`Session`, `Tally`), `runtime/kernel/files.cpp` (`LevelAsked`), `nativeSession` in `runtime/android/jni.cpp` |
| The report, the choice, the queue, the screens | `android/app/src/main/java/com/mw2/recomp/PerfReports.kt` |
| The server: ingest, storage, summary, page | `relay/src/perf.rs`, in the relay's own program |
| Putting the server up, checking it, taking it down | `relay/perf-setup.sh` and the runbook below |

## Deploying it (runbook)

The relay runs on the Oracle Cloud VM `minecraft-prod-01`
(`mw3.yassin.app`), reached only from F-117 with
the scripts in `homelabstuff\mw3-relay`. Until now it listened on UDP 3074
alone. Reports need HTTPS on the same name, so three things are added:
two ports, Caddy in front (it holds a Let's Encrypt certificate and passes
on `/perf` only), and two settings for the relay. All of it is on the VM
that is already there: **no new Oracle resource, nothing outside Always
Free** (security-list rules, Caddy and Let's Encrypt cost nothing; the
reports are a few megabytes on the boot volume).

Do the steps in order. Every step is safe to repeat.

### 1. Oracle console: let TCP 80 and 443 in (the only step that is not a command)

Nothing on the VM can open this; without it Caddy gets no certificate and
step 3 says so.

1. Oracle Cloud console, region **US West (Phoenix)**: menu **Networking**,
   **Virtual cloud networks**, **minecraft-vcn**.
2. **Security Lists**, **Default Security List for minecraft-vcn** (the one
   that already has UDP 3074 "mw2-relay").
3. **Add Ingress Rules**, and fill in:

   | Field | Value |
   |---|---|
   | Stateless | unticked |
   | Source Type | CIDR |
   | Source CIDR | `0.0.0.0/0` |
   | IP Protocol | TCP |
   | Source Port Range | leave empty (All) |
   | Destination Port Range | `80` |
   | Description | `mw3-perf: Caddy, certificate challenge and redirect` |

4. **+ Another Ingress Rule**, the same but Destination Port Range `443`
   and Description `mw3-perf: Caddy, HTTPS for /perf`. Then **Add Ingress
   Rules**.

DNS needs nothing: `mw3.yassin.app` already points at the VM, DNS only
(not proxied), which is what Caddy's certificate needs.

### 2. Update the relay to a build that takes reports

On F-117, in `homelabstuff\mw3-relay`, with the commit this page was merged
in (the merge note names it) or any later one on `android`:

```powershell
.\Update-Mw3Relay.ps1 -Commit <commit>
```

This is the usual update: tests, build, restart, check, and the previous
build put back if the new one fails. The new build behaves exactly as the
old one until step 3: it takes reports only when told where to keep them.
The build now compiles SQLite (the `rusqlite` crate, bundled), which needs
the C compiler `mw3-relay setup` already installed (`build-essential`).

### 3. Turn reports on

```powershell
. .\Mw3RelayCommon.ps1
Invoke-Mw3RelayHost -Script @'
src=$(sudo sh -c 'ls -d /home/mw3build/src/<commit>*/relay' | head -n 1)
sudo install -m 0755 "$src/perf-setup.sh" /usr/local/sbin/mw3-relay-perf
sudo /usr/local/sbin/mw3-relay-perf enable
'@
```

(`<commit>` as in step 2: the update left that commit's `relay/` folder on
the VM, and the script is in it.) `enable` does, and prints, each of these:

| What | Exactly |
|---|---|
| Ports | Refuses to go on if anything but Caddy already listens on TCP 80 or 443. |
| iptables | `-A INPUT -p tcp -m multiport --dports 80,443 -m comment --comment mw3-perf -j ACCEPT`, inserted live before Oracle's final `REJECT` and written into `/etc/iptables/rules.v4` the same way (backup beside it). The file is never reloaded, as with the relay's own rule: that would drop Docker's chains and Minecraft's ports. |
| The relay's unit | `mw2-relay.service` itself is **not edited**. A drop-in, `/etc/systemd/system/mw2-relay.service.d/perf.conf`, adds `Environment=MW2_RELAY_PERF_PORT=3076`, `Environment=MW2_RELAY_PERF_DB=/var/lib/mw3-relay/perf.sqlite`, `StateDirectory=mw3-relay` and `StateDirectoryMode=0750`. So the unit `Update-Mw3Relay.ps1` installs with every build stays as it is, the sandbox stays as it is (the state directory is the one new writable path), and a build without reports ignores both variables. The same as flags: `--perf-port 3076 --perf-db /var/lib/mw3-relay/perf.sqlite`. |
| The relay | Restarted once, if it does not answer `http://127.0.0.1:3076/perf/health` yet (connected game copies reconnect; the script prints how many there are). It listens on 127.0.0.1 only; port 3076 is opened nowhere. |
| Caddy | `apt-get install --no-install-recommends caddy` (that package alone; nothing else is upgraded), with `/etc/caddy/Caddyfile` written first (below). |
| Check | Waits up to two minutes for `https://mw3.yassin.app/perf/health` to answer through Caddy with a valid certificate. If it does not, it says so plainly, and names step 1 as the usual reason. |

The Caddyfile it writes:

```
{
	log {
		exclude http.log http.stdlib
	}
}

mw3.yassin.app {
	@perf path /perf /perf/*
	handle @perf {
		request_body {
			max_size 4KB
		}
		reverse_proxy 127.0.0.1:3076 {
			header_up X-Forwarded-For {remote_host}
		}
	}
	handle {
		respond 404
	}
}
```

No `log` directive for the site, so there is no access log; the global
`exclude` also drops the two loggers that would name a client's address
when a request fails. `X-Forwarded-For` is how the relay knows which
address to rate-limit; it keeps that in memory only (see below).

### 4. Check from outside

From F-117 (or anywhere that is not the VM):

```powershell
curl.exe -sS https://mw3.yassin.app/perf/health      # {"ok":true}
curl.exe -sS https://mw3.yassin.app/perf/summary     # {"groups":[],"smallest_group":5}
curl.exe -sS -o NUL -w "%{http_code}`n" https://mw3.yassin.app/           # 404
curl.exe -sS -o NUL -w "%{http_code}`n" -X POST -d "{}" https://mw3.yassin.app/perf   # 400
python relay-probe.py mw3.yassin.app --hold 8        # the relay itself still takes players
```

A timeout on the first line with step 3 saying DONE means step 1 is
missing or wrong. On the VM, `sudo mw3-relay-perf check` prints the drop-in,
the listening socket, the database file, the firewall rule, Caddy's state
and the four status codes through Caddy.

Once `/perf/health` answers, the apps start asking their players (each app
looks once a day), and nothing else has to be released.

### Undo

```powershell
Invoke-Mw3RelayHost -Script 'sudo /usr/local/sbin/mw3-relay-perf disable'
```

Stops and removes Caddy, removes its Caddyfile, deletes the iptables rule
(live and in the file), and removes the drop-in. The relay is restarted
only if no game copy is connected; otherwise it stops taking reports at its
next start, and nothing reaches it meanwhile because the ports are closed.
The reports stay in `/var/lib/mw3-relay`; `disable --delete-reports`
removes them too. Then delete the two ingress rules of step 1 in the
console. The apps notice within a day (`/perf/health` no longer answers),
stop asking new players, and keep what was waiting for 30 days at most.
The relay's own rollback (`mw3-relay rollback`) is unaffected: an older
build ignores the drop-in.

### On another host

Anyone running their own relay can take reports the same way:
`mw2-relay --perf-port 3076 --perf-db /some/file.sqlite` (add
`--perf-address 0.0.0.0` only behind something that terminates TLS), and
an app built with `MW2_PERF_URL=https://their.host/perf`.

## The server

`relay/src/perf.rs`, a thread of the relay's own, so there is one program
to build and watch. Plain HTTP on 127.0.0.1; Caddy holds the certificate.

| Request | Answer |
|---|---|
| `GET /perf/health` | `{"ok":true}`: reports are taken here. The app asks this before it offers the choice. |
| `POST /perf` | One report as JSON. 200 kept; 400 not a report; 413 over 2,048 bytes; 429 too many from this address; 503 no room. |
| `GET /perf/summary` | The groups as JSON (below), answered from memory for ten minutes. |
| `GET /perf` | The same as a page: a table for each graphics chip. |

**What is kept.** One SQLite table, `reports`, whose columns are the
report's fields and the day it came (days since 1970, UTC; no time). A
report is checked field by field against a list in the code: exactly those
fields, each of its kind (a name of at most 64 letters, digits, spaces and
`._+-(),:@#`, so never a slash; one of a few words; a number in a range).
Anything else is refused whole, so a later version of the app cannot start
sending more without the server being changed too. The same report sent
twice (an answer lost on the way) is kept once.

**No address.** The address a report comes from (Caddy's
`X-Forwarded-For`, or the connection's own when nothing is in front) is
used for the rate limit alone: it is hashed with a key made at start and
held in memory, in a table of at most 4,096 entries that forgets an address
an hour after its last report. It is not a column, it is not logged (the
relay logs one line at start, `performance reports: taken, N held; no
address is kept or logged`, and nothing for each report), and Caddy writes
no access log. `relay/tests/perf.rs` sends reports with a known address
through the real program and then searches the database file and the log
for it.

**Limits.** 2,048 bytes a report (refused by its declared length, before
it is read; Caddy caps the body at 4 KB before that); four reports at once
and six an hour from one address; sixty at once and one a second from
everyone; 1,000,000 rows (about 200 MB), after which reports are refused
until old ones age out; three seconds for a client to send its request.

**Retention.** Rows older than 365 days are deleted at start and once a
day.

**The summary.** A group is one graphics chip, game and set of settings
(preset, scene scale, shadows, FPS limit, multiplier). It is shown once it
has at least 5 reports from at least 5 different install numbers, as the
median FPS, the median 1% low and the count:

```json
{"groups":[{"fps_limit":60,"game":"mw3","gpu":"Turnip Adreno (TM) 740","median_fps":58.6,"median_low1":41.2,
            "multiplier":1,"preset":"automatic","reports":7,"scene_scale":70,"shadows":"auto"}],
 "smallest_group":5}
```

Nothing of a single report (phone model, map, install number, day) is in
it.

## The app

**Asking.** Off unless the player says yes. The run of a player who has
not answered leaves one mark after a minute of a session, that one was
played, and nothing else; once they have answered, yes or no, it leaves
none. When the
launcher is next opened and that mark is there, it asks
`GET https://mw3.yassin.app/perf/health` (once a day at the most, the
answer remembered) and shows the question only if the server answered: no
player is asked while there is nowhere to send, and nothing is measured or
queued before the answer is yes. The question is one screen: what it is
for, every field, what is never sent, where to change it, the link to
PRIVACY.md, and **Yes, send them**, **No** and **Show me an example**
(a filled-in report, readable and as it is sent). It is asked once;
leaving it without an answer is a no.

**Settings, Network, Performance reports** has the switch (turning it on
asks the server anew whether it answers, whatever was remembered that
day), **Show me an example** and **Reports
sent**, which lists every report that left the phone, the newest first,
with those still waiting counted above. Turning it off deletes what was
waiting.

**A session.** For a player who said yes, the run writes what its session
has come to every ten seconds (a run a player ends is killed, and says
nothing at its end): from the runtime, the seconds of play, the frames a
second, the 1% low (the frames a second of the slowest hundredth of the
frames of play, from a histogram of every frame's time), what the frames
waited for in most of the two-second windows the adaptive rule measures
(the graphics chip where it took 85% of a frame's time by its own clock,
otherwise the processor; unknown where the chip keeps no clock), the map
(the name the game asks for its loading screen by, which is the map's name)
and the graphics chip and driver; from the app, the game, the mode, the
settings in use and the hottest thermal status so far. Menus, loading
screens and films are not play and are not counted. A session is one run
of the game from start to quit: where it went through several maps, the
numbers are of all of them and the map named is the last. The next time the
launcher opens it makes the report: the fields of the list and no others,
whatever the file holds; each name cleaned of anything that is not a
letter, digit or plain punctuation; a session under a minute dropped.

**Sending.** HTTPS only (the app refuses any other address, but for a
debug build pointed at `http://127.0.0.1`, below). A report that could not
be sent waits in a queue of at most 20 for at most 30 days, and is tried
again whenever the launcher opens and, through a `JobScheduler` job, when
the phone next has a network. One the server refuses as not a report is
dropped; one it took is added to **Reports sent** (the last 50).

**The install number** is 16 random bytes, kept with the month it was made
in and replaced when the month changes.

The preset field is the one chosen in **Settings, Graphics**
(`Prefs.graphicsPresetName`): `automatic`, `battery saver`, `balanced`,
`best looking`, or `custom` once a setting of the Advanced page was changed
by itself.

## Tests

| What | Where | Run |
|---|---|---|
| The report: the listed fields and no other, nothing shaped like a path, a short session dropped, the install number new each month | `android/app/src/test/java/com/mw2/recomp/PerfReportTest.kt` | `gradlew :app:testMw3Tu0DebugUnitTest` |
| The session's sum: frames a second, 1% low, what held it back | `tests/adaptive_test.cpp` | `ctest -R adaptive` |
| The server's rules: only the listed fields, four at once and six an hour, the table of addresses, no address in the file, groups of five, twelve months | `relay/src/perf.rs` (`mod tests`) | `cargo test` in `relay/` |
| The server as a program: size cap, rate limit, no address in the file or the log | `relay/tests/perf.rs` | the same |

**End to end with a phone and a server on the PC.** A debug build may send
to the PC beside it:

```sh
cd relay && cargo run -- --public --port 3999 --perf-port 38080 --perf-db perf-test.sqlite
adb reverse tcp:38080 tcp:38080
cd android && MW2_PERF_URL=http://127.0.0.1:38080/perf ./gradlew -PappSuffix=.perf :app:assembleMw3Tu0Debug
```

Play a session of over a minute, close the game, open the launcher: the
question appears. Say yes, play another, open the launcher again, and the
report is in `perf-test.sqlite` and under **Reports sent**. With the
server stopped, the report waits (**Reports sent** says so) and goes when
the launcher is next opened with the server back.

**Measured on 27 November 2026** (the debug `.perf` apps, the relay from
this tree on the PC, `adb reverse`; every answer given through the real
screens):

- Retroid Pocket Flip 2, Modern Warfare 2 (`mw2-campaign`, PASS): the
  first session left only `files/perf/played`; the next launcher showed
  the question (behind the update offer, once that was closed); **Show me
  an example**, **Back**, **Yes, send them**. The second session's report
  (map `trainer`, 60.0 FPS, 1% low 43.3, held back by the processor) was
  sent when the run ended, and was in **Reports sent** and in the SQLite
  file. With `adb reverse` removed the next one waited (**Reports sent**:
  "1 report is waiting to be sent") and went when the `PerfSendJob` ran with
  the server back. With one waiting, turning the switch off deleted it, and
  a session after that kept nothing and sent nothing.
- AYN Thor, Modern Warfare 3 (`mw3-bots`, PASS): question, Yes, then a
  report of `mp_dome`, preset `automatic`, 119.0 FPS.
- AYN Thor, Advanced Warfare (`aw-campaign`, PASS): `seoul` (the
  briefing film's name), preset `automatic`, 119.0 FPS.
- Retroid Pocket Flip 2, Call of Duty 4 (`cod4-campaign`, PASS): `bog_b`,
  preset `automatic`, 59.9 FPS.

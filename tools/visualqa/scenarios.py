"""The standard scenarios: what each title is asked to do while it is recorded.

A scenario starts one title, loads a level or a match from the console
(`MW2_CONSOLE`), and walks the player through it with a pad script
(`MW2_INPUT_SCRIPT`) for a fixed time. The suite is one campaign level and
one match for every title, and Survival where the title has it;
`visualqa.py suite` runs them all.

A scenario is calm, a fight, or calm and then a fight. Shots, sparks and
explosions light patches of the picture for single frames, which is what a
flickering shadow does too, so the single-frame checks judge the calm part
alone: a match is walked through empty first, and its bots join (and the
player fires) from `combat_from`.
"""
from dataclasses import dataclass, field

# The disc images a game is played from, by the names they have had in a
# phone's Download folder; the first one there is used.
DISC_IMAGES = {
    "mw2": ["mw2.iso", "Call of Duty - Modern Warfare 2.iso"],
    "mw3": ["mw3.iso", "Call of Duty - Modern Warfare 3.iso"],
    "cod4": ["Call of Duty 4 - Modern Warfare (USA, Europe).iso", "cod4.iso"],
    "aw": ["Call of Duty - Advanced Warfare (USA, Europe) (En,Fr,De,Es,It,Pl,Ru) (Game Disc).iso", "aw.iso"],
}

# The games the owner's app is not read for, and the folder in a phone's
# storage their test files are pushed to once instead. Advanced Warfare's
# install is the owner's and eleven gigabytes; the folder has the two
# executables (`aw/tu0/default.xex`, `default_mp.xex` of a set-up checkout)
# and the install disc's two packages (`aw/install/content/.../00000002/*`),
# and the game is played from the Game Disc's image like the others.
TEST_FILES = {
    "aw": "/sdcard/Download/visualqa-aw",
}

# The devices the suite runs on: the name of each one's lock, and its serial.
DEVICES = {
    "thor": "f4ecf93d",  # AYN Thor, Adreno 740, 120 Hz
    "flip": "f315edc9",  # Retroid Pocket Flip 2, Adreno 650, 60 Hz
}


def walk(start, end, fire_from=None):
    """A pad script that walks and turns from `start` to `end` seconds, and fires from `fire_from`.

    Forward for 2.4 s of every 3, a turn between (left and right by turns of
    unequal length, so the path does not close on itself), a sidestep and a
    jump now and then, and in a fight a burst of fire every twelve seconds:
    enough to load new parts of a level, cross lit and shadowed ground and
    draw the weapon's effects.
    """
    turns = ("rx+:0.25", "rx-:0.5", "rx-:0.35", "rx-:0.35", "rx-:0.5", "rx-:0.25", "rx+:0.5", "rx+:0.25",
             "rx-:0.35", "rx+:0.5", "rx+:0.5", "rx+:0.25")
    entries = []
    step = 0
    at = float(start)
    while at + 3.0 <= end:
        entries.append(f"{at:.1f}:ly+:2.4")
        entries.append(f"{at + 2.5:.1f}:{turns[step % len(turns)]}")
        if fire_from is not None and at >= fire_from and step % 4 == 1:
            entries.append(f"{at + 1.0:.1f}:rt:0.4")
        if step % 7 == 4:
            entries.append(f"{at + 0.5:.1f}:lx+:1.2")
        if step % 9 == 6:
            entries.append(f"{at + 0.2:.1f}:a")
        at += 3.0
        step += 1
    return ",".join(entries)


@dataclass
class Scenario:
    name: str
    game: str                 # mw2, mw3, cod4, aw
    title: str                # CAMPAIGN or MULTIPLAYER: which of the app's two titles
    seconds: int              # the run's length (MW2_RUN_SECONDS)
    console: str              # MW2_CONSOLE
    inputs: str               # MW2_INPUT_SCRIPT up to play; the walk is added from `play_from`
    play_from: float          # seconds of the run from which the picture is judged as play
    what: str                 # a line for the report
    # Seconds of the run from which there is a fight on screen; None for a scenario that stays calm.
    combat_from: float = None
    options: dict = field(default_factory=dict)
    # Seconds of play (from the log's "in play at") after which the level is over by its own design
    # and what the screen shows is no longer judged; None for a level that lasts the run.
    over_after: float = None

    def over_at(self, log):
        """The second of the run from which nothing is judged, or None."""
        if self.over_after is None or log is None or log.in_play_at is None:
            return None
        return log.in_play_at + self.over_after

    def input_script(self):
        steps = walk(self.play_from, self.seconds - 2, self.combat_from)
        return f"{self.inputs},{steps}" if self.inputs else steps

    def switches(self, dump_folder=None, dump_every=0):
        values = {
            "MW2_REPORT": "1", "MW2_REPORT_SECONDS": "1", "MW2_STUTTERS": "1",
            "MW2_RUN_SECONDS": str(self.seconds), "MW2_CONSOLE": self.console,
            "MW2_INPUT_SCRIPT": self.input_script(),
        }
        if dump_folder and dump_every:
            values["MW2_DUMP_FRAMES"] = dump_folder
            values["MW2_DUMP_FRAME_EVERY"] = str(dump_every)
            values["MW2_DUMP_FRAMES_AFTER_SECONDS"] = str(int(self.play_from))
        values.update(self.options)
        return values


# Start five times over the first twenty seconds: the title screen, and the
# screens a first run puts before the menu. Then A as the level opens: a
# fresh install has no save device and the title asks before it goes on.
_TO_LEVEL = "8:start,11:start,14:start,17:start,20:start,50:a,55:a,60:a"
# The multiplayer's title screen, then A through the team and class menus of a match.
_INTO_MATCH = "18:start,50:a,54:a,58:a,62:a,66:a"
_LOCAL_MATCH = "30:set onlinegame 0;30.5:set systemlink 1;33:map "
# A match is empty for its first forty seconds of play and has six bots after.
_MATCH_PLAY = 70
_MATCH_FIGHT = 110
_BOTS_JOIN = f";{_MATCH_FIGHT - 4}:bots 6"

SUITE = [
    Scenario("mw2-campaign", "mw2", "CAMPAIGN", 150, "24:set ui_autoContinue 1;25:devmap trainer;65:god", _TO_LEVEL, 65,
             "Modern Warfare 2, S.S.D.D.: the camp in sunlight"),
    Scenario("mw2-bots", "mw2", "MULTIPLAYER", 170, "16:map mp_rust" + _BOTS_JOIN, _INTO_MATCH, _MATCH_PLAY,
             "Modern Warfare 2, Rust: empty, then six bots", combat_from=_MATCH_FIGHT),
    Scenario("mw3-campaign", "mw3", "CAMPAIGN", 170, "24:set ui_autoContinue 1;25:devmap sp_ny_manhattan;65:god", _TO_LEVEL,
             95, "Modern Warfare 3, Black Tuesday: the street fight", combat_from=95),
    # Modern Warfare 3's `map` opens a lobby and stops there: its match is started through the menus
    # (A on the title screen, the second entry, A on Start Game with the map chosen by its dvar).
    Scenario("mw3-bots", "mw3", "MULTIPLAYER", 170, "46:set ui_mapname mp_dome" + _BOTS_JOIN,
             "30:a,36:down,37:a,48:a,58:a,62:a,66:a", _MATCH_PLAY,
             "Modern Warfare 3, Dome: empty, then six bots", combat_from=_MATCH_FIGHT),
    # The first wave kills a player who only walks 38 to 57 seconds after play begins (`god` and
    # `notarget` do nothing in Special Ops, with `devmap` either), and the results screen then
    # stands still, which is not a frozen picture: the mission is judged for its first 35 seconds.
    Scenario("mw3-survival", "mw3", "CAMPAIGN", 100, "24:set specialops 1;25:map so_survival_mp_dome",
             _TO_LEVEL, 55, "Modern Warfare 3, Survival on Dome: the first wave", combat_from=55, over_after=35),
    Scenario("cod4-campaign", "cod4", "CAMPAIGN", 150, "25:devmap bog_b;65:god", _TO_LEVEL, 65,
             "Call of Duty 4, The Bog: the first visit that stalled", combat_from=65),
    Scenario("cod4-bots", "cod4", "MULTIPLAYER", 170, _LOCAL_MATCH + "mp_crash" + _BOTS_JOIN, _INTO_MATCH, _MATCH_PLAY,
             "Call of Duty 4, Crash: empty, then six bots", combat_from=_MATCH_FIGHT),
    Scenario("aw-campaign", "aw", "CAMPAIGN", 210, "24:set ui_autoContinue 1;25:devmap seoul;65:god", _TO_LEVEL, 65,
             "Advanced Warfare, Induction (Seoul): the drop and the street", combat_from=65),
    Scenario("aw-match", "aw", "MULTIPLAYER", 200, "45:set ui_mapname mp_prison",
             "20:down:0.07,22:down:0.07,24:a,38:down:0.07,40:a,54:a,100:a,106:a,112:a", 125,
             "Advanced Warfare, Riot in a private match, empty: its sun shadows"),
]


def named(name):
    for scenario in SUITE:
        if scenario.name == name:
            return scenario
    raise SystemExit(f"no scenario {name}: one of {', '.join(scenario.name for scenario in SUITE)}")

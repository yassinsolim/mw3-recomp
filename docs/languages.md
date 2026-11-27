# Languages

Two things have a language: the app (the launcher, setup, Settings, the
in-game menu) and the game itself, whose text and voices come from the
player's own disc.

## The app

The five apps share one codebase (`android/app`) and follow the phone's
system language (Android picks `values-<locale>/`; Android 13's per-app
language in the system settings works as well). There is no picker in the
app.

| Folder | Language |
|---|---|
| `values` | English (the source) |
| `values-es` | Spanish |
| `values-pt-rBR` | Portuguese (Brazil) |
| `values-fr` | French |
| `values-de` | German |
| `values-it` | Italian |
| `values-ru` | Russian |
| `values-pl` | Polish |
| `values-tr` | Turkish |
| `values-ar` | Arabic (right to left; the manifest has `supportsRtl`) |
| `values-zh-rCN` | Simplified Chinese |
| `values-ja` | Japanese |
| `values-ko` | Korean |
| `values-in` | Indonesian (`in` is Android's folder code for `id`) |

Each source set has them: `main/res/values-*/strings.xml` and
`strings_camo.xml`, and each game's flavour (`mw2`, `cod4`, `aw`, `bo3`) its
own `values-*/strings.xml` for the strings it words differently. A flavour's
string must be translated in the flavour too: otherwise main's translation,
which is more specific than the flavour's English, is shown.

Not translated (`translatable="false"`): the games' wordmarks, the credit
line and the setup command lines. Game titles, file names, paths, commands,
dvars and units stay as they are inside translated sentences. The app's name
comes from the flavour (`resValue`) and is the game's title in every
language.

`LocaleResourcesTest` (`gradlew :app:testMw2Tu0DebugUnitTest`) reads every
`values-*` file against its English: each string must exist and be
translatable in English, keep the same format placeholders (`%1$s`, `%2$d`,
`%1$.2f`) and `%%`, and format with the English's arguments without an
exception, which on a phone would be a crash in that language only.

A new English string with no translation is shown in English: add it to each
`values-*` file (or leave it, and lint's MissingTranslation lists it).

What stays English: the log, the performance report, the device details and
the native runtime's own messages (an install's or the renderer's reason for
failing, which the app shows inside a translated sentence), because they are
read by whoever looks into a bug report.

## The game

What each game's Xbox 360 disc holds, read from the images and packages on
the development PC (`tools/xdvdfs.py`, `tools/stfs.py`), and what the title
reads to choose a language.

| Game | The disc this port takes | Languages on it | How the title chooses | What the port does |
|---|---|---|---|---|
| Modern Warfare 2 | USA/Europe | English: the text is inside each zone (`common.ff`, the levels'), with no language folders | `XGetLanguage` (an import of the executable) | English. A disc of another language is another pressing; none was on the development PC, so whether its executable is the same is not known. The runtime takes only the executable it was built from (by its SHA-256): a pressing with another one is refused, and one with the same would play in its own language |
| Modern Warfare 3 | USA/Europe (and one other packaging of the campaign) | English, as Modern Warfare 2 | `XGetLanguage` | English, as Modern Warfare 2 |
| Call of Duty 4 | USA/Europe | Not looked at: its game files were not on the development PC. The engine before Modern Warfare 2's keeps its text in its zones the same way, so one language a pressing is expected | not looked at | English, as before |
| Advanced Warfare | USA/Europe game disc and install disc | The game disc has its levels' text only in English (`english/eng_<level>.ff`, 95 files); the install disc has `code_post_gfx`, `common`, `ui` in nine languages and a sound pack each, which is not enough for a level in another language. A title update of another pressing (Latin American, Spanish) has its patch's text in that language only, which the runtime already reads for its tables (`aw_update_language.cpp`) | `XamGetLocaleEx` (campaign), `XamGetLanguage` and `XamGetLocaleEx` (multiplayer) | English |
| Black Ops III | USA/Europe (En, Fr, De, Es, It, Pt, Pl, Ru) | Nine: `<xx>_core_*.ff` and `<xx>_mp_gfx.ff` for `en`, `fr`, `ge`, `it`, `es`, `ms` (the Americas' Spanish), `po` (Polish), `ru`, `bp` (Brazilian Portuguese); voices in `snd/<xx>/` for all but Polish. Title update 5 and Compatibility Pack 1 have the same nine (31 and 22 fastfiles each), Compatibility Pack 2, The Giant and Awakening those and `ja`, `fj`, `ea` | `XGetLanguage` (the XConfig user language, setting 9): German, French, Italian, Portuguese, Polish and Russian are theirs; Spanish is `mexicanspanish` with a locale of the Americas (`XamGetLocaleEx`: Canada, Chile, Colombia, Mexico, the United States, Argentina) and `spanish` otherwise; Japanese, Korean and Chinese are English on this pressing (sub_826D8900) | The phone's language |

### Black Ops III in the phone's language

The runtime tells the title the dashboard language and locale in
`MW2_LANGUAGE` and `MW2_LOCALE` (the console's `XC_LANGUAGE_*` and
`XC_LOCALE_*` numbers; English and the United States when unset):
`XGetLanguage`, `XamGetLanguage`, the XConfig user language
(`ExGetXConfigSetting(3, 9)`, which the title's own XGetLanguage reads) and
`XamGetLocaleEx` answer with them (`runtime/kernel/misc.cpp`).

The app sets both from the phone's language for Black Ops III only
(`GameLanguage.kt`): German 3, French 4, Spanish 5, Italian 6, Portuguese 9,
Polish 11, Russian 12, with the phone's country as the locale (Spanish with
no locale of its own, `es-419` among them, is Mexico's, so the Americas'
Spanish). Any other language is English, as the title itself would make a
Japanese, Korean or Chinese console on this pressing. The other four games
are told English whatever the phone says, as before: their discs have no
other language to show.

Nothing is added or downloaded: the languages are the player's disc's,
update's and packs'.

Seen on Windows (RTX 5080, the multiplayer `bo3`, a fresh profile each run,
frames dumped and looked at), with the disc, title update 5 and the packs:

| `MW2_LANGUAGE`, `MW2_LOCALE` | What the title loaded and showed |
|---|---|
| unset | `en_core_*.ff`; "Press (A) to Start" |
| 3, 13 (German, Germany) | `ge_core_*.ff`, `ge_mp_frontend.ff`; "(A) zum Starten", "Sicherheitsbereich anpassen", MEHRSPIELER, LOKAL, SPIEL STARTEN, a Team-Deathmatch on Combine counting down "Spielstart in 2" |
| 12, 37 (Russian, Russia) | `ru_core_*.ff`; СЕТЕВАЯ ИГРА, ЛОК. МАТЧ, НАЧАТЬ ИГРУ, the Cyrillic drawn by the title's own fonts |
| 5, 22 (Spanish, Mexico) | `ms_core_*.ff`; MULTIJUGADOR, VÍA LIBRE, ZOMBIS, BUSCAR PARTIDA LAN |

No `Com_Error` and no error line in any of the four logs. Not yet seen on a
phone, and the zombies title (`bo3zm`) not run in another language.

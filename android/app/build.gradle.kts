import java.security.MessageDigest

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

// The native build is the repository's own CMakeLists.txt, two folders up.
// Nothing about the runtime is duplicated here: the same project that makes
// the desktop executable makes libmw2.so, with ANDROID set (cmake/android.cmake).
val repositoryRoot = file("../..")

// What a build machine may set, so that a CI job does not need a patched copy
// of this file (.github/workflows/android.yml):
//
//   MW2_NDK_VERSION        which NDK to use, when the runner has another one
//   MW2_VERSION_NAME       the release this apk is, without the leading "v"
//                          ("0.0.2-alpha"); the workflow takes it from the tag
//   MW2_VERSION_CODE       the number Android compares to decide whether an apk
//                          is an update; the workflow sets it when it must
//   MW2_NATIVE_OPTIMISATION  -O2 compiles the recompiled tree a good deal
//                          quicker than -O3 and runs within a few percent of
//                          it; a runner with six hours cares about that
//   MW2_COMPILER_LAUNCHER  ccache, usually
//   MW2_XEX_SHA256_SP/MP   the hashes the installer checks a player's own
//                          copy against, for a build that has the recompiled
//                          sources but not the disc they came from
//   MW2_KEYSTORE and friends  a signing key, so the apk is installable
fun setting(name: String): String? = System.getenv(name)?.trim()?.ifEmpty { null }

// -PmpInstance=2: a copy of Modern Warfare 3's multiplayer alone, as an app
// of its own beside the game (com.mw3.recomp.mp2, "CoD: MW3 Multiplayer (2)"),
// so that two can play each other on one device (docs/MW3-MP-BRINGUP.md).
// Debug builds only. It is the game's libmw3mp.so; the copy has its own
// settings, game folder and saves.
val mpInstance = (findProperty("mpInstance") as String?)?.toIntOrNull()?.coerceIn(1, 9) ?: 1
// Where the native build of an app that carries both titles (the mw3 and mw2
// flavours) leaves the multiplayer's library (cmake/android.cmake,
// MW2_MP_LIBRARY_DIRECTORY), under the ABI's name as jniLibs folders are.
val mw3mpLibraries = layout.buildDirectory.dir("mw3mp-jni").get().asFile
val mw2mpLibraries = layout.buildDirectory.dir("mw2mp-jni").get().asFile
val cod4mpLibraries = layout.buildDirectory.dir("cod4mp-jni").get().asFile
val awmpLibraries = layout.buildDirectory.dir("awmp-jni").get().asFile
val bo3zmLibraries = layout.buildDirectory.dir("bo3zm-jni").get().asFile
// The app with the whole game (the mw3 and mw2 flavours) is published without
// the recompiled code, which each player builds from their own disc into a
// package of its own (android/game, docs/game-library.md) that this app loads
// it from. MW2_SPLIT=OFF builds the app the old way instead, with the code in
// its own libraries: for telling a fault of the split from a fault of the game.
val gameSplit = (setting("MW2_SPLIT") ?: "runtime") != "OFF"
// -PappSuffix=.dev: the app and its game package under other names
// (com.mw3.recomp.dev, com.mw3.recomp.dev.game), so that a build signed with
// this machine's debug key installs beside the published app instead of being
// refused as an update of it. Its game folder and saves are its own.
val appSuffix = (findProperty("appSuffix") as String?).orEmpty()
val mw3ApplicationId = "com.mw3.recomp$appSuffix"
val mw2ApplicationId = "com.mw2.recomp$appSuffix"
val cod4ApplicationId = "com.cod4.recomp$appSuffix"
val awApplicationId = "com.aw.recomp$appSuffix"
val bo3ApplicationId = "com.bo3.recomp$appSuffix"

// Where the MW2 flavours' copy of the bundled graphics driver is put
// (bundledDriverAssets, at the end of this file).
val mw2DriverAssets = layout.buildDirectory.dir("mw2-driver-assets").get().asFile

// The graphics driver the apps bundle and what loads it, both fetched by
// android/fetch_deps.sh and neither committed: the Turnip package under
// src/mw3/assets/drivers and third_party/libadrenotools. An app built without
// either runs on the phone's own driver and only its log says so, and on an
// Adreno 740 that is a picture in 32-pixel blocks at half the frame rate:
// test apps built from worktrees that had neither were taken for a
// regression of the renderer for a night. So the app is not built without
// them unless the build says it means to: -PwithoutBundledDriver=true
// (setup\setup.ps1, when the driver could not be downloaded).
val withoutBundledDriver = (findProperty("withoutBundledDriver") as String?) == "true"
val bundledDriverPackage = file("src/mw3/assets/drivers/turnip_v27_r2.zip")
val adrenotoolsSources = rootProject.file("../third_party/libadrenotools/CMakeLists.txt")
val bundledDriverCheck = tasks.register("bundledDriverCheck") {
    doLast {
        val missing = listOf(bundledDriverPackage, adrenotoolsSources).filterNot { it.isFile }
        if (missing.isNotEmpty() && !withoutBundledDriver) {
            throw GradleException(
                "The bundled graphics driver is incomplete: ${missing.joinToString { it.path }} " +
                    "is not there. Run android/fetch_deps.sh (bash), which fetches both, and build again. " +
                    "To build an app that runs on the phone's own driver on purpose, add -PwithoutBundledDriver=true."
            )
        }
    }
}
tasks.matching { it.name == "preBuild" }.configureEach { dependsOn(bundledDriverCheck) }

// The app's own sources have to be here. They went missing once -- written,
// built against, and never committed -- and the result was an apk that
// installed, launched, and died on a manifest pointing at classes that did
// not exist. An empty source tree is not a warning, it is a broken build.
val kotlinSources = fileTree("src/main/java") { include("**/*.kt") }.files
if (kotlinSources.size < 10) {
    throw GradleException(
        "Only ${kotlinSources.size} Kotlin sources under android/app/src/main/java. " +
            "The app cannot be built from this tree -- check that they are committed " +
            "(git ls-files '*.kt')."
    )
}

android {
    namespace = "com.mw2.recomp"
    compileSdk = 35
    // r27+ aligns a library for 16 KB pages by default and clang is new enough.
    ndkVersion = setting("MW2_NDK_VERSION") ?: "28.2.13676358"

    defaultConfig {
        applicationId = "cod.modern.warfare2"
        // Android 8.0. AAudio, ASurfaceControl-free presenting, and the
        // Vulkan 1.1 drivers this renderer needs all predate it; below that
        // there is no device with the memory to run this anyway.
        minSdk = 26
        targetSdk = 35
        // Bump EVERY published release: Android refuses to install an update
        // whose versionCode is at or below the installed one. The release
        // workflows set MW2_VERSION_CODE from the tag (v1.2.3 is 10203,
        // docs/android-releases.md); this is what a build without one carries.
        // 107: v0.1.7, over 106 (v0.1.6), 105, 104, 103, 102, 101, 100 and 16
        // (v0.0.2-alpha). Call of Duty 4's and Advanced Warfare's releases
        // count from v0.1.0 (100), each in a repository of its own.
        versionCode = setting("MW2_VERSION_CODE")?.toIntOrNull() ?: 107
        // Which build this is, carried inside the apk. "The wrong apk" has
        // cost more than one evening, and every time the only way to settle
        // it was to guess. The launcher shows this and Android's app info
        // shows it, so the file on the phone can be matched against the run
        // that made it without downloading anything twice.
        // The release the build is from, which is what a report has to say to
        // be acted on. It was "1.0" -- a number no release ever had.
        versionName = "${setting("MW2_VERSION_NAME") ?: "0.1.7"} " +
            "(${setting("MW2_BUILD_SHA") ?: "local"})"
        buildConfigField("String", "BUILD_SHA", "\"${setting("MW2_BUILD_SHA") ?: "local"}\"")
        buildConfigField("String", "BUILD_STAMP", "\"${setting("MW2_BUILD_STAMP") ?: "unstamped"}\"")
        buildConfigField("int", "MP_INSTANCE", "1")
        // The package the recompiled code is loaded from, for an app that is
        // built without it (the mw3 and mw2 flavours); the app asks the
        // system where that package is, which it may only when the manifest
        // names it.
        buildConfigField("String", "GAME_PACKAGE", "\"\"")
        manifestPlaceholders["gamePackage"] = "$mw3ApplicationId.game"
        // Where the releases are listed, for a build that tests updating
        // against a server of its own (docs/android-releases.md); empty for
        // GitHub's list of UPDATE_REPO's.
        buildConfigField("String", "UPDATE_API", "\"${setting("MW2_UPDATE_API") ?: ""}\"")
        // Where the toolchain package is fetched from, for a build that tests
        // the download against the same server; empty for the release
        // setup/toolchain.json names. The file's SHA-256 is that list's either way.
        buildConfigField("String", "TOOLCHAIN_URL", "\"${setting("MW2_TOOLCHAIN_URL") ?: ""}\"")
        // Where performance reports go, for a player who chose to send them
        // (docs/perf-reports.md): HTTPS always, but for a debug build pointed
        // at a server on the PC beside it (MW2_PERF_URL=http://127.0.0.1:...).
        buildConfigField("String", "PERF_URL", "\"${setting("MW2_PERF_URL") ?: "https://mw3.yassin.app/perf"}\"")
        // The toolchain package's name: -PtoolchainSuffix=.test for a build
        // that tries an update of it beside the published package, which the
        // published apps on the same device go on using.
        val toolchainPackage = "com.recomp.toolchain" + (findProperty("toolchainSuffix") as String?).orEmpty()
        buildConfigField("String", "TOOLCHAIN_PACKAGE", "\"$toolchainPackage\"")
        manifestPlaceholders["toolchainPackage"] = toolchainPackage

        ndk {
            // The guest's address space is 4 GiB reserved in one mapping, so
            // a 32-bit process cannot host it at all.
            abiFilters += "arm64-v8a"
        }

        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DANDROID_STL=c++_shared",
                    // The runtime's optional parts, all off: they cost frames
                    // and the phone has none to spare.
                    "-DMW2_DIAGNOSTICS=ON",
                    "-DMW2_USE_SDL=OFF",
                    "-DMW2_LOGGING=ON",
                    "-DCMAKE_BUILD_TYPE=Release",
                    // A tree configured before libadrenotools was fetched
                    // keeps building without it: the argument makes it be
                    // configured again, and CMake refuse a tree without it.
                    "-DMW2_REQUIRE_ADRENOTOOLS=${if (withoutBundledDriver) "OFF" else "ON"}",
                )
                setting("MW2_COMPILER_LAUNCHER")?.let {
                    arguments += listOf(
                        "-DCMAKE_C_COMPILER_LAUNCHER=$it",
                        "-DCMAKE_CXX_COMPILER_LAUNCHER=$it",
                    )
                }
                val sp = setting("MW2_XEX_SHA256_SP") ?: "990e640d3a92544f9d585e71faf1ab65082d37ec6f5865b2599335fa123bf6ff"
                val mp = setting("MW2_XEX_SHA256_MP") ?: "aacc47aff3221f3bd9129e80827fd3bcd74aa43bf74bb5c5ddbd0597c0d5fcde"
                arguments += listOf("-DMW2_XEX_SHA256_SP=$sp", "-DMW2_XEX_SHA256_MP=$mp")
                // TU6 builds verify the disc's executables against these, then
                // patch them on the device; the TU fastfiles' hashes gate the
                // downloaded update. TU0 builds leave all three unset (their
                // executables ARE the disc's).
                setting("MW2_DISC_SHA256_SP")?.let { arguments += listOf("-DMW2_DISC_SHA256_SP=$it") }
                setting("MW2_DISC_SHA256_MP")?.let { arguments += listOf("-DMW2_DISC_SHA256_MP=$it") }
                setting("MW2_UPDATE_SHA256_PATCH_MP")?.let { arguments += listOf("-DMW2_UPDATE_SHA256_PATCH_MP=$it") }
                setting("MW2_UPDATE_SHA256_DLC1_UI_MP")?.let { arguments += listOf("-DMW2_UPDATE_SHA256_DLC1_UI_MP=$it") }
                setting("MW2_UPDATE_SHA256_DLC2_UI_MP")?.let { arguments += listOf("-DMW2_UPDATE_SHA256_DLC2_UI_MP=$it") }
                cppFlags += (setting("MW2_NATIVE_OPTIMISATION") ?: "-O3")
            }
        }
        // The multiplayer's library as a title update makes it, for an app
        // whose game has one (Advanced Warfare's, below); none otherwise.
        buildConfigField("String", "LIBRARY_MP_UPDATE", "\"\"")
    }

    // The disc carries two executables and each is recompiled into a tree of
    // its own (build.sh TITLE=sp|mp). Each game is one app with both (the mw3
    // and mw2 flavours). Modern Warfare 2's were separate apps first, each
    // with its recompiled code inside, and still build that way (campaign,
    // multiplayer): it is how title update 6 is built (docs/MW2-ANDROID.md).
    //
    // LIBRARY_SP and LIBRARY_MP are what System.loadLibrary is given for the
    // campaign and for the multiplayer; empty for a title the app does not
    // carry (Title.kt).
    //
    // Each title comes in two game versions that never interoperate (TU0 plays
    // only with TU0, TU6 only with TU6): the second dimension. TU0 keeps the
    // original packages (com.mw2.recomp.sp/.mp) so updates install over it;
    // TU6 rides beside them as .sp.tu6/.mp.tu6. Variant tasks are therefore
    // assembleCampaignTu0Release, assembleCampaignTu6Release,
    // assembleMultiplayerTu0Release and assembleMultiplayerTu6Release
    // (see .github/workflows/build-apk.yml).
    flavorDimensions += "title"
    flavorDimensions += "tu"
    productFlavors {
        create("campaign") {
            dimension = "title"
            applicationIdSuffix = ".sp"
            resValue("string", "app_name", "CoD: Modern Warfare 2")
            buildConfigField("String", "UPDATE_REPO", "\"flavinxs0/mw2-android\"")
            buildConfigField("String", "LIBRARY_SP", "\"mw2\"")
            buildConfigField("String", "LIBRARY_MP", "\"\"")
            externalNativeBuild { cmake { arguments += listOf("-DMW2_TITLE=sp", "-DMW2_ONLINE=none") } }
        }
        create("multiplayer") {
            dimension = "title"
            applicationIdSuffix = ".mp"
            versionNameSuffix = "-multiplayer"
            resValue("string", "app_name", "CoD: MW2 Multiplayer")
            buildConfigField("String", "UPDATE_REPO", "\"flavinxs0/mw2-android\"")
            buildConfigField("String", "LIBRARY_SP", "\"\"")
            buildConfigField("String", "LIBRARY_MP", "\"mw2\"")
            // LAN backend: plain UDP system-link between phones/PCs. Steam
            // makes no sense on Android (no Steam client), and "none" would
            // leave MP with system-link on one device only.
            externalNativeBuild { cmake { arguments += listOf("-DMW2_TITLE=mp", "-DMW2_ONLINE=lan") } }
        }
        // Modern Warfare 3, the whole game in one app: the disc's two
        // executables, the campaign's (default.xex, with Special Ops;
        // ppc_mw3/) and the multiplayer's (default_mp.xex; ppc_mw3_mp/,
        // docs/MW3-MP-BRINGUP.md), as two libraries. The launcher offers
        // both, the run's process loads the one chosen (Title.kt), and both
        // read the one folder of game files. -DMW2_BOTH_TITLES=ON is what
        // makes the second library (cmake/android.cmake). The disc's version
        // only (mw3Tu0*; the tu6 variants are switched off below). The two
        // libraries are the runtime alone: the recompiled code is in the
        // player's own game package (gameSplit above). The
        // multiplayer's network is what the player chooses in Settings,
        // NETWORK (GameActivity sets MW2_NETWORK): a relay, the local
        // network, or none.
        create("mw3") {
            dimension = "title"
            applicationId = mw3ApplicationId
            resValue("string", "app_name", "CoD: Modern Warfare 3" + if (appSuffix.isEmpty()) "" else " ($appSuffix)")
            // The launcher emblem's ring: amber for the game.
            resValue("color", "icon_accent", "#E8A33D")
            // The public repository's releases carry this app, without the
            // recompiled code (.github/workflows/android-release.yml).
            buildConfigField("String", "UPDATE_REPO", "\"yassinsolim/mw3-recomp\"")
            if (gameSplit) buildConfigField("String", "GAME_PACKAGE", "\"$mw3ApplicationId.game\"")
            buildConfigField("String", "LIBRARY_SP", "\"mw3\"")
            buildConfigField("String", "LIBRARY_MP", "\"mw3mp\"")
            externalNativeBuild {
                cmake {
                    arguments += listOf(
                        "-DMW2_TITLE=mw3", "-DMW3_RECOMP_ONLY=ON", "-DMW3_LINK_PROBE=ON",
                        "-DMW2_ONLINE=none", "-DMW2_BOTH_TITLES=ON",
                        "-DMW2_MP_LIBRARY_DIRECTORY=${File(mw3mpLibraries, "arm64-v8a").invariantSeparatorsPath}",
                    )
                    setting("MW3_XEX_SHA256")?.let { arguments += listOf("-DMW3_XEX_SHA256_SP=$it") }
                    setting("MW3_XEX_SHA256_MP")?.let { arguments += listOf("-DMW3_XEX_SHA256_MP=$it") }
                    if (gameSplit) arguments += listOf("-DMW2_SPLIT=runtime")
                    setting("MW2_LTO")?.let { arguments += listOf("-DMW2_LTO=$it") }
                }
            }
        }
        // Modern Warfare 2, the whole game in one app, as Modern Warfare 3's
        // is: the campaign's library (libmw2.so, with Special Ops) and the
        // multiplayer's (libmw2mp.so), without the recompiled code, which is
        // in the player's own game package (com.mw2.recomp.game). The disc's
        // version only (mw2Tu0*): title update 6 is the campaign and
        // multiplayer flavours'.
        create("mw2") {
            dimension = "title"
            applicationId = mw2ApplicationId
            resValue("string", "app_name", "CoD: Modern Warfare 2" + if (appSuffix.isEmpty()) "" else " ($appSuffix)")
            // Blue, and two bars instead of three (src/mw2/res/drawable), so
            // it and Modern Warfare 3 tell apart on one home screen.
            resValue("color", "icon_accent", "#4FA3E0")
            // The public repository's releases carry this app, without the
            // recompiled code (.github/workflows/android-release-mw2.yml).
            buildConfigField("String", "UPDATE_REPO", "\"yassinsolim/mw2-recomp\"")
            if (gameSplit) buildConfigField("String", "GAME_PACKAGE", "\"$mw2ApplicationId.game\"")
            manifestPlaceholders["gamePackage"] = "$mw2ApplicationId.game"
            buildConfigField("String", "LIBRARY_SP", "\"mw2\"")
            buildConfigField("String", "LIBRARY_MP", "\"mw2mp\"")
            externalNativeBuild {
                cmake {
                    arguments += listOf(
                        "-DMW2_TITLE=sp", "-DMW2_BOTH_TITLES=ON",
                        "-DMW2_MP_LIBRARY_DIRECTORY=${File(mw2mpLibraries, "arm64-v8a").invariantSeparatorsPath}",
                    )
                    if (gameSplit) arguments += listOf("-DMW2_SPLIT=runtime")
                    setting("MW2_LTO")?.let { arguments += listOf("-DMW2_LTO=$it") }
                }
            }
        }
        // Call of Duty 4: Modern Warfare, the whole game in one app as the
        // two above: the campaign's library (libcod4.so) and the
        // multiplayer's (libcod4mp.so), without the recompiled code, which is
        // in the player's own game package (com.cod4.recomp.game). The disc's
        // version only (docs/COD4-BRINGUP.md).
        create("cod4") {
            dimension = "title"
            applicationId = cod4ApplicationId
            resValue("string", "app_name", "CoD 4: Modern Warfare" + if (appSuffix.isEmpty()) "" else " ($appSuffix)")
            // Teal, and one bar (src/cod4/res/drawable), so the three games
            // tell apart on one home screen.
            resValue("color", "icon_accent", "#35C7C0")
            // The public repository's releases carry this app, without the
            // recompiled code (.github/workflows/android-release-cod4.yml).
            buildConfigField("String", "UPDATE_REPO", "\"yassinsolim/cod4-recomp\"")
            if (gameSplit) buildConfigField("String", "GAME_PACKAGE", "\"$cod4ApplicationId.game\"")
            manifestPlaceholders["gamePackage"] = "$cod4ApplicationId.game"
            buildConfigField("String", "LIBRARY_SP", "\"cod4\"")
            buildConfigField("String", "LIBRARY_MP", "\"cod4mp\"")
            externalNativeBuild {
                cmake {
                    arguments += listOf(
                        "-DMW2_TITLE=cod4", "-DMW3_RECOMP_ONLY=ON", "-DMW3_LINK_PROBE=ON",
                        "-DMW2_ONLINE=none", "-DMW2_BOTH_TITLES=ON",
                        "-DMW2_MP_LIBRARY_DIRECTORY=${File(cod4mpLibraries, "arm64-v8a").invariantSeparatorsPath}",
                    )
                    if (gameSplit) arguments += listOf("-DMW2_SPLIT=runtime")
                    setting("MW2_LTO")?.let { arguments += listOf("-DMW2_LTO=$it") }
                }
            }
        }
        // Call of Duty: Advanced Warfare, the same shape: the campaign's
        // library (libaw.so) and the multiplayer's (libawmp.so), without the
        // recompiled code, which is in the player's own game package
        // (com.aw.recomp.game). The game disc's version only, with the
        // install disc's two packages beside the game folder
        // (docs/AW-BRINGUP.md).
        create("aw") {
            dimension = "title"
            applicationId = awApplicationId
            resValue("string", "app_name", "CoD: Advanced Warfare" + if (appSuffix.isEmpty()) "" else " ($appSuffix)")
            // Violet, and a chevron in place of the bars (src/aw/res/drawable),
            // so the four games tell apart on one home screen.
            resValue("color", "icon_accent", "#A98BFF")
            // The public repository's releases carry this app, without the
            // recompiled code (.github/workflows/android-release-aw.yml).
            buildConfigField("String", "UPDATE_REPO", "\"yassinsolim/aw-recomp\"")
            if (gameSplit) buildConfigField("String", "GAME_PACKAGE", "\"$awApplicationId.game\"")
            manifestPlaceholders["gamePackage"] = "$awApplicationId.game"
            buildConfigField("String", "LIBRARY_SP", "\"aw\"")
            buildConfigField("String", "LIBRARY_MP", "\"awmp\"")
            // The multiplayer as title update 17 makes it, which a player's
            // own map packs are for: a third library (cmake/android.cmake),
            // started in place of the disc's while the update's package is
            // beside the packs (Title.kt, docs/dlc.md).
            buildConfigField("String", "LIBRARY_MP_UPDATE", "\"awmp17\"")
            externalNativeBuild {
                cmake {
                    arguments += listOf(
                        "-DMW2_TITLE=aw", "-DMW3_RECOMP_ONLY=ON", "-DMW3_LINK_PROBE=ON",
                        "-DMW2_ONLINE=none", "-DMW2_BOTH_TITLES=ON",
                        "-DMW2_MP_LIBRARY_DIRECTORY=${File(awmpLibraries, "arm64-v8a").invariantSeparatorsPath}",
                    )
                    if (gameSplit) arguments += listOf("-DMW2_SPLIT=runtime")
                    setting("MW2_LTO")?.let { arguments += listOf("-DMW2_LTO=$it") }
                }
            }
        }
        // Call of Duty: Black Ops III, the same shape with other halves: on
        // this console it has no campaign, so the first library is the
        // multiplayer's (libbo3.so, default.xex) and the second the zombies'
        // (libbo3zm.so, default_zm.xex). Both are the executables title
        // update 5 makes of the disc's, which the runtime makes at every
        // start from the player's disc and update (docs/BO3-BRINGUP.md). Not
        // published anywhere: no update repository.
        create("bo3") {
            dimension = "title"
            applicationId = bo3ApplicationId
            resValue("string", "app_name", "CoD: Black Ops III" + if (appSuffix.isEmpty()) "" else " ($appSuffix)")
            // Orange, and a diamond in place of the bars and the chevron
            // (src/bo3/res/drawable), so the five games tell apart on one
            // home screen.
            resValue("color", "icon_accent", "#FF8A3D")
            buildConfigField("String", "UPDATE_REPO", "\"\"")
            if (gameSplit) buildConfigField("String", "GAME_PACKAGE", "\"$bo3ApplicationId.game\"")
            manifestPlaceholders["gamePackage"] = "$bo3ApplicationId.game"
            buildConfigField("String", "LIBRARY_SP", "\"bo3\"")
            buildConfigField("String", "LIBRARY_MP", "\"bo3zm\"")
            externalNativeBuild {
                cmake {
                    arguments += listOf(
                        "-DMW2_TITLE=bo3", "-DMW3_RECOMP_ONLY=ON", "-DMW3_LINK_PROBE=ON",
                        "-DMW2_ONLINE=none", "-DMW2_BOTH_TITLES=ON",
                        "-DMW2_MP_LIBRARY_DIRECTORY=${File(bo3zmLibraries, "arm64-v8a").invariantSeparatorsPath}",
                    )
                    if (gameSplit) arguments += listOf("-DMW2_SPLIT=runtime")
                    setting("MW2_LTO")?.let { arguments += listOf("-DMW2_LTO=$it") }
                }
            }
        }
        // The multiplayer alone, as an app beside the game: the test harness
        // for two players on one device (-PmpInstance above), not something
        // a player installs.
        create("mw3mp") {
            dimension = "title"
            applicationId = "com.mw3.recomp.mp" + if (mpInstance > 1) "$mpInstance" else ""
            versionNameSuffix = "-multiplayer"
            resValue("string", "app_name", "CoD: MW3 Multiplayer" + if (mpInstance > 1) " ($mpInstance)" else "")
            // Green, so it and the game tell apart at a glance.
            resValue("color", "icon_accent", "#3FCF8E")
            buildConfigField("String", "UPDATE_REPO", "\"\"")
            buildConfigField("int", "MP_INSTANCE", "$mpInstance")
            buildConfigField("String", "LIBRARY_SP", "\"\"")
            buildConfigField("String", "LIBRARY_MP", "\"mw3mp\"")
            externalNativeBuild {
                cmake {
                    arguments += listOf(
                        "-DMW2_TITLE=mw3mp", "-DMW3_RECOMP_ONLY=ON", "-DMW3_LINK_PROBE=ON",
                        "-DMW2_ONLINE=none",
                    )
                    setting("MW3_XEX_SHA256_MP")?.let { arguments += listOf("-DMW3_XEX_SHA256_MP=$it") }
                    setting("MW2_LTO")?.let { arguments += listOf("-DMW2_LTO=$it") }
                }
            }
        }
        create("tu0") {
            dimension = "tu"
            buildConfigField("String", "MW2_TU", "\"tu0\"")
            buildConfigField("String", "MW2_TU6_URL", "\"\"")
        }
        create("tu6") {
            dimension = "tu"
            applicationIdSuffix = ".tu6"
            versionNameSuffix = "-tu6"
            buildConfigField("String", "MW2_TU", "\"tu6\"")
            // Title update 6, downloaded at install time like the PC launcher
            // does (launcher/setup.cpp kUpdateUrl): a public archive of title
            // updates, pinned to a commit so the file cannot change. Game
            // files never ride in the APK itself.
            buildConfigField(
                "String", "MW2_TU6_URL",
                "\"https://raw.githubusercontent.com/codxenon/xbox360-title-updates/25de2389ce214dae6159fcd45a9581cb3b640bd9/games/Call%20of%20Duty%20-%20Modern%20Warfare%202%20%28USA%2C%20Europe%29/TU6/TU_10LC20N_0000018000000.00000000001GA\"",
            )
            externalNativeBuild { cmake { arguments += listOf("-DMW2_VERSION=tu6") } }
        }
    }

    androidComponents {
        beforeVariants { variant ->
            val flavors = variant.productFlavors.toMap()
            if (flavors["title"] in setOf("mw3", "mw3mp", "mw2", "cod4", "aw", "bo3") && flavors["tu"] == "tu6") variant.enable = false
            // A second copy is a test harness, never a release.
            if (mpInstance > 1 && flavors["title"] == "mw3mp" && variant.buildType != "debug") variant.enable = false
        }
    }

    // The MW3 wording and the bundled driver (src/mw3) are for both MW3
    // flavours. The second library of an app with both titles, the
    // multiplayer's, comes out of the native build as a file rather than as
    // one of its targets, so it is packaged from the folder that build leaves
    // it in.
    sourceSets {
        getByName("mw3mp") {
            res.srcDir("src/mw3/res")
            assets.srcDir("src/mw3/assets")
        }
        getByName("mw3") {
            jniLibs.srcDir(mw3mpLibraries)
        }
        getByName("mw2") {
            jniLibs.srcDir(mw2mpLibraries)
        }
        getByName("cod4") {
            jniLibs.srcDir(cod4mpLibraries)
        }
        getByName("aw") {
            jniLibs.srcDir(awmpLibraries)
        }
        getByName("bo3") {
            jniLibs.srcDir(bo3zmLibraries)
        }
        // Modern Warfare 2's, Call of Duty 4's, Advanced Warfare's and Black
        // Ops III's apps carry the same Turnip driver and nothing else of
        // src/mw3/assets (the camouflage packs are MW3's): a copy of the
        // drivers folder alone, made by bundledDriverAssets below.
        listOf("campaign", "multiplayer", "mw2", "cod4", "aw", "bo3").forEach { getByName(it) { assets.srcDir(mw2DriverAssets) } }
    }

    externalNativeBuild {
        cmake {
            path = File(repositoryRoot, "CMakeLists.txt")
            version = "3.22.1+"
        }
    }

    // A release apk with no key is not installable, and an app nobody can
    // install is not a build. A key given in the environment is used; without
    // one the debug key signs it, which Android accepts and which makes it
    // plain that this is not a store build.
    signingConfigs {
        val keystorePath = setting("MW2_KEYSTORE")
        if (keystorePath != null && file(keystorePath).exists()) {
            create("supplied") {
                storeFile = file(keystorePath)
                storePassword = setting("MW2_KEYSTORE_PASSWORD")
                keyAlias = setting("MW2_KEY_ALIAS")
                keyPassword = setting("MW2_KEY_PASSWORD") ?: setting("MW2_KEYSTORE_PASSWORD")
            }
        }
    }

    buildTypes {
        release {
            signingConfig = signingConfigs.findByName("supplied")
                ?: signingConfigs.getByName("debug")
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
            // The library is enormous and the debug info larger still; it is
            // kept beside the build for reading crash reports against.
            ndk { debugSymbolLevel = "SYMBOL_TABLE" }
        }
        debug {
            isMinifyEnabled = false
            isJniDebuggable = true
        }
    }

    packaging {
        jniLibs {
            // The apk is installed extracted: the runtime dlopens the
            // adrenotools hook libraries by path, and a driver loaded from a
            // zip needs real files on disk beside them.
            useLegacyPackaging = true
        }
    }

    buildFeatures {
        buildConfig = true
        viewBinding = false
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
}

// The multiplayer's library is in its folder only once the native build has
// run, and Gradle does not know the folder is that build's output: say so, or
// the packaging could read it first and leave the multiplayer out.
tasks.configureEach {
    Regex("merge((?:Mw[23]|Cod4|Aw|Bo3)Tu0\\w+)JniLibFolders").matchEntire(name)?.let {
        dependsOn("externalNativeBuild${it.groupValues[1]}")
    }
}

// The bundled Turnip package (android/fetch_deps.sh puts it under src/mw3),
// for the MW2 flavours' assets; empty when the download was skipped, and the
// app then has no bundled driver (DriverStore.installBundledDefault).
val bundledDriverAssets = tasks.register<Sync>("bundledDriverAssets") {
    from("src/mw3/assets") { include("drivers/**") }
    into(mw2DriverAssets)
}
tasks.configureEach {
    // Every task that reads the MW2 flavours' assets: merging them, and in a
    // release build lint's model and analysis, which read the source folders
    // (the v0.1.0 release jobs of the MW2 repo failed on each undeclared one).
    val forMw2 = listOf("Campaign", "Multiplayer", "Mw2", "Cod4", "Aw", "Bo3").any { name.contains(it) }
    if (forMw2 && (name.contains("Assets") || name.contains("lint", ignoreCase = true))) dependsOn(bundledDriverAssets)
}

// ---- the build kit (docs/ondevice-build.md) ----------------------------------
// What the app needs to build the game code on the device itself
// (GameBuild.kt), as assets under buildkit/: everything that decides what the
// app's own runtime will accept, so it travels with the app and not with the
// toolchain package, which only brings the compiler and the recompiler.
//
//   config/<title>.toml       the recompiler's configuration, as it is here
//   runtime/mmio_hook.h       force-included into every recompiled file
//   cmake/game_library.list   the symbols left to the dynamic linker
//   shared_header_prefix.h    what tools/genshared.py writes before the
//                             declarations, taken out of that script
//   kit.json                  the titles (executable, the SHA-256 of the
//                             supported builds, the folders the configuration
//                             names), what setup\setup.ps1 does for the game
//                             besides, the compiler's and linker's options as
//                             CMakeLists.txt and cmake/game_library.cmake give
//                             them, and for each title the statement the
//                             runtime expects and the earlier ones it takes
//                             as well (cmake/game_abi.cmake's, made here the
//                             same way from config/abi/)
class KitTitle(
    val library: String, val executable: String, val sha256: List<String>, val toml: String,
    val tables: String, val tree: String, val defines: List<String>,
    /**
     * For a title a player's own title update makes of the one before it
     * (Advanced Warfare's multiplayer for the map packs, docs/dlc.md): the
     * folder its executable is made in on the device, from that title's
     * executable and the update's patch. It has no SHA-256 here: the update's
     * header differs with the disc's pressing, and the runtime checks the
     * image (runtime/install/install.cpp).
     */
    val updateFolder: String? = null,
    /**
     * For such a title in a kit that has no title of the disc's own (Black
     * Ops III, whose disc's executables are run nowhere): the SHA-256 of the
     * disc's executable it is made from, which the build takes from the
     * player's disc.
     */
    val discSha256: List<String> = emptyList(),
)
class KitGame(
    val folder: String, val titles: List<KitTitle>, val extraSources: List<String> = emptyList(),
    /** The jump tables the analyser does not find are added (tools/intext_switch_tables.py, InTextTables.kt). */
    val inTextTables: Boolean = false,
)
// The executables' hashes are setup/setup.ps1's (Modern Warfare 2),
// tools/check_mw3_xex.py's, tools/check_cod4_xex.py's and
// tools/check_aw_xex.py's.
val kitGames = mapOf(
    "mw2" to KitGame(
        "mw2/tu0",
        listOf(
            KitTitle("mw2", "default.xex", listOf("990e640d3a92544f9d585e71faf1ab65082d37ec6f5865b2599335fa123bf6ff"),
                "MW2.tu0.toml", "mw2_switch_tables.tu0.toml", "ppc_tu0", listOf("MW2_VERSION_TU0=1")),
            KitTitle("mw2mp", "default_mp.xex", listOf("aacc47aff3221f3bd9129e80827fd3bcd74aa43bf74bb5c5ddbd0597c0d5fcde"),
                "MW2MP.tu0.toml", "mw2mp_switch_tables.tu0.toml", "ppc_mp_tu0", listOf("MW2_TITLE_MP=1", "MW2_VERSION_TU0=1")),
        ),
    ),
    "mw3" to KitGame(
        "mw3/tu0",
        listOf(
            KitTitle("mw3", "default.xex",
                listOf("d90bc8d93b7b8f6001ead8330271196a6ef7a5c2e4a9e9b63118f47d58a05912",
                    "e94de720373d668a2bd592dfc74339bfbbc98cca993176fcd055b1449633bcd1"),
                "MW3.toml", "mw3_switch_tables.toml", "ppc_mw3", listOf("MW3_TITLE=1")),
            KitTitle("mw3mp", "default_mp.xex", listOf("0d7669844122b8f94e29ec87baf6bf0a7b84ab4abad8a82b43aa444682f90f7c"),
                "MW3MP.toml", "mw3mp_switch_tables.toml", "ppc_mw3_mp", listOf("MW3_TITLE=1", "MW3_TITLE_MP=1")),
        ),
    ),
    "cod4" to KitGame(
        "cod4/tu0",
        listOf(
            KitTitle("cod4", "default.xex", listOf("f6b848ac4ecf2b12881223e4c3cd63c49ae36e1fadc6f2179428033274602739"),
                "COD4.toml", "cod4_switch_tables.toml", "ppc_cod4", listOf("COD4_TITLE=1", "MW3_TITLE=1")),
            KitTitle("cod4mp", "default_mp.xex", listOf("9aae8db5b38f0b394f9052d1ee9c8b1f18038ff86fc5f4110f173f66043a0cc6"),
                "COD4MP.toml", "cod4mp_switch_tables.toml", "ppc_cod4_mp", listOf("COD4_TITLE=1", "COD4_TITLE_MP=1", "MW3_TITLE=1")),
        ),
        // The two instructions the configuration replaces by calls (CMakeLists.txt).
        listOf("runtime/recomp/ppc_extra_instructions.cpp"),
        inTextTables = true,
    ),
    "aw" to KitGame(
        "aw/tu0",
        listOf(
            KitTitle("aw", "default.xex", listOf("a8b1e99c27d6d03a0b0700e6eada1e7d24ee4b194e3c9fa844f16c25f8376a73"),
                "AW.toml", "aw_switch_tables.toml", "ppc_aw", listOf("AW_TITLE=1", "MW3_TITLE=1")),
            KitTitle("awmp", "default_mp.xex", listOf("3ef96359f96ee7994e43de70f56a9c2a019d5b841f11318ca2dffdb717b265f8"),
                "AWMP.toml", "awmp_switch_tables.toml", "ppc_aw_mp", listOf("AW_TITLE=1", "AW_TITLE_MP=1", "MW3_TITLE=1")),
            KitTitle("awmp17", "default_mp.xex", emptyList(),
                "AWMP.tu17.toml", "awmp_switch_tables.tu17.toml", "ppc_aw_mp_tu17",
                listOf("AW_TITLE=1", "AW_TITLE_MP=1", "AW_TITLE_TU17=1", "MW3_TITLE=1"), updateFolder = "aw/tu17"),
        ),
        // The instructions the configuration replaces by calls (CMakeLists.txt).
        listOf("runtime/recomp/ppc_extra_instructions.cpp"),
        inTextTables = true,
    ),
    // Both titles are what title update 5 makes of the disc's executables
    // (docs/BO3-BRINGUP.md): the build takes those from the disc and the
    // update from the dlc folder. The hashes are the disc's, as
    // CMakeLists.txt has them.
    "bo3" to KitGame(
        "bo3/tu0",
        listOf(
            KitTitle("bo3", "default.xex", emptyList(),
                "BO3.toml", "bo3_switch_tables.toml", "ppc_bo3", listOf("BO3_TITLE=1", "MW3_TITLE=1"),
                updateFolder = "bo3/tu5",
                discSha256 = listOf("e64ed5fa25ca8c97d3970efc9b0d17bc32253cc43dbf9f5f96637aada064d8bb")),
            KitTitle("bo3zm", "default_zm.xex", emptyList(),
                "BO3ZM.toml", "bo3zm_switch_tables.toml", "ppc_bo3_zm", listOf("BO3_TITLE=1", "BO3_TITLE_ZM=1", "MW3_TITLE=1"),
                updateFolder = "bo3/tu5",
                discSha256 = listOf("d0eb63c97f69cf14ecaf5171429fa4afcce6f0fd5ca3d774c7a0af343f33fe4e")),
        ),
        // The instructions the configuration replaces by calls (CMakeLists.txt).
        listOf("runtime/recomp/ppc_extra_instructions.cpp"),
        inTextTables = true,
    ),
)
// A file's text and its SHA-256 as cmake/game_abi.cmake take them: without
// the carriage returns a Windows checkout has.
fun kitText(path: String): String = File(repositoryRoot, path).readBytes().toString(Charsets.ISO_8859_1).replace("\r", "")
fun kitHash(text: String): String =
    MessageDigest.getInstance("SHA-256").digest(text.toByteArray(Charsets.ISO_8859_1)).joinToString("") { "%02x".format(it) }
fun kitJson(value: Any?): String = when (value) {
    is Map<*, *> -> value.entries.joinToString(",", "{", "}") { "${kitJson(it.key.toString())}:${kitJson(it.value)}" }
    is List<*> -> value.joinToString(",", "[", "]") { kitJson(it) }
    is String -> "\"" + value.replace("\\", "\\\\").replace("\"", "\\\"").replace("\n", "\\n") + "\""
    else -> value.toString()
}
kitGames.forEach { (game, kit) ->
    val into = layout.buildDirectory.dir("build-kit/$game").get().asFile
    val copied = kit.titles.map { "config/${it.toml}" } + kit.extraSources + listOf("runtime/mmio_hook.h", "cmake/game_library.list")
    val hashed = copied + kit.titles.flatMap { title ->
        listOf("kernel_imports.list", "ppc_config.h", "generated_code.sha256", "earlier_statements.list")
            .map { "config/abi/${title.library}/$it" }
    }.filter { File(repositoryRoot, it).isFile } + listOf(
        "setup/setup.ps1", "setup/toolchain.json", "patches/xenonrecomp-mw2.patch", "tools/genshared.py",
        "XenonRecomp/XenonUtils/ppc_context.h",
    )
    val task = tasks.register("gameBuildKit${game.replaceFirstChar { it.uppercase() }}") {
        inputs.files(hashed.map { File(repositoryRoot, it) })
        inputs.file("build.gradle.kts")
        outputs.dir(into)
        doLast {
            into.deleteRecursively()
            val root = File(into, "buildkit")
            copied.forEach { File(root, it).apply { parentFile.mkdirs() }.writeText(kitText(it), Charsets.ISO_8859_1) }
            val commit = Regex("XenonCommit[ ]*=[ ]*'([0-9a-f]+)'").find(kitText("setup/setup.ps1"))?.groupValues?.get(1)
                ?: throw GradleException("setup/setup.ps1 no longer names the XenonRecomp commit (\$XenonCommit)")
            val patch = kitHash(kitText("patches/xenonrecomp-mw2.patch"))
            val shared = kitText("tools/genshared.py")
            val prefix = Regex("HEADER = \"\"\"(.*?)\"\"\"", RegexOption.DOT_MATCHES_ALL).find(shared)?.groupValues?.get(1)
                ?: throw GradleException("tools/genshared.py no longer has its HEADER text")
            // The script's string has each backslash written twice.
            File(root, "shared_header_prefix.h").writeText(prefix.replace("\\\\", "\\"), Charsets.ISO_8859_1)
            val memoryHook = kitHash(kitText("runtime/mmio_hook.h"))
            // The recompiler writes its context header into the tree behind these lines (cmake/game_abi.cmake).
            val context = kitHash("#pragma once\n#include \"ppc_config.h\"\n\n" + kitText("XenonRecomp/XenonUtils/ppc_context.h"))
            val titles = kit.titles.map { title ->
                val recorded = "config/abi/${title.library}"
                val code = kitText("$recorded/generated_code.sha256").trim()
                val shares = "code-extent ${kitHash(kitText("$recorded/ppc_config.h"))}\n" +
                    "register-context $context\n" +
                    "system-calls ${kitHash(kitText("$recorded/kernel_imports.list"))}\n" +
                    "memory-hook $memoryHook\n"
                // The statements of libraries built before the statement named
                // the code, where they were made from this code and share what
                // this app shares (cmake/game_abi.cmake).
                val earlier = File(repositoryRoot, "$recorded/earlier_statements.list").takeIf { it.isFile }
                    ?.let { kitText("$recorded/earlier_statements.list") }.orEmpty()
                    .lines().filterNot { it.startsWith("#") }.joinToString("\n").split(Regex("\n\n+"))
                    .map { it.trim() + "\n" }
                    .filter { it.startsWith("generated-code $code\ntitle ${title.library}\n") && it.endsWith("\n" + shares) }
                    .map { it.substringAfter('\n') }
                mapOf(
                    "library" to title.library, "executable" to title.executable, "sha256" to title.sha256,
                    "folder" to (title.updateFolder ?: kit.folder), "fromUpdate" to (title.updateFolder != null),
                    "discSha256" to title.discSha256,
                    "toml" to title.toml, "tables" to title.tables, "tree" to title.tree,
                    "defines" to listOf("MW2_TRACE_INDIRECT=1") + title.defines,
                    "expectedAbi" to "title ${title.library}\ngenerated-code $code\n" + shares,
                    "earlierAbis" to earlier,
                )
            }
            // The release the toolchain package is published in and the file
            // it must be (setup/toolchain.json, docs/android-releases.md): the
            // app downloads only that, and takes a file with no other hash.
            val toolchain = groovy.json.JsonSlurper().parseText(kitText("setup/toolchain.json")) as Map<*, *>
            val toolchainRelease = toolchain["release"] as Map<*, *>
            val toolchainHash = toolchain["sha256"] as String
            if (!Regex("[0-9a-f]{64}").matches(toolchainHash)) {
                throw GradleException("setup/toolchain.json has no SHA-256 for ${toolchain["asset"]}: draft the toolchain release first")
            }
            File(root, "kit.json").writeText(
                kitJson(
                    mapOf(
                        "game" to game, "folder" to kit.folder,
                        "recompiler" to mapOf("commit" to commit, "patchSha256" to patch),
                        "memoryHookSha256" to memoryHook,
                        "toolchain" to mapOf(
                            "repo" to toolchainRelease["repo"], "tag" to toolchainRelease["tag"],
                            "asset" to toolchain["asset"], "version" to toolchain["version"], "sha256" to toolchainHash,
                        ),
                        "extraSources" to kit.extraSources,
                        "inTextTables" to kit.inTextTables,
                        // CMakeLists.txt (ppc_recomp) and the NDK's own defaults, as a
                        // setup\setup.ps1 build has them, less -g: nothing on the device
                        // strips the library, and the compiler needs less memory without.
                        "compile" to listOf(
                            "-DANDROID", "-fdata-sections", "-ffunction-sections", "-funwind-tables", "-fstack-protector-strong",
                            "-no-canonical-prefixes", "-D_FORTIFY_SOURCE=2", "-O3", "-DNDEBUG", "-fPIC", "-w", "-fwrapv",
                            "-fno-omit-frame-pointer", "-std=gnu++20",
                        ),
                        // cmake/game_library.cmake, as the linker itself takes them.
                        "link" to listOf(
                            "--build-id=sha1", "--gc-sections", "-z", "max-page-size=16384", "-z", "common-page-size=16384",
                            "--unresolved-symbols=ignore-all",
                        ),
                        "titles" to titles,
                    ),
                ) + "\n",
            )
        }
    }
    android.sourceSets.getByName(game) { assets.srcDir(into) }
    val flavour = game.replaceFirstChar { it.uppercase() } + "Tu0"
    tasks.configureEach {
        // As bundledDriverAssets: every task that reads the flavour's assets.
        if (name.contains(flavour) && (name.contains("Assets") || name.contains("lint", ignoreCase = true))) dependsOn(task)
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.15.0")
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("androidx.activity:activity-ktx:1.9.3")
    implementation("androidx.documentfile:documentfile:1.0.1")
    // Tombstone.kt's reading of the system's crash record, on the build
    // machine: `gradlew :app:testMw2Tu0DebugUnitTest`.
    testImplementation("junit:junit:4.13.2")
}

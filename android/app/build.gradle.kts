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

// -PmpInstance=2: a second copy of the MW3 multiplayer app beside the first
// (com.mw3.recomp.mp2, "CoD: MW3 Multiplayer (2)"), so that two can play each
// other on one device (docs/MW3-MP-BRINGUP.md). Debug builds only. The native
// library is the same; the copy has its own settings, game folder and saves.
val mpInstance = (findProperty("mpInstance") as String?)?.toIntOrNull()?.coerceIn(1, 9) ?: 1

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
        // whose versionCode is at or below the installed one. CI may set
        // MW2_VERSION_CODE instead of editing this file.
        // 16: v0.0.2-alpha, over 15 (v0.0.2).
        versionCode = setting("MW2_VERSION_CODE")?.toIntOrNull() ?: 16
        // Which build this is, carried inside the apk. "The wrong apk" has
        // cost more than one evening, and every time the only way to settle
        // it was to guess. The launcher shows this and Android's app info
        // shows it, so the file on the phone can be matched against the run
        // that made it without downloading anything twice.
        // The release the build is from, which is what a report has to say to
        // be acted on. It was "1.0" -- a number no release ever had.
        versionName = "${setting("MW2_VERSION_NAME") ?: "0.0.2-alpha"} " +
            "(${setting("MW2_BUILD_SHA") ?: "local"})"
        buildConfigField("String", "BUILD_SHA", "\"${setting("MW2_BUILD_SHA") ?: "local"}\"")
        buildConfigField("String", "BUILD_STAMP", "\"${setting("MW2_BUILD_STAMP") ?: "unstamped"}\"")
        buildConfigField("int", "MP_INSTANCE", "1")

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
    }

    // The disc carries two executables and each is recompiled into a tree of
    // its own (build.sh TITLE=sp|mp). They are separate apps: each is a
    // hundred-odd megabytes of recompiled code, and nobody wants both in one
    // download.
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
            externalNativeBuild { cmake { arguments += listOf("-DMW2_TITLE=sp", "-DMW2_ONLINE=none") } }
        }
        create("multiplayer") {
            dimension = "title"
            applicationIdSuffix = ".mp"
            versionNameSuffix = "-multiplayer"
            resValue("string", "app_name", "CoD: MW2 Multiplayer")
            buildConfigField("String", "UPDATE_REPO", "\"flavinxs0/mw2-android\"")
            // LAN backend: plain UDP system-link between phones/PCs. Steam
            // makes no sense on Android (no Steam client), and "none" would
            // leave MP with system-link on one device only.
            externalNativeBuild { cmake { arguments += listOf("-DMW2_TITLE=mp", "-DMW2_ONLINE=lan") } }
        }
        // Modern Warfare 3: the disc's single-player executable (campaign and
        // Special Ops), recompiled into ppc_mw3/ by tools/recomp_mw3.sh. An
        // app of its own, with the disc's version only (mw3Tu0*; the tu6
        // variants are switched off below). No published builds, so nothing
        // to check for updates against.
        create("mw3") {
            dimension = "title"
            applicationId = "com.mw3.recomp"
            resValue("string", "app_name", "CoD: Modern Warfare 3")
            buildConfigField("String", "UPDATE_REPO", "\"\"")
            externalNativeBuild {
                cmake {
                    arguments += listOf(
                        "-DMW2_TITLE=mw3", "-DMW3_RECOMP_ONLY=ON", "-DMW3_LINK_PROBE=ON",
                        "-DMW2_ONLINE=none",
                    )
                    setting("MW3_XEX_SHA256")?.let { arguments += listOf("-DMW3_XEX_SHA256=$it") }
                    setting("MW2_LTO")?.let { arguments += listOf("-DMW2_LTO=$it") }
                }
            }
        }
        // Its multiplayer: the disc's default_mp.xex, recompiled into
        // ppc_mw3_mp/ (docs/MW3-MP-BRINGUP.md). Beside the campaign app, as
        // MW2's two are. Its network is what the player chooses in Settings,
        // NETWORK (GameActivity sets MW2_NETWORK): a relay, the local network,
        // or none.
        create("mw3mp") {
            dimension = "title"
            applicationId = "com.mw3.recomp.mp" + if (mpInstance > 1) "$mpInstance" else ""
            versionNameSuffix = "-multiplayer"
            resValue("string", "app_name", "CoD: MW3 Multiplayer" + if (mpInstance > 1) " ($mpInstance)" else "")
            buildConfigField("String", "UPDATE_REPO", "\"\"")
            buildConfigField("int", "MP_INSTANCE", "$mpInstance")
            externalNativeBuild {
                cmake {
                    arguments += listOf(
                        "-DMW2_TITLE=mw3mp", "-DMW3_RECOMP_ONLY=ON", "-DMW3_LINK_PROBE=ON",
                        "-DMW2_ONLINE=none",
                    )
                    setting("MW3_XEX_SHA256_MP")?.let { arguments += listOf("-DMW3_XEX_SHA256=$it") }
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
            if (flavors["title"] in setOf("mw3", "mw3mp") && flavors["tu"] == "tu6") variant.enable = false
            // A second copy is a test harness, never a release.
            if (mpInstance > 1 && flavors["title"] == "mw3mp" && variant.buildType != "debug") variant.enable = false
        }
    }

    // The MW3 wording and the bundled driver (src/mw3) are for both MW3 apps.
    sourceSets {
        getByName("mw3mp") {
            res.srcDir("src/mw3/res")
            assets.srcDir("src/mw3/assets")
        }
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

dependencies {
    implementation("androidx.core:core-ktx:1.15.0")
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("androidx.activity:activity-ktx:1.9.3")
    implementation("androidx.documentfile:documentfile:1.0.1")
}

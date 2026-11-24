pluginManagement {
    repositories {
        google {
            content {
                includeGroupByRegex("com\\.android.*")
                includeGroupByRegex("com\\.google.*")
                includeGroupByRegex("androidx.*")
            }
        }
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "MW2"
include(":app")
// The recompiled code's package, built on a player's PC and never published
// (android/game/build.gradle.kts).
include(":game")
// The build tools for building the game code on the phone, published beside
// the apps, and the app that tries them on a device (docs/ondevice-build.md).
include(":toolchain")
include(":toolchain-probe")

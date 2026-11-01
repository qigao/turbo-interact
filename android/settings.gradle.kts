pluginManagement {
    repositories {
        google()
        gradlePluginPortal()
        mavenCentral()
    }
}

plugins {
    id("io.github.qigao.simpledsl.settings") version "0.3.0"
}

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "TurboInteractAndroid"

// SimpleDSL discovers the production :agent module by its build.gradle file.
// Do not duplicate module topology with a manual include or a consumer project.

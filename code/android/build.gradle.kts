// The Android app for the SWC adapter. See the spec's §4 for the wire protocol
// and docs/superpowers/plans/ for the task breakdown.
//
// AGP 9.1.0 pairs with Gradle 9.x; the wrapper is pinned to a Gradle 9.3.1 that
// is already in this machine's cache, so a build does not have to download a
// distribution first. `./gradlew` regenerates the wrapper on a fresh clone.
plugins {
    id("com.android.application") version "9.1.0" apply false
    id("org.jetbrains.kotlin.android") version "2.2.10" apply false
    id("org.jetbrains.kotlin.plugin.serialization") version "2.2.10" apply false
}

package com.oetsolutions.swc.update

import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL

/*
 * Where the release manifest comes from (spec §9.5).
 *
 * An INTERFACE with a real implementation and a test double, and the seam is the
 * whole point of the file: the check's logic (parse, compare, decide) is in
 * `ReleaseManifest` and is JVM-testable with no network, while the one thing a JVM
 * unit test must not do — make a real request — is isolated here. `AppViewModel`
 * takes a `ManifestFetcher`, so every test drives the decision path offline, and
 * only the real app ever constructs `HttpManifestFetcher`.
 *
 * A suspended function rather than a callback so a caller can `withContext` it off
 * the main thread without a thread hop of its own, and `Result`-shaped so a
 * transport failure is a value the view model turns into a message rather than an
 * exception that escapes the coroutine.
 */

/** The published manifest (spec §9.5), read as text. */
fun interface ManifestFetcher {
    /** Fetch the manifest body, or throw `IOException` on any transport failure. */
    suspend fun fetch(url: String): String
}

/**
 * Downloads the firmware image (spec §9.5 step 5: "download to the inactive slot
 * and confirm"), for the app to then push over USB.
 *
 * A separate seam from [ManifestFetcher] because it is a different kind of read — a
 * ~1.5 MB binary versus a few hundred bytes of JSON — and the app verifies its
 * `sha256` and `size_bytes` before any of it reaches the device. Suspended and
 * `Result`-shaped for the same reason the fetcher is: a transport failure is a
 * value the view model reports, not an exception that escapes its coroutine.
 */
fun interface ImageDownloader {
    /** Fetch the image bytes, or throw `IOException` on any transport failure. */
    suspend fun download(url: String): ByteArray
}

/**
 * The production fetcher: a plain HTTPS GET with a short timeout.
 *
 * **`HttpURLConnection`, deliberately, and not a third-party HTTP client.** The app
 * has no HTTP dependency and §9.5 does not need one: the manifest is a single
 * small static JSON document fetched once when the user taps "Check for updates".
 * Adding a networking library for that would be a dependency, a transitive surface
 * and a supply-chain edge for one GET.
 *
 * **TLS is the platform's, and that is the honest state of the trust anchor.** The
 * connection is `https://` and the URL's scheme is re-checked here (the manifest
 * parser checks the IMAGE url, this checks the MANIFEST url), so the fetch is
 * authenticated by the Android system trust store plus certificate pinning where
 * the platform applies it. Spec §9.5 asks for a pinned CA on the DEVICE side; on
 * the phone the system store is the platform model, and the firmware-side pinning
 * gap is recorded separately (open item N-62). The manifest's own `sha256` and the
 * device's `ImageVerify` gate are what protect the IMAGE regardless of the channel.
 */
class HttpManifestFetcher : ManifestFetcher {
    override suspend fun fetch(url: String): String {
        if (!url.startsWith("https://")) {
            // Refused before any request, for the same reason the manifest parser
            // refuses a plain-http image url: a downgrade is not a lesser preference.
            throw IOException("the manifest URL is not https")
        }
        val conn = (URL(url).openConnection() as HttpURLConnection).apply {
            requestMethod = "GET"
            connectTimeout = 10_000
            readTimeout = 10_000
            // A release host redirects to the object store; follow it, since the
            // redirect target is still the release host's choice over HTTPS.
            instanceFollowRedirects = true
            setRequestProperty("Accept", "application/json")
        }
        try {
            val code = conn.responseCode
            if (code != HttpURLConnection.HTTP_OK) {
                throw IOException("the release host answered HTTP $code")
            }
            // Bounded: a manifest is a few hundred bytes. A response stream that
            // never ends must not grow the string without limit, so read a byte at
            // a time and stop past a ceiling that is generous for a real manifest
            // and tiny for an attack.
            val body = conn.inputStream.bufferedReader().use { r ->
                val sb = StringBuilder()
                val buf = CharArray(4096)
                while (true) {
                    val n = r.read(buf)
                    if (n < 0) break
                    sb.append(buf, 0, n)
                    if (sb.length > kMaxManifestBytes) {
                        throw IOException("the manifest was larger than $kMaxManifestBytes bytes")
                    }
                }
                sb.toString()
            }
            return body
        } finally {
            conn.disconnect()
        }
    }

    private companion object {
        /** Generous for spec §9.5's manifest, tiny next to an image. */
        const val kMaxManifestBytes = 256 * 1024
    }
}

/**
 * The production image downloader, over the same `HttpURLConnection` policy as
 * [HttpManifestFetcher] and for the same reasons (no HTTP dependency for one GET,
 * HTTPS enforced before any request).
 *
 * **It does NOT verify the digest.** The `sha256`/`size_bytes` check is the view
 * model's job against the manifest it just parsed, and keeping it there means the
 * verification is unit-tested with no network — the downloader is only the bytes.
 * The device re-verifies the same digest through its own `ImageVerify` gate before
 * writing anything, so the image is checked twice by two independent
 * implementations.
 */
class HttpImageDownloader : ImageDownloader {
    override suspend fun download(url: String): ByteArray {
        if (!url.startsWith("https://")) {
            throw IOException("the image URL is not https")
        }
        val conn = (URL(url).openConnection() as HttpURLConnection).apply {
            requestMethod = "GET"
            connectTimeout = 15_000
            // Longer than the manifest's: this is a megabyte and a half.
            readTimeout = 60_000
            instanceFollowRedirects = true
        }
        try {
            val code = conn.responseCode
            if (code != HttpURLConnection.HTTP_OK) {
                throw IOException("the release host answered HTTP $code")
            }
            val out = java.io.ByteArrayOutputStream()
            val buf = ByteArray(64 * 1024)
            conn.inputStream.use { input ->
                while (true) {
                    val n = input.read(buf)
                    if (n < 0) break
                    out.write(buf, 0, n)
                    // Bounded by the device's own app slot, so a runaway response
                    // cannot exhaust the phone's memory. The manifest's size_bytes
                    // is re-checked by the caller; this is the belt to that braces.
                    if (out.size() > kMaxImageBytes) {
                        throw IOException("the image was larger than $kMaxImageBytes bytes")
                    }
                }
            }
            return out.toByteArray()
        } finally {
            conn.disconnect()
        }
    }

    private companion object {
        /**
         * The device's app slot, from the firmware's `kAppSlotBytes` (1,920 KiB,
         * `partitions.csv`; spec §9.2) — the same bound `SwcClient`'s
         * `kWireFirmwareMaxBytes` carries, and pinned to the firmware by
         * `check_app_limits.py`. A larger image than this is one the device would
         * refuse anyway, so refusing it here means a runaway response is stopped
         * before it fills the phone's memory rather than after.
         */
        const val kMaxImageBytes = 1_966_080
    }
}

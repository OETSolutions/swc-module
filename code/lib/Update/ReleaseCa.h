#pragma once

/*
 * The PINNED trust anchor for the WiFi OTA release fetch (spec 9.5, N-62).
 *
 * Spec 9.5 requires the fetch be verified "against a pinned CA certificate --
 * not `setInsecure()`". Using IDF's `esp_crt_bundle_attach` (the stock ~200-root
 * Mozilla bundle) authenticates the channel but is NOT pinned: any of those roots
 * (or any CA coerced into issuing for the release host) can vouch for the manifest
 * and the image. This header holds the actual root that signs GitHub Releases, so
 * the device trusts EXACTLY that root and nothing else.
 *
 * **Which root, and why this one.** GitHub's TLS chain is
 * github.com -> Sectigo Public Server Authentication CA DV E36 -> **Sectigo Public
 * Server Authentication Root E46** (itself issued by USERTrust ECC CA). The ROOT
 * (E46) is what is pinned here, not an intermediate: an intermediate rotates
 * (Sectigo reissues the DV E36 regularly), so pinning it would break the fetch on
 * the next rotation, while the root is valid to **2038-01-18**. The server
 * presents the intermediate, so a root-only pin validates the full chain.
 *
 * **Maintenance is a real cost, accepted deliberately:** a pinned CA does not
 * self-update. If this root is renewed or replaced, the release fetch fails at TLS
 * against the pinned anchor (loudly -- `OtaWifiCheck` now logs the mbedTLS error),
 * and this PEM must be updated in the same change that moves the release host. The
 * `check_release_ca.py` gate records the notAfter date so the expiry is visible.
 *
 * Source: the leaf chain of `github.com:443` (openssl s_client -showcerts), the
 * last certificate = the root.
 */
static const char kReleaseCaPem[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDRjCCAsugAwIBAgIQGp6v7G3o4ZtcGTFBto2Q3TAKBggqhkjOPQQDAzCBiDEL\n"
    "MAkGA1UEBhMCVVMxEzARBgNVBAgTCk5ldyBKZXJzZXkxFDASBgNVBAcTC0plcnNl\n"
    "eSBDaXR5MR4wHAYDVQQKExVUaGUgVVNFUlRSVVNUIE5ldHdvcmsxLjAsBgNVBAMT\n"
    "JVVTRVJUcnVzdCBFQ0MgQ2VydGlmaWNhdGlvbiBBdXRob3JpdHkwHhcNMjEwMzIy\n"
    "MDAwMDAwWhcNMzgwMTE4MjM1OTU5WjBfMQswCQYDVQQGEwJHQjEYMBYGA1UEChMP\n"
    "U2VjdGlnbyBMaW1pdGVkMTYwNAYDVQQDEy1TZWN0aWdvIFB1YmxpYyBTZXJ2ZXIg\n"
    "QXV0aGVudGljYXRpb24gUm9vdCBFNDYwdjAQBgcqhkjOPQIBBgUrgQQAIgNiAAR2\n"
    "+pmpbiDt+dd34wc7qNs9Xzjoq1WmVk/WSOrsfy2qw7LFeeyZYX8QeccCWvkEN/U0\n"
    "NSt3zn8gj1KjAIns1aeibVvjS5KToID1AZTc8GgHHs3u/iVStSBDHBv+6xnOQ6Oj\n"
    "ggEgMIIBHDAfBgNVHSMEGDAWgBQ64QmG1M8ZwpZ2dEl23OA1xmNjmjAdBgNVHQ4E\n"
    "FgQU0SLaTFnxS18mOKqd1u7rDcP7qWEwDgYDVR0PAQH/BAQDAgGGMA8GA1UdEwEB\n"
    "/wQFMAMBAf8wHQYDVR0lBBYwFAYIKwYBBQUHAwEGCCsGAQUFBwMCMBEGA1UdIAQK\n"
    "MAgwBgYEVR0gADBQBgNVHR8ESTBHMEWgQ6BBhj9odHRwOi8vY3JsLnVzZXJ0cnVz\n"
    "dC5jb20vVVNFUlRydXN0RUNDQ2VydGlmaWNhdGlvbkF1dGhvcml0eS5jcmwwNQYI\n"
    "KwYBBQUHAQEEKTAnMCUGCCsGAQUFBzABhhlodHRwOi8vb2NzcC51c2VydHJ1c3Qu\n"
    "Y29tMAoGCCqGSM49BAMDA2kAMGYCMQCMCyBit99vX2ba6xEkDe+YO7vC0twjbkv9\n"
    "PKpqGGuZ61JZryjFsp+DFpEclCVy4noCMQCwvZDXD/m2Ko1HA5Bkmz7YQOFAiNDD\n"
    "49IWa2wdT7R3DtODaSXH/BiXv8fwB9su4tU=\n"
    "-----END CERTIFICATE-----\n";

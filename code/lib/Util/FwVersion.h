#pragma once

/*
 * The firmware version string, as reported in the spec 4.3 `hello` frame.
 *
 * PlatformIO's build_flags define the RAW pair (`SWC_FW_VERSION_RAW`,
 * `SWC_GIT_SHA_RAW`) because the value must be stringified through a macro layer:
 * a `-D` value cannot contain an unescaped quote, so the raw form is passed and
 * the quoting happens here. That two-step is why this header exists rather than
 * the build defining the final name directly.
 *
 * When the value is genuinely absent (a bare host build, a fresh clone with no
 * release env), the fallbacks below keep every consumer compiling. They are
 * DELIBERATELY obviously-not-a-release strings: a device reporting "dev-unknown"
 * is honest, and a device reporting a plausible-looking version number it does
 * not have is not.
 */

#define SWC_STR_INNER(x) #x
#define SWC_STR(x) SWC_STR_INNER(x)

#ifdef SWC_FW_VERSION_RAW
#define SWC_FW_VERSION SWC_STR(SWC_FW_VERSION_RAW)
#else
#define SWC_FW_VERSION "dev-unknown"
#endif

#ifdef SWC_GIT_SHA_RAW
#define SWC_GIT_SHA SWC_STR(SWC_GIT_SHA_RAW)
#else
#define SWC_GIT_SHA "unknown"
#endif

#pragma once

#include <stddef.h>

/*
 * The HAND-WRITTEN half of the firmware/app contract. Task 19's generator
 * produces the other half (`swc_contract.h`) and asserts the two agree.
 *
 * Why this file exists at all, rather than everything being generated: the
 * generator is a later task, and a consumer that cannot compile until its
 * producer exists is a forward dependency. The frame vocabulary is likewise a
 * closed set that the framing layer must already know about.
 *
 * So the split is: **names and shapes that exist before the generator runs live
 * here; anything the generator derives from them is generated and CI-diffed.**
 * The generator's job is then anti-drift -- it re-derives and COMPARES -- which
 * is what actually catches a divergence. A generated file that nothing compares
 * against a source drifts exactly as freely as a hand-written one.
 */

/*
 * The one function-pointer type that hands a frame to a transport.
 *
 * Defined here because TWO tasks carry it: `UsbCdc` (the producer) and
 * `CommandRouter` (which stores it). Declaring it in both is the duplicate-name
 * defect the contract exists to prevent.
 *
 * The `line` a sink receives is **exactly one frame, WITHOUT its trailing
 * newline** -- the transport owns the newline, so a sink that also adds one does
 * not emit a double-spaced link.
 */
typedef void (*FrameSink)(void *ctx, const char *line, size_t len);

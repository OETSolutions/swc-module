#include "Bindings/ActionLibrary.h"
#include <gtest/gtest.h>
#include <cstring>

namespace {
Action HwKey(uint16_t code) {
    Action a{};
    a.kind = ActionKind::kHwKey;
    a.dac_code = code;
    return a;
}
Action Intent(const char *action, const char *data) {
    Action a{};
    a.kind = ActionKind::kAppIntent;
    std::strncpy(a.target, action, sizeof(a.target) - 1);
    std::strncpy(a.payload, data, sizeof(a.payload) - 1);
    return a;
}
}  // namespace

TEST(ActionLibrary, EverySpecKindIsExecutable) {
    // Spec 3.6's eleven kinds, each given the field its own row requires. A kind
    // that cannot be made executable is a kind the firmware cannot run.
    Action none{};            none.kind = ActionKind::kNone;
    Action release{};         release.kind = ActionKind::kHwKeyRelease;
    Action launch{};          launch.kind = ActionKind::kAppLaunch;
    std::strncpy(launch.target, "com.spotify.music", sizeof(launch.target) - 1);
    Action keycode{};         keycode.kind = ActionKind::kKeycode;
    std::strncpy(keycode.target, "KEYCODE_MEDIA_NEXT", sizeof(keycode.target) - 1);
    Action media{};           media.kind = ActionKind::kMedia;
    std::strncpy(media.target, "next", sizeof(media.target) - 1);
    Action volume{};          volume.kind = ActionKind::kVolume;
    std::strncpy(volume.target, "media", sizeof(volume.target) - 1);
    Action system{};          system.kind = ActionKind::kSystem;
    std::strncpy(system.target, "screen_off", sizeof(system.target) - 1);
    Action buzz{};            buzz.kind = ActionKind::kBuzzer;
    std::strncpy(buzz.target, "kBootOk", sizeof(buzz.target) - 1);
    Action raw{};             raw.kind = ActionKind::kAppRaw;
    std::strncpy(raw.target, "ping", sizeof(raw.target) - 1);

    EXPECT_TRUE(ActionIsExecutable(none));
    EXPECT_TRUE(ActionIsExecutable(release));
    EXPECT_TRUE(ActionIsExecutable(launch));
    EXPECT_TRUE(ActionIsExecutable(HwKey(1240)));
    EXPECT_TRUE(ActionIsExecutable(Intent("com.oetsolutions.swc.ACTION_NAVIGATE", "geo:1,2")));
    EXPECT_TRUE(ActionIsExecutable(keycode));
    EXPECT_TRUE(ActionIsExecutable(media));
    EXPECT_TRUE(ActionIsExecutable(volume));
    EXPECT_TRUE(ActionIsExecutable(system));
    EXPECT_TRUE(ActionIsExecutable(buzz));
    EXPECT_TRUE(ActionIsExecutable(raw));
}

TEST(ActionLibrary, HwKeyMustCarryALevelOneWayOrTheOther) {
    // Spec 3.6: HW_KEY takes `key_resistance_mohm`, OR `dac_code`. Neither means
    // the output would be driven to a level nothing defined -- the exact fault
    // spec 6.7 exists to prevent, so it is refused rather than guessed.
    Action neither{};
    neither.kind = ActionKind::kHwKey;
    EXPECT_FALSE(ActionIsExecutable(neither));

    Action by_code = HwKey(1240);
    EXPECT_TRUE(ActionIsExecutable(by_code));

    Action by_resistance{};
    by_resistance.kind = ActionKind::kHwKey;
    by_resistance.key_resistance_mohm = 24000;   // spec 3.7's b1
    EXPECT_TRUE(ActionIsExecutable(by_resistance));
}

TEST(ActionLibrary, AnIntentWithNoActionIsNotAnIntent) {
    Action a{};
    a.kind = ActionKind::kAppIntent;
    std::strncpy(a.payload, "geo:1,2", sizeof(a.payload) - 1);   // data, but no action
    EXPECT_FALSE(ActionIsExecutable(a)) << "the action is what the intent DOES";
}

TEST(ActionLibrary, APayloadIsMeaningfulOnlyForTheKindsThatUseOne) {
    // The flag is a property of the kind, not of a stored action (spec 3.6).
    EXPECT_TRUE(ActionTakesPayload(ActionKind::kAppIntent));
    EXPECT_TRUE(ActionTakesPayload(ActionKind::kAppRaw));
    EXPECT_FALSE(ActionTakesPayload(ActionKind::kHwKey));
    EXPECT_FALSE(ActionTakesPayload(ActionKind::kNone));
    EXPECT_FALSE(ActionTakesPayload(ActionKind::kHwKeyRelease));
}

TEST(ActionLibrary, AKindOutsideTheSpecIsRefusedNotGuessed) {
    Action a{};
    a.kind = static_cast<ActionKind>(200);   // no such kind
    EXPECT_FALSE(ActionIsExecutable(a));
}

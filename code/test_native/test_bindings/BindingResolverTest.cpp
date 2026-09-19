#include "Bindings/BindingResolver.h"
#include <gtest/gtest.h>
#include <cstring>

namespace {
// Spec 3.7's worked example: one channel, three buttons, idle 2835 mV, and the
// bindings that make the product's core case real -- vol_up SINGLE drives a
// the output (works with no app) while next DOUBLE launches an app (the app's extra).
Config MakeConfig() {
    Config c{};
    c.schema_version = kConfigSchemaVersion;
    c.channel_count = 1;
    std::strncpy(c.channels[0].name, "SWC1", sizeof(c.channels[0].name) - 1);
    c.channels[0].enabled = true;
    c.channels[0].ladder.learned_idle_mv = 2835;
    c.channels[0].ladder.count = 3;
    c.channels[0].ladder.buttons[0] = {"vol_up", "Volume Up",   1430, 120, 3300, 235, 200, 98};
    c.channels[0].ladder.buttons[1] = {"vol_dn", "Volume Down", 1785, 120, 3300, 235, 200, 97};
    c.channels[0].ladder.buttons[2] = {"next",   "Next Track",  2145, 110, 3300, 235, 200, 99};

    c.binding_count = 4;
    std::strncpy(c.bindings[0].id, "b1", sizeof(c.bindings[0].id) - 1);
    c.bindings[0].channel = static_cast<uint8_t>(BindingChannel::kSwc1);
    std::strncpy(c.bindings[0].button, "vol_up", sizeof(c.bindings[0].button) - 1);
    c.bindings[0].gesture = Gesture::kSingle;
    c.bindings[0].enabled = true;
    c.bindings[0].action_count = 1;
    c.bindings[0].actions[0].kind = ActionKind::kOutVoltage;
    c.bindings[0].actions[0].key_mv = 2400;   // spec 3.7's b1

    std::strncpy(c.bindings[1].id, "b3", sizeof(c.bindings[1].id) - 1);
    c.bindings[1].channel = static_cast<uint8_t>(BindingChannel::kSwc1);
    std::strncpy(c.bindings[1].button, "next", sizeof(c.bindings[1].button) - 1);
    c.bindings[1].gesture = Gesture::kDouble;
    c.bindings[1].enabled = true;
    c.bindings[1].action_count = 1;
    c.bindings[1].actions[0].kind = ActionKind::kAppLaunch;
    std::strncpy(c.bindings[1].actions[0].target, "com.spotify.music",
                 sizeof(c.bindings[1].actions[0].target) - 1);

    // b4 is spec 3.7's APP_INTENT: the kind that actually carries a data payload.
    // (b3 is APP_LAUNCH, which takes a `package` and no data -- pairing a payload
    // with APP_LAUNCH is the kind of shape this model exists to make impossible.)
    std::strncpy(c.bindings[3].id, "b4", sizeof(c.bindings[3].id) - 1);
    c.bindings[3].channel = static_cast<uint8_t>(BindingChannel::kSwc1);
    std::strncpy(c.bindings[3].button, "next", sizeof(c.bindings[3].button) - 1);
    c.bindings[3].gesture = Gesture::kLong;
    c.bindings[3].enabled = true;
    c.bindings[3].action_count = 1;
    c.bindings[3].actions[0].kind = ActionKind::kAppIntent;
    std::strncpy(c.bindings[3].actions[0].target, "com.oetsolutions.swc.ACTION_NAVIGATE",
                 sizeof(c.bindings[3].actions[0].target) - 1);
    std::strncpy(c.bindings[3].actions[0].payload, "geo:40.7608,-111.8910?q=Home",
                 sizeof(c.bindings[3].actions[0].payload) - 1);

    // b2 is on channel 2, which this config does not even enable: resolving a
    // SWC1 gesture must never find it.
    std::strncpy(c.bindings[2].id, "b2", sizeof(c.bindings[2].id) - 1);
    c.bindings[2].channel = static_cast<uint8_t>(BindingChannel::kSwc2);
    std::strncpy(c.bindings[2].button, "vol_up", sizeof(c.bindings[2].button) - 1);
    c.bindings[2].gesture = Gesture::kSingle;
    c.bindings[2].enabled = true;
    c.bindings[2].action_count = 1;
    c.bindings[2].actions[0].kind = ActionKind::kOutRelease;
    return c;
}
GestureEvent Ev(Gesture g, uint8_t b) { return GestureEvent{g, b, 0}; }
}  // namespace

TEST(BindingResolver, ResolvesAButtonsSinglePressToItsAction) {
    const ResolvedAction r = BindingResolve(MakeConfig(), 0, Ev(Gesture::kSingle, 0));
    ASSERT_TRUE(r.found);
    EXPECT_EQ(r.action.kind, ActionKind::kOutVoltage);
    EXPECT_EQ(r.action.key_mv, 2400);
}

TEST(BindingResolver, CarriesTheDataPayloadThroughUntouched) {
    // b4, spec 3.7's APP_INTENT. A binding's payload is stored in the ACTION, and
    // this asserts it arrives byte for byte rather than being re-derived.
    const ResolvedAction r = BindingResolve(MakeConfig(), 0, Ev(Gesture::kLong, 2));
    ASSERT_TRUE(r.found);
    EXPECT_EQ(r.action.kind, ActionKind::kAppIntent);
    EXPECT_STREQ(r.action.target, "com.oetsolutions.swc.ACTION_NAVIGATE");
    EXPECT_STREQ(r.action.payload, "geo:40.7608,-111.8910?q=Home");
}

TEST(BindingResolver, ABindingOnAnotherChannelIsNotResolved) {
    // b2 binds vol_up SINGLE on SWC2. Resolving a SWC1 event must not find it --
    // this is the check the earlier per-channel revision could not express.
    const ResolvedAction r = BindingResolve(MakeConfig(), 0, Ev(Gesture::kSingle, 0));
    ASSERT_TRUE(r.found);
    EXPECT_NE(r.action.kind, ActionKind::kOutRelease) << "that binding is SWC2's";
}

TEST(BindingResolver, AnyChannelIsHonouredFromEitherChannel) {
    Config c = MakeConfig();
    c.bindings[0].channel = static_cast<uint8_t>(BindingChannel::kAny);
    c.channel_count = 2;
    c.channels[1] = c.channels[0];
    EXPECT_TRUE(BindingResolve(c, 0, Ev(Gesture::kSingle, 0)).found);
    EXPECT_TRUE(BindingResolve(c, 1, Ev(Gesture::kSingle, 0)).found)
        << "ANY means one binding honoured from either steering-wheel channel";
}

TEST(BindingResolver, UnboundGestureIsNotFoundRatherThanDefaultingToSomething) {
    const ResolvedAction r = BindingResolve(MakeConfig(), 0, Ev(Gesture::kLong, 0));
    EXPECT_FALSE(r.found) << "an unbound gesture must do nothing, not act by accident";
}

TEST(BindingResolver, ResolvesTheSameGestureDifferentlyPerButton) {
    const Config c = MakeConfig();
    EXPECT_TRUE(BindingResolve(c, 0, Ev(Gesture::kSingle, 0)).found);   // vol_up SINGLE
    EXPECT_FALSE(BindingResolve(c, 0, Ev(Gesture::kSingle, 2)).found);  // next SINGLE
}

TEST(BindingResolver, AnEmptyActionListSwallowsTheGestureWithoutActing) {
    // Spec 3.5: empty actions is NOT the same as enabled:false -- the gesture is
    // recognised and does nothing, so no lower-priority binding may take it.
    Config c = MakeConfig();
    c.bindings[0].action_count = 0;
    const ResolvedAction r = BindingResolve(c, 0, Ev(Gesture::kSingle, 0));
    EXPECT_TRUE(r.found);
    EXPECT_EQ(r.action.kind, ActionKind::kNone) << "recognised, and deliberately inert";
}

TEST(BindingResolver, ADisabledBindingIsSkippedNotSwallowed) {
    Config c = MakeConfig();
    c.bindings[0].enabled = false;
    EXPECT_FALSE(BindingResolve(c, 0, Ev(Gesture::kSingle, 0)).found)
        << "disabled means do not match at all, so a later binding may";
}

TEST(BindingResolver, AnUnExecutableActionIsRefusedRatherThanDropped) {
    Config c = MakeConfig();
    c.bindings[0].actions[0].key_mv = 0;   // OUT_VOLTAGE with no level
    EXPECT_FALSE(BindingResolve(c, 0, Ev(Gesture::kSingle, 0)).found);
}

TEST(BindingResolver, AnActionListTooLongIsRefusedRatherThanTruncated) {
    Config c = MakeConfig();
    c.bindings[0].action_count = kMaxActionsPerBinding + 1;
    EXPECT_FALSE(BindingResolve(c, 0, Ev(Gesture::kSingle, 0)).found)
        << "a count past the array is a corrupt config, not a shorter action list";
}

// The three counts below are the same rule as action_count, on the three other
// arrays the resolver indexes. Each is a uint8_t read straight from storage, so
// each is an out-of-bounds read if it is trusted -- and each is reachable, since
// a corrupt or hand-edited config is exactly what the store's CRC is there to
// catch, and the CRC does not run on every path.
TEST(BindingResolver, ABindingCountPastTheTableIsRefusedRatherThanRead) {
    Config c = MakeConfig();
    c.binding_count = kMaxBindings + 1;
    EXPECT_FALSE(BindingResolve(c, 0, Ev(Gesture::kSingle, 0)).found);
}

TEST(BindingResolver, ALadderCountPastTheButtonArrayIsRefusedRatherThanRead) {
    Config c = MakeConfig();
    c.channels[0].ladder.count = kLadderMaxButtons + 1;
    // Index 0 is in range either way; the count is what decides whether the
    // loop's bound is meaningful, so the refusal must not depend on the index.
    EXPECT_FALSE(BindingResolve(c, 0, Ev(Gesture::kSingle, 0)).found);
}

TEST(BindingResolver, AButtonIndexPastTheLadderIsRefusedRatherThanRead) {
    Config c = MakeConfig();
    EXPECT_FALSE(BindingResolve(c, 0, Ev(Gesture::kSingle, 3)).found)
        << "ladder.count is 3, so index 3 is one past the last button";
}

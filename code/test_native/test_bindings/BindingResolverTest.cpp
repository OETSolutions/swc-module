#include "Bindings/BindingResolver.h"
#include <gtest/gtest.h>
#include <cstring>

// The AUX tests below assert the VALIDATOR's refusal of an AUX1 binding, because
// that is where the rule lives and the only gate between a hand-built config and
// the wire. The resolver deliberately resolves whatever index it is handed.
#include "Config/ConfigCodec.h"
#include "ConfigFixtures.h"

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

/*
 * Give the config two AUX inputs -- AUX2 and AUX3 -- with the press window
 * `Aux1ProfileDefault` uses (a switch to the rail, shorted to ground when
 * pressed). AUX1 is deliberately absent from the table's first slot in these
 * tests: it is the programming/maintenance hold and is not a gesture source
 * (`ConfigValidate` refuses a binding on it), so a fixture that made AUX1 the
 * bindable one would be testing a config the device never accepts.
 *
 * The ids are the OTHER half of a binding's identity -- `Binding.button` names an
 * `AuxButtonConfig.id` for this family, exactly as it names a `LadderButton.id`
 * for a wheel channel (spec 3.1/3.5) -- so they are distinct from the ladder's.
 */
void AddAuxInputs(Config &c) {
    c.aux_count = kMaxAuxButtons;
    c.aux[0] = {"aux1", 1, 100, 1600};   // present but not bindable
    c.aux[1] = {"aux2", 2, 100, 1600};
    c.aux[2] = {"aux3", 3, 100, 1600};
}

// One binding on an AUX input, appended after the fixture's own (so its position
// makes it the LAST match, never a precedence override of a wheel binding).
void AddAuxBinding(Config &c, const char *button, BindingChannel ch, Gesture g) {
    const uint8_t i = c.binding_count++;
    Binding &b = c.bindings[i];
    b = Binding{};
    std::strncpy(b.id, "auxbind", sizeof(b.id) - 1);
    b.channel = static_cast<uint8_t>(ch);
    std::strncpy(b.button, button, sizeof(b.button) - 1);
    b.gesture = g;
    b.enabled = true;
    b.action_count = 1;
    b.actions[0].kind = ActionKind::kOutVoltage;
    // A DISTINCT level from the fixture's own wheel binding (2400), so a test can
    // tell which binding a resolve actually found.
    b.actions[0].key_mv = 2500;
}
}  // namespace

TEST(BindingResolver, ResolvesAButtonsSinglePressToItsAction) {
    const ResolvedBinding r = BindingResolve(MakeConfig(), 0, Ev(Gesture::kSingle, 0));
    ASSERT_TRUE(r.found);
    EXPECT_EQ(r.actions[0].kind, ActionKind::kOutVoltage);
    EXPECT_EQ(r.actions[0].key_mv, 2400);
}

TEST(BindingResolver, CarriesTheDataPayloadThroughUntouched) {
    // b4, spec 3.7's APP_INTENT. A binding's payload is stored in the ACTION, and
    // this asserts it arrives byte for byte rather than being re-derived.
    const ResolvedBinding r = BindingResolve(MakeConfig(), 0, Ev(Gesture::kLong, 2));
    ASSERT_TRUE(r.found);
    EXPECT_EQ(r.actions[0].kind, ActionKind::kAppIntent);
    EXPECT_STREQ(r.actions[0].target, "com.oetsolutions.swc.ACTION_NAVIGATE");
    EXPECT_STREQ(r.actions[0].payload, "geo:40.7608,-111.8910?q=Home");
}

TEST(BindingResolver, ABindingOnAnotherChannelIsNotResolved) {
    // b2 binds vol_up SINGLE on SWC2. Resolving a SWC1 event must not find it --
    // this is the check the earlier per-channel revision could not express.
    const ResolvedBinding r = BindingResolve(MakeConfig(), 0, Ev(Gesture::kSingle, 0));
    ASSERT_TRUE(r.found);
    EXPECT_NE(r.actions[0].kind, ActionKind::kOutRelease) << "that binding is SWC2's";
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
    const ResolvedBinding r = BindingResolve(MakeConfig(), 0, Ev(Gesture::kLong, 0));
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
    const ResolvedBinding r = BindingResolve(c, 0, Ev(Gesture::kSingle, 0));
    EXPECT_TRUE(r.found);
    EXPECT_EQ(r.action_count, 0) << "recognised, with nothing to run -- deliberately inert";
}

TEST(BindingResolver, CarriesTheWholeOrderedActionList) {
    // Spec 3.5's product case: ONE binding whose SINGLE is "emit the factory key
    // press AND tell the app". Both halves must reach the caller IN ORDER; the
    // single-`actions[0]` revision dropped the second, and the caller's
    // first-action-only branch dropped BOTH when the app action came first.
    Config c = MakeConfig();
    c.bindings[0].action_count = 2;
    c.bindings[0].actions[0].kind = ActionKind::kAppIntent;
    std::strncpy(c.bindings[0].actions[0].target, "com.oetsolutions.swc.ACTION_NAVIGATE",
                 sizeof(c.bindings[0].actions[0].target) - 1);
    c.bindings[0].actions[1].kind = ActionKind::kOutVoltage;
    c.bindings[0].actions[1].key_mv = 2400;
    const ResolvedBinding r = BindingResolve(c, 0, Ev(Gesture::kSingle, 0));
    ASSERT_TRUE(r.found);
    ASSERT_EQ(r.action_count, 2);
    EXPECT_EQ(r.actions[0].kind, ActionKind::kAppIntent)
        << "position is preserved -- the caller executes in order";
    EXPECT_EQ(r.actions[1].kind, ActionKind::kOutVoltage);
    EXPECT_EQ(r.actions[1].key_mv, 2400);
}

TEST(BindingResolver, AnUnExecutableActionAnywhereInTheListIsRefused) {
    // A malformed action is refused for the WHOLE binding, not silently dropped
    // from the tail: now that the caller runs every action, an unchecked second
    // action is reachable rather than dead.
    Config c = MakeConfig();
    c.bindings[0].action_count = 2;
    c.bindings[0].actions[0].kind = ActionKind::kOutVoltage;
    c.bindings[0].actions[0].key_mv = 2400;
    c.bindings[0].actions[1].kind = ActionKind::kOutVoltage;
    c.bindings[0].actions[1].key_mv = 0;   // no level
    EXPECT_FALSE(BindingResolve(c, 0, Ev(Gesture::kSingle, 0)).found);
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

// --- the AUX inputs, spec 3.1/3.5 ---------------------------------------------
//
// The AUX family used to be accepted-and-inert (open item N-26): `Config` carried
// the `aux[3]` table, the validator accepted a binding naming one, and NO firmware
// path resolved it -- so the app could store an AUX2 binding, read it back, and
// watch it never fire. What made the fix possible is the shared scan taking the
// binding's `channel` ordinal and the input's own id, which is the two facts a
// binding actually matches on; `Binding.resolve` could not see an AUX input at all
// because that family's ids live in `cfg.aux`, not in a channel's ladder.

TEST(BindingResolver, ResolvesAnAuxInputsGestureToItsAction) {
    Config c = MakeConfig();
    AddAuxInputs(c);
    AddAuxBinding(c, "aux2", BindingChannel::kAux2, Gesture::kSingle);
    const ResolvedBinding r = BindingResolveAux(c, 1, Ev(Gesture::kSingle, 0));
    ASSERT_TRUE(r.found) << "an AUX2 binding must be reachable from the AUX2 input";
    ASSERT_EQ(r.action_count, 1);
    EXPECT_EQ(r.actions[0].kind, ActionKind::kOutVoltage);
    EXPECT_EQ(r.actions[0].key_mv, 2500);
}

TEST(BindingResolver, AnAuxInputIsMatchedByItsOwnIdNotByIndex) {
    // The two facts a binding matches on are its channel and the INPUT's id. Two
    // AUX inputs with the same window but different ids must not be
    // interchangeable: resolving AUX3 must not find the AUX2 binding.
    Config c = MakeConfig();
    AddAuxInputs(c);
    AddAuxBinding(c, "aux2", BindingChannel::kAux2, Gesture::kSingle);
    EXPECT_FALSE(BindingResolveAux(c, 2, Ev(Gesture::kSingle, 0)).found)
        << "AUX3 pressed: the id differs, so the AUX2 binding must not fire";
}

TEST(BindingResolver, AnAuxBindingDoesNotFireForAWheelPress) {
    // The mirror of `ABindingOnAnotherChannelIsNotResolved`, for the other family:
    // the two families share ONE scan, so a press on SWC1's `vol_up` must resolve
    // SWC1's own binding and never the AUX2 one. The two bindings carry different
    // levels (2400 vs 2500), so "which binding was found" is decidable -- an
    // assertion on `found` alone could not tell them apart.
    Config c = MakeConfig();
    AddAuxInputs(c);
    AddAuxBinding(c, "aux2", BindingChannel::kAux2, Gesture::kSingle);
    const ResolvedBinding r = BindingResolve(c, 0, Ev(Gesture::kSingle, 0));
    ASSERT_TRUE(r.found);
    EXPECT_EQ(r.actions[0].key_mv, 2400)
        << "a wheel press must resolve the wheel's binding, not the AUX one";
}

TEST(BindingResolver, AnyChannelAlsoCoversTheAuxInputs) {
    // Spec 3.5's `ANY` is a real wildcard over the whole input enum, so a binding
    // written with it must be honoured from an AUX input too. Restricting ANY to
    // the wheel channels would silently make it a SWC-only alias.
    Config c = MakeConfig();
    AddAuxInputs(c);
    AddAuxBinding(c, "aux2", BindingChannel::kAny, Gesture::kLong);
    EXPECT_TRUE(BindingResolveAux(c, 1, Ev(Gesture::kLong, 0)).found)
        << "ANY must be honoured from an AUX input as well";
}

TEST(BindingResolver, AnAuxInputHasExactlyOneButton) {
    // An AUX input is a single switch with a single id, so `ev.button_index` can
    // only be 0. Any other value names a button this input does not have, and
    // reading the config as if it had one would resolve a binding for a button
    // the user never pressed.
    Config c = MakeConfig();
    AddAuxInputs(c);
    AddAuxBinding(c, "aux2", BindingChannel::kAux2, Gesture::kSingle);
    EXPECT_FALSE(BindingResolveAux(c, 1, Ev(Gesture::kSingle, 1)).found)
        << "an AUX input has no button index 1";
}

TEST(BindingResolver, AnAuxCountPastTheTableIsRefusedRatherThanRead) {
    // The same count-is-untrusted rule as every other array, on the AUX table.
    Config c = MakeConfig();
    AddAuxInputs(c);
    AddAuxBinding(c, "aux2", BindingChannel::kAux2, Gesture::kSingle);
    c.aux_count = static_cast<uint8_t>(kMaxAuxButtons + 1);
    EXPECT_FALSE(BindingResolveAux(c, 1, Ev(Gesture::kSingle, 0)).found);
}

TEST(BindingResolver, Aux1IsRefusedAsABindingSourceByTheValidator) {
    // AUX1 carries spec 7.5's 1.5 s programming hold and spec 8.2's 3 s
    // maintenance hold, so a binding on the same switch would be ambiguous with
    // them. The decision lives in the validator (`ConfigValidate`), which is the
    // single gate between a hand-built config and the wire -- so this asserts the
    // refusal where it is made rather than in the resolver, which deliberately
    // resolves whatever index it is handed.
    //
    // This needs a config that is valid in every OTHER respect, or the refusal
    // would be indistinguishable from a refusal for some unrelated reason. The
    // local `MakeConfig` is not that config -- it has no `device_id`, which the
    // validator refuses on its first line -- so the suite's shared, fully valid
    // fixture is used instead. That distinction is the whole test: it asserts the
    // config is accepted, then refused after ONE field changes.
    Config c = swctest::MakeConfig();
    AddAuxInputs(c);
    ASSERT_TRUE(ConfigValidate(c))
        << "fixture guard: the config must be valid before the AUX1 binding is added";
    AddAuxBinding(c, "aux1", BindingChannel::kAux1, Gesture::kSingle);
    EXPECT_FALSE(ConfigValidate(c))
        << "a binding on AUX1 must be refused: that switch is the programming button";
    // And the SAME config with the binding moved to AUX2 is accepted, so the
    // refusal is about AUX1 specifically and not about AUX bindings in general.
    c.bindings[c.binding_count - 1].channel =
        static_cast<uint8_t>(BindingChannel::kAux2);
    std::strncpy(c.bindings[c.binding_count - 1].button, "aux2",
                 sizeof(c.bindings[c.binding_count - 1].button) - 1);
    EXPECT_TRUE(ConfigValidate(c)) << "AUX2 and AUX3 ARE bindable";
}

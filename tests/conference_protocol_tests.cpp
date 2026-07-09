/*
 *  Copyright (C) 2004-2026 Savoir-faire Linux Inc.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA.
 */

// Unit tests for ConfProtocolParser (application/confOrder+json dispatch).
//
// Regression coverage for the defects cataloged in
// apps/svetophone/specs/conference-actions.md:
//   D1/D12 — a single missing optional handler used to make parseV0/parseV1
//            return early ("Missing methods"), silently dropping EVERY
//            incoming conference order.
//   D2     — parseV1 iterated accValue["medias"] instead of
//            deviceValue["medias"], so canonical (Jami-shaped) V1 orders
//            never dispatched media actions (muteAudio / active /
//            voiceActivity).
//   D3     — the muteVideo guard was inverted (`&& !muteStreamVideo_`),
//            invoking an empty std::function -> std::bad_function_call.
//   (new)  — parseV1's root loop hit the scalar "version" member with
//            Json::Value::isMember(), which throws Json::LogicError on
//            non-object values; latent while D1 masked it.
//
// The parser is self-contained (lambdas in, JSON in, dispatch out), so these
// are pure table tests — no Manager / account fixtures needed.

#include "conference_protocol.h"
#include "connectivity/sip_utils.h"

#include <json/json.h>

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using sip_core::ConfProtocolParser;

namespace {

void
fail(const std::string& message)
{
    std::cerr << "TEST FAILURE: " << message << "\n";
    std::exit(1);
}

void
expect_true(bool condition, const std::string& message)
{
    if (!condition) {
        fail(message);
    }
}

// Records every dispatch the parser makes, so each test asserts both the
// presence of expected calls and the absence of everything else.
struct Recorder
{
    std::vector<std::string> calls;

    void add(const std::string& s) { calls.push_back(s); }

    bool has(const std::string& s) const
    {
        for (const auto& c : calls)
            if (c == s)
                return true;
        return false;
    }

    size_t count() const { return calls.size(); }
};

// Wires every parser handler to the recorder. `moderator` controls the
// checkAuthorization_ answer.
void
wireAll(ConfProtocolParser& parser, Recorder& rec, bool moderator)
{
    parser.onVersion([&rec](uint32_t v) { rec.add("version:" + std::to_string(v)); });
    parser.onCheckAuthorization([moderator](std::string_view) { return moderator; });
    parser.onHangupParticipant([&rec](const std::string& uri, const std::string& dev) {
        rec.add("hangup:" + uri + "/" + dev);
    });
    parser.onRaiseHand([&rec](const std::string& sender,
                              const std::string& uri,
                              const std::string& dev,
                              bool state) {
        rec.add("raiseHand:" + uri + "/" + dev + ":" + (state ? "1" : "0") + "@" + sender);
    });
    parser.onSetActiveStream([&rec](const std::string& sid, bool state) {
        rec.add("active:" + sid + ":" + (state ? "1" : "0"));
    });
    parser.onMuteStreamAudio([&rec](const std::string& uri,
                                    const std::string& dev,
                                    const std::string& sid,
                                    bool state) {
        rec.add("muteAudio:" + uri + "/" + dev + "/" + sid + ":" + (state ? "1" : "0"));
    });
    parser.onSetLayout([&rec](int layout) { rec.add("layout:" + std::to_string(layout)); });
    parser.onKickParticipant([&rec](const std::string& uri) { rec.add("kick:" + uri); });
    parser.onSetActiveParticipant(
        [&rec](const std::string& uri) { rec.add("activePart:" + uri); });
    parser.onMuteParticipant([&rec](const std::string& uri, bool state) {
        rec.add("mutePart:" + uri + ":" + (state ? "1" : "0"));
    });
    parser.onRaiseHandUri([&rec](const std::string& sender, const std::string& uri, bool state) {
        rec.add("raiseHandUri:" + uri + ":" + (state ? "1" : "0") + "@" + sender);
    });
    parser.onVoiceActivity([&rec](const std::string& sid, bool state) {
        rec.add("voice:" + sid + ":" + (state ? "1" : "0"));
    });
    // muteStreamVideo intentionally NOT wired: production leaves it
    // unimplemented, and D3 was a crash on exactly this configuration.
}

Json::Value
parseJson(const std::string& text)
{
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string err;
    auto reader = std::unique_ptr<Json::CharReader>(builder.newCharReader());
    if (!reader->parse(text.data(), text.data() + text.size(), &root, &err))
        fail("test JSON does not parse: " + err);
    return root;
}

void
runBuiltOrder(Recorder& rec, bool moderator, std::string_view peerId, Json::Value order)
{
    ConfProtocolParser parser;
    wireAll(parser, rec, moderator);
    parser.initData(std::move(order), peerId);
    parser.parse();
}

void
runOrder(Recorder& rec, bool moderator, std::string_view peerId, const std::string& json)
{
    runBuiltOrder(rec, moderator, peerId, parseJson(json));
}

// ---- V0 ----

void
test_v0_moderator_full_dispatch()
{
    Recorder rec;
    runOrder(rec,
             true,
             "alice",
             R"({"layout": 2,
                 "activeParticipant": "bob",
                 "muteParticipant": "bob", "muteState": "true",
                 "hangupParticipant": "carl",
                 "handRaised": "alice", "handState": "true"})");
    expect_true(rec.has("layout:2"), "V0 moderator: layout not dispatched");
    expect_true(rec.has("activePart:bob"), "V0 moderator: activeParticipant not dispatched");
    expect_true(rec.has("mutePart:bob:1"), "V0 moderator: muteParticipant not dispatched");
    expect_true(rec.has("kick:carl"), "V0 moderator: hangupParticipant not dispatched");
    expect_true(rec.has("raiseHandUri:alice:1@alice"), "V0: own hand raise not dispatched");
}

void
test_v0_non_moderator_rejected()
{
    Recorder rec;
    runOrder(rec,
             false,
             "alice",
             R"({"layout": 1,
                 "activeParticipant": "bob",
                 "muteParticipant": "bob", "muteState": "true",
                 "hangupParticipant": "carl",
                 "handRaised": "alice", "handState": "true"})");
    expect_true(rec.has("raiseHandUri:alice:1@alice"),
                "V0 non-moderator: own hand raise must still work");
    expect_true(rec.count() == 1,
                "V0 non-moderator: moderation actions must be rejected");
}

void
test_v0_moderator_lowers_other_hand_but_cannot_raise()
{
    Recorder lower;
    runOrder(lower, true, "alice", R"({"handRaised": "bob", "handState": "false"})");
    expect_true(lower.has("raiseHandUri:bob:0@alice"), "V0: moderator must lower bob's hand");

    // A raise addressed at someone else is treated as the sender's own raise
    // (clients stamp self-raises with a login the host may not recognize —
    // see test_v0_login_stamped_self_action below); it must never land on
    // the stamped third party.
    Recorder raise;
    runOrder(raise, true, "alice", R"({"handRaised": "bob", "handState": "true"})");
    expect_true(!raise.has("raiseHandUri:bob:1@alice"),
                "V0: moderator must NOT raise someone else's hand");
    expect_true(raise.has("raiseHandUri:alice:1@alice"),
                "V0: a raise stamped with a foreign uri must resolve to the sender");
}

void
test_v0_login_stamped_self_action()
{
    // Live bug (2026-07-09): the sender stamps its typed login ("m12") while
    // the host knows it by the extension it dialed ("74112"). The raise used
    // to be dropped silently by the peerId==uri gate.
    Recorder raise;
    runOrder(raise, false, "74112", R"({"handRaised": "m12", "handState": "true"})");
    expect_true(raise.has("raiseHandUri:74112:1@74112"),
                "V0: login-stamped self raise must resolve to the SIP sender");

    Recorder lower;
    runOrder(lower, false, "74112", R"({"handRaised": "m12", "handState": "false"})");
    expect_true(lower.has("raiseHandUri:74112:0@74112"),
                "V0: login-stamped self lower must resolve to the SIP sender");

    // From a moderator, a lower keeps the stamped target: the Conference
    // resolves it and falls back to the sender only when it matches nobody.
    Recorder modLower;
    runOrder(modLower, true, "74112", R"({"handRaised": "m12", "handState": "false"})");
    expect_true(modLower.has("raiseHandUri:m12:0@74112"),
                "V0: moderator lower must keep the stamped target with the sender attached");
}

void
test_v0_missing_optional_handler_keeps_other_dispatches()
{
    // D1/D12 regression: leave setLayout_ unregistered; the other actions
    // must still dispatch.
    Recorder rec;
    ConfProtocolParser parser;
    parser.onCheckAuthorization([](std::string_view) { return true; });
    parser.onKickParticipant([&rec](const std::string& uri) { rec.add("kick:" + uri); });
    parser.initData(parseJson(R"({"layout": 1, "hangupParticipant": "carl"})"), "alice");
    parser.parse();
    expect_true(rec.has("kick:carl"),
                "D1 regression: kick must dispatch even with setLayout_ unset");
}

// ---- V1 ----

// Canonical (Jami-compliant) V1 order shape.
std::string
canonicalV1(const std::string& uri, const std::string& dev, const std::string& body)
{
    return R"({"version": 1, ")" + uri + R"(": {"devices": {")" + dev + R"(": )" + body
           + R"(}}})";
}

void
test_v1_canonical_moderator_dispatch()
{
    Recorder rec;
    runOrder(rec,
             true,
             "alice",
             canonicalV1("bob",
                         "dev1",
                         R"({"hangup": true,
                             "medias": {"stream7": {"muteAudio": true,
                                                    "active": true,
                                                    "voiceActivity": true}}})"));
    expect_true(rec.has("version:1"), "V1: version callback not dispatched");
    expect_true(rec.has("hangup:bob/dev1"), "V1: hangup not dispatched");
    expect_true(rec.has("muteAudio:bob/dev1/stream7:1"),
                "D2 regression: canonical-shape muteAudio not dispatched");
    expect_true(rec.has("active:stream7:1"),
                "D2 regression: canonical-shape active not dispatched");
    expect_true(rec.has("voice:stream7:1"),
                "D2 regression: canonical-shape voiceActivity not dispatched");
}

void
test_v1_legacy_self_nested_shape_still_dispatches()
{
    // The pre-fix svetophone/sip_core senders self-nested the device object:
    //   root[uri] = {"medias": {...}, "devices": {dev: {"medias": {...}}}}
    // Interop with deployed builds requires this shape to keep working.
    Recorder rec;
    runOrder(rec,
             true,
             "alice",
             R"({"version": 1,
                 "bob": {"medias": {"stream7": {"muteAudio": true}},
                         "devices": {"dev1": {"medias": {"stream7": {"muteAudio": true}}}}}})");
    expect_true(rec.has("muteAudio:bob/dev1/stream7:1"),
                "legacy self-nested V1 muteAudio must keep dispatching");
}

void
test_v1_raise_hand_self_authorization()
{
    // Sender raises their own hand: allowed even for non-moderators, keyed by
    // the order's account uri matching the parser peerId.
    Recorder rec;
    runOrder(rec,
             false,
             "bob",
             canonicalV1("bob", "dev1", R"({"raiseHand": true})"));
    expect_true(rec.has("raiseHand:bob/dev1:1@bob"), "V1: self raiseHand must dispatch");

    // A hand order from a non-moderator can only be a self-action: it must
    // never land on the stamped third party, only on the sender itself.
    Recorder other;
    runOrder(other,
             false,
             "mallory",
             canonicalV1("bob", "dev1", R"({"raiseHand": false})"));
    expect_true(!other.has("raiseHand:bob/dev1:0@mallory"),
                "V1: non-moderator must not lower another participant's hand");
    expect_true(other.has("raiseHand:mallory/dev1:0@mallory"),
                "V1: non-moderator hand order must resolve to the sender's own hand");

    // A moderator may lower, but not raise, someone else's hand.
    Recorder lower;
    runOrder(lower, true, "alice", canonicalV1("bob", "dev1", R"({"raiseHand": false})"));
    expect_true(lower.has("raiseHand:bob/dev1:0@alice"), "V1: moderator must lower bob's hand");
    Recorder raise;
    runOrder(raise, true, "alice", canonicalV1("bob", "dev1", R"({"raiseHand": true})"));
    expect_true(!raise.has("raiseHand:bob/dev1:1@alice"),
                "V1: moderator must NOT raise someone else's hand");
    expect_true(raise.has("raiseHand:alice/dev1:1@alice"),
                "V1: a raise stamped with a foreign uri must resolve to the sender");
}

void
test_v1_login_stamped_self_action()
{
    // Live bug (2026-07-09): remote m12 raised its hand; the V1 order carried
    // its typed login ("m12") while the host knew the leg only by the dialed
    // extension ("74112"). The peerId==accountUri gate dropped every raise
    // silently. Such orders must dispatch as the sender's own action.
    Recorder raise;
    runOrder(raise, false, "74112", canonicalV1("m12", "", R"({"raiseHand": true})"));
    expect_true(raise.has("raiseHand:74112/:1@74112"),
                "V1: login-stamped self raise must resolve to the SIP sender");

    Recorder lower;
    runOrder(lower, false, "74112", canonicalV1("m12", "", R"({"raiseHand": false})"));
    expect_true(lower.has("raiseHand:74112/:0@74112"),
                "V1: login-stamped self lower must resolve to the SIP sender");

    // From a moderator (the common case under allModerators), a lower keeps
    // the stamped target and attaches the sender: Conference::setHandRaised
    // resolves the target and falls back to the sender when it matches no
    // participant, so a moderator lowering its OWN login-stamped hand works
    // without letting it lower arbitrary uris by accident.
    Recorder modLower;
    runOrder(modLower, true, "74112", canonicalV1("m12", "", R"({"raiseHand": false})"));
    expect_true(modLower.has("raiseHand:m12/:0@74112"),
                "V1: moderator lower must keep the stamped target with the sender attached");
}

void
test_v1_non_moderator_media_actions_rejected()
{
    Recorder rec;
    runOrder(rec,
             false,
             "mallory",
             canonicalV1("bob",
                         "dev1",
                         R"({"hangup": true,
                             "medias": {"stream7": {"muteAudio": true,
                                                    "active": true,
                                                    "voiceActivity": true}}})"));
    expect_true(!rec.has("hangup:bob/dev1"), "V1: non-moderator hangup must be rejected");
    expect_true(!rec.has("muteAudio:bob/dev1/stream7:1"),
                "V1: non-moderator muteAudio must be rejected");
    expect_true(!rec.has("active:stream7:1"), "V1: non-moderator active must be rejected");
    expect_true(rec.has("voice:stream7:1"),
                "V1: voiceActivity is not moderator-gated and must dispatch");
}

void
test_v1_mute_video_without_handler_does_not_crash()
{
    // D3 regression: muteVideo order with the (unimplemented) video handler
    // unregistered used to call an empty std::function.
    Recorder rec;
    runOrder(rec,
             true,
             "alice",
             canonicalV1("bob",
                         "dev1",
                         R"({"medias": {"stream7": {"muteVideo": true}}})"));
    expect_true(rec.count() == 1 && rec.has("version:1"),
                "D3 regression: muteVideo with no handler must dispatch nothing");
}

void
test_v1_version_scalar_does_not_throw()
{
    // The root loop must skip scalar members ("version", non-moderator
    // "layout") instead of calling isMember() on them (Json::LogicError).
    Recorder rec;
    runOrder(rec, false, "mallory", R"({"version": 1, "layout": 2})");
    expect_true(!rec.has("layout:2"), "V1: non-moderator layout must be rejected");
}

void
test_v1_moderator_layout()
{
    Recorder rec;
    runOrder(rec, true, "alice", R"({"version": 1, "layout": 2})");
    expect_true(rec.has("layout:2"), "V1: moderator layout not dispatched");
}

void
test_v1_minimal_handler_set()
{
    // D1/D12 regression: only checkAuthorization + raiseHand wired.
    Recorder rec;
    ConfProtocolParser parser;
    parser.onCheckAuthorization([](std::string_view) { return false; });
    parser.onRaiseHand([&rec](const std::string& sender,
                              const std::string& uri,
                              const std::string& dev,
                              bool state) {
        rec.add("raiseHand:" + uri + "/" + dev + ":" + (state ? "1" : "0") + "@" + sender);
    });
    parser.initData(parseJson(canonicalV1("bob", "dev1", R"({"raiseHand": true})")), "bob");
    parser.parse();
    expect_true(rec.has("raiseHand:bob/dev1:1@bob"),
                "D1 regression: raiseHand must dispatch with minimal handler set");
}

// ---- canonical V1 builders (ConfOrder) ----

void
test_builders_round_trip_through_parser()
{
    // D4 regression: each outgoing builder must produce the canonical shape
    // that our own (fixed) parser dispatches.
    Recorder raise;
    runBuiltOrder(raise, false, "bob", sip_core::ConfOrder::raiseHand("bob", "dev1", true));
    expect_true(raise.has("raiseHand:bob/dev1:1@bob"), "builder: raiseHand round-trip failed");

    Recorder hangup;
    runBuiltOrder(hangup, true, "alice", sip_core::ConfOrder::hangupParticipant("bob", "dev1"));
    expect_true(hangup.has("hangup:bob/dev1"), "builder: hangup round-trip failed");

    Recorder mute;
    runBuiltOrder(mute,
                  true,
                  "alice",
                  sip_core::ConfOrder::muteAudio("bob", "dev1", "stream7", true));
    expect_true(mute.has("muteAudio:bob/dev1/stream7:1"),
                "builder: muteAudio round-trip failed");

    Recorder active;
    runBuiltOrder(active,
                  true,
                  "alice",
                  sip_core::ConfOrder::setActiveStream("bob", "dev1", "stream7", true));
    expect_true(active.has("active:stream7:1"), "builder: setActiveStream round-trip failed");
}

void
test_builders_canonical_shape()
{
    // No self-nesting: the account object must contain ONLY "devices", and
    // the device object only the action (+"medias").
    auto order = sip_core::ConfOrder::muteAudio("bob", "dev1", "stream7", true);
    expect_true(order["version"].asInt() == 1, "builder: version member missing");
    expect_true(order["bob"].isObject() && order["bob"].size() == 1
                    && order["bob"].isMember("devices"),
                "builder: account object must contain only 'devices'");
    const auto& dev = order["bob"]["devices"]["dev1"];
    expect_true(dev.isObject() && dev.size() == 1 && dev.isMember("medias"),
                "builder: device object must contain only 'medias'");
    expect_true(dev["medias"]["stream7"]["muteAudio"].asBool(),
                "builder: muteAudio flag missing");
}

void
test_unsupported_version_ignored()
{
    Recorder rec;
    runOrder(rec, true, "alice", R"({"version": 2, "layout": 1})");
    expect_true(!rec.has("layout:1"), "unsupported protocol version must be ignored");
}

void
test_peer_identity_normalization()
{
    // Live-observed on the staging PBX: Call::getPeerNumber() is a full
    // bracketed URI. Every Conference identity comparison normalizes with
    // sip_utils::stripSipUriPrefix — pin its behavior for the exact shapes
    // confInfo publishes (specs/conference-actions.md, identity model).
    expect_true(sip_core::sip_utils::stripSipUriPrefix("<sip:009@tele.svetets.ru>") == "009",
                "bracketed sip uri must normalize to the user part");
    expect_true(sip_core::sip_utils::stripSipUriPrefix("sip:009@tele.svetets.ru") == "009",
                "unbracketed sip uri must normalize to the user part");
    expect_true(sip_core::sip_utils::stripSipUriPrefix("009@tele.svetets.ru") == "009",
                "user@domain must normalize to the user part");
    expect_true(sip_core::sip_utils::stripSipUriPrefix("009") == "009",
                "bare user part must pass through unchanged");
}

} // namespace

int
main()
{
    test_v0_moderator_full_dispatch();
    test_v0_non_moderator_rejected();
    test_v0_moderator_lowers_other_hand_but_cannot_raise();
    test_v0_login_stamped_self_action();
    test_v0_missing_optional_handler_keeps_other_dispatches();
    test_v1_canonical_moderator_dispatch();
    test_v1_legacy_self_nested_shape_still_dispatches();
    test_v1_raise_hand_self_authorization();
    test_v1_login_stamped_self_action();
    test_v1_non_moderator_media_actions_rejected();
    test_v1_mute_video_without_handler_does_not_crash();
    test_v1_version_scalar_does_not_throw();
    test_v1_moderator_layout();
    test_v1_minimal_handler_set();
    test_unsupported_version_ignored();
    test_peer_identity_normalization();
    test_builders_round_trip_through_parser();
    test_builders_canonical_shape();
    std::cout << "conference_protocol_tests: all tests passed\n";
    return 0;
}

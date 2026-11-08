// Host-side unit test for DynamiteSyncImpl/MessageValidation.h. No game, no Windows.
// Build and run: hook/tests/run_tests.sh
//
// Each case builds a packet the way a partner could send it, runs the same two checks as
// DynamiteSyncImpl::RecvRaw (flatbuffers Verifier, then ValidateMessage) and checks the verdict.
// "verifier-only" counts packets the Verifier alone would have passed to a handler.

#include "DynamiteSyncImpl/MessageValidation.h"

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

using namespace DynamiteMessage;
using flatbuffers::FlatBufferBuilder;

#include "Tpp/BossQuietActionCommand.h"
#include "Tpp/BossQuietActionTask.h"

// The same sizes RecvRaw passes (both 176 bytes with the current headers).
static constexpr Dynamite::MessageLimits kLimits{
    .bossQuietActionCommandSize = sizeof(BossQuietActionCommand),
    .bossQuietActionTaskSize = sizeof(BossQuietActionTask),
};

static int failures = 0;
static int verifierOnlyWouldPass = 0;

static void Check(const char *name, bool wantAccepted, const std::function<void(FlatBufferBuilder &)> &build) {
    FlatBufferBuilder b(256);
    build(b);
    flatbuffers::Verifier v(b.GetBufferPointer(), b.GetSize());
    const bool verified = VerifyMessageWrapperBuffer(v);
    const char *reason = verified ? Dynamite::ValidateMessage(GetMessageWrapper(b.GetBufferPointer()), kLimits) : "verifier";
    const bool accepted = verified && reason == nullptr;
    if (verified && !wantAccepted) {
        verifierOnlyWouldPass++;
    }
    const bool ok = accepted == wantAccepted;
    if (!ok) {
        failures++;
    }
    std::printf("%s %-58s want=%-6s got=%-6s %s\n", ok ? "PASS" : "FAIL", name, wantAccepted ? "accept" : "reject",
        accepted ? "accept" : "reject", reason ? reason : "");
}

template <typename T> static void Finish(FlatBufferBuilder &b, Message type, flatbuffers::Offset<T> body) {
    b.Finish(CreateMessageWrapper(b, 1, type, body.Union()));
}

static std::vector<int8_t> Bytes8(size_t n) { return std::vector<int8_t>(n, 1); }
static std::vector<uint8_t> UBytes(size_t n) { return std::vector<uint8_t>(n, 1); }

int main() {
    const Vec3 pos(1, 2, 3);

    // --- well-formed packets, as the mod itself sends them ---
    Check("Ping", true, [](auto &b) { Finish(b, Message_Ping, CreatePing(b, 7)); });
    Check("AddFixedUserMarker", true, [&](auto &b) { Finish(b, Message_AddFixedUserMarker, CreateAddFixedUserMarker(b, &pos)); });
    Check("AddFollowUserMarker", true, [&](auto &b) { Finish(b, Message_AddFollowUserMarker, CreateAddFollowUserMarker(b, &pos, 5)); });
    Check("RemoveUserMarker", true, [](auto &b) { Finish(b, Message_RemoveUserMarker, CreateRemoveUserMarker(b, 3)); });
    Check("SetSightMarker", true, [](auto &b) { Finish(b, Message_SetSightMarker, CreateSetSightMarker(b, 3, 10)); });
    Check("RequestVar", true, [](auto &b) { Finish(b, Message_RequestVar, CreateRequestVarDirect(b, "svars", "foo")); });
    Check("SyncVar Int32", true, [](auto &b) {
        std::vector<int32_t> vals{1, 2, 3};
        auto av = CreateInt32Direct(b, &vals);
        Finish(b, Message_SyncVar, CreateSyncVarDirect(b, "svars", "foo", 3, 0, ArrayValues_Int32, av.Union()));
    });
    Check("SyncVar Bool", true, [](auto &b) {
        std::vector<uint8_t> vals{1};
        auto av = CreateBoolDirect(b, &vals);
        Finish(b, Message_SyncVar, CreateSyncVarDirect(b, "gvars", "bar", 1, 4, ArrayValues_Bool, av.Union()));
    });
    Check("SendEmblem", true, [](auto &b) {
        EmblemInfo e{};
        Finish(b, Message_SendEmblem, CreateSendEmblem(b, &e));
    });
    Check("SendBossQuietActionCommand 176 bytes", true, [](auto &b) {
        auto v = Bytes8(176);
        Finish(b, Message_SendBossQuietActionCommand, CreateSendBossQuietActionCommandDirect(b, 1, &v));
    });
    Check("SendBossQuietExtraActionCommand 176 bytes", true, [](auto &b) {
        auto v = UBytes(176);
        Finish(b, Message_SendBossQuietExtraActionCommand, CreateSendBossQuietExtraActionCommandDirect(b, 1, &v));
    });
    Check("SendBossQuietSetNextActionTask full task", true, [](auto &b) {
        auto v = UBytes(kLimits.bossQuietActionTaskSize);
        Finish(b, Message_SendBossQuietSetNextActionTask, CreateSendBossQuietSetNextActionTaskDirect(b, 1, &v));
    });
    Check("SendBossQuietSetActionTask full task", true, [](auto &b) {
        auto v = UBytes(kLimits.bossQuietActionTaskSize);
        Finish(b, Message_SendBossQuietSetActionTask, CreateSendBossQuietSetActionTaskDirect(b, 1, &v));
    });
    Check("SendBossQuietDamage", true, [](auto &b) { Finish(b, Message_SendBossQuietDamage, CreateSendBossQuietDamage(b, 1, 2, 3, 4, 5)); });
    Check("BodyPosition", true, [&](auto &b) { Finish(b, Message_BodyPosition, CreateBodyPosition(b, 9, &pos, 0.5f)); });

    // --- malformed packets a handler would have crashed on or read past the end of ---
    Check("type NONE", false, [](auto &b) { b.Finish(CreateMessageWrapper(b, 1, Message_NONE, 0)); });
    Check("Ping type, body missing", false, [](auto &b) { b.Finish(CreateMessageWrapper(b, 1, Message_Ping, 0)); });
    Check("SyncVar type, body missing", false, [](auto &b) { b.Finish(CreateMessageWrapper(b, 1, Message_SyncVar, 0)); });
    Check("AddFixedUserMarker without pos", false, [](auto &b) { Finish(b, Message_AddFixedUserMarker, CreateAddFixedUserMarker(b, nullptr)); });
    Check("AddFollowUserMarker without pos", false, [](auto &b) { Finish(b, Message_AddFollowUserMarker, CreateAddFollowUserMarker(b, nullptr, 5)); });
    Check("BodyPosition without pos", false, [](auto &b) { Finish(b, Message_BodyPosition, CreateBodyPosition(b, 9, nullptr)); });
    Check("RequestVar without name", false, [](auto &b) { Finish(b, Message_RequestVar, CreateRequestVarDirect(b, "svars", nullptr)); });
    Check("RequestVar without category", false, [](auto &b) { Finish(b, Message_RequestVar, CreateRequestVarDirect(b, nullptr, "foo")); });
    Check("SyncVar without name", false, [](auto &b) {
        std::vector<int32_t> vals{1};
        auto av = CreateInt32Direct(b, &vals);
        Finish(b, Message_SyncVar, CreateSyncVarDirect(b, "svars", nullptr, 1, 0, ArrayValues_Int32, av.Union()));
    });
    Check("SyncVar without values", false, [](auto &b) { Finish(b, Message_SyncVar, CreateSyncVarDirect(b, "svars", "foo", 1, 0, ArrayValues_NONE, 0)); });
    Check("SyncVar Float table without vector", false, [](auto &b) {
        auto av = CreateFloat(b, 0);
        Finish(b, Message_SyncVar, CreateSyncVarDirect(b, "svars", "foo", 1, 0, ArrayValues_Float, av.Union()));
    });
    Check("SyncVar array_start + size wraps uint32", false, [](auto &b) {
        std::vector<int32_t> vals{1, 2};
        auto av = CreateInt32Direct(b, &vals);
        Finish(b, Message_SyncVar, CreateSyncVarDirect(b, "svars", "foo", 2, UINT32_MAX, ArrayValues_Int32, av.Union()));
    });
    Check("SendEmblem without emblem_info", false, [](auto &b) { Finish(b, Message_SendEmblem, CreateSendEmblem(b, nullptr)); });
    Check("SendBossQuietActionCommand without vector", false, [](auto &b) {
        Finish(b, Message_SendBossQuietActionCommand, CreateSendBossQuietActionCommand(b, 1, 0));
    });
    Check("SendBossQuietActionCommand 4 bytes (memcpy reads 176)", false, [](auto &b) {
        auto v = Bytes8(4);
        Finish(b, Message_SendBossQuietActionCommand, CreateSendBossQuietActionCommandDirect(b, 1, &v));
    });
    Check("SendBossQuietExtraActionCommand 175 bytes", false, [](auto &b) {
        auto v = UBytes(175);
        Finish(b, Message_SendBossQuietExtraActionCommand, CreateSendBossQuietExtraActionCommandDirect(b, 1, &v));
    });
    Check("SendBossQuietSetNextActionTask empty", false, [](auto &b) {
        auto v = UBytes(0);
        Finish(b, Message_SendBossQuietSetNextActionTask, CreateSendBossQuietSetNextActionTaskDirect(b, 1, &v));
    });
    Check("SendBossQuietSetActionTask without vector", false, [](auto &b) {
        Finish(b, Message_SendBossQuietSetActionTask, CreateSendBossQuietSetActionTask(b, 1, 0));
    });
    Check("unknown union type 200", false, [](auto &b) {
        auto p = CreatePing(b, 1);
        b.Finish(CreateMessageWrapper(b, 1, static_cast<Message>(200), p.Union()));
    });

    std::printf("\nmalformed packets the Verifier alone would pass to a handler: %d\n", verifierOnlyWouldPass);
    std::printf("%s: %d failure(s)\n", failures == 0 ? "OK" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}

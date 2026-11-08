#ifndef HOOK_DYNAMITE_MESSAGEVALIDATION_H
#define HOOK_DYNAMITE_MESSAGEVALIDATION_H

// Checks a received DynamiteMessage before any handler touches it.
//
// flatbuffers::Verifier only proves that offsets stay inside the buffer. It accepts tables with
// missing fields (a null union value, a null string, a null vector) and vectors of any length.
// The handlers dereference those fields and memcpy fixed-size game structs out of the vectors,
// so a short or partial packet from the partner reads out of bounds or crashes the game.
//
// Kept free of game types so it can be unit-tested on any host (see tests/).

#include "DynamiteSyncSchema_generated.h"

#include <cstddef>
#include <cstdint>

namespace Dynamite {

    struct MessageLimits {
        size_t bossQuietActionCommandSize; // sizeof(BossQuietActionCommand)
        size_t bossQuietActionTaskSize;    // sizeof(BossQuietActionTask)
    };

    namespace detail {
        template <typename T> inline bool HasValue(const T *table) { return table != nullptr && table->value() != nullptr; }

        inline const char *ValidateSyncVarValues(const DynamiteMessage::SyncVar *m) {
            using namespace DynamiteMessage;
            uint32_t count = 0;
            switch (m->array_values_type()) {
            case ArrayValues_Bool:
                if (!HasValue(m->array_values_as_Bool())) return "SyncVar: Bool values missing";
                count = m->array_values_as_Bool()->value()->size();
                break;
            case ArrayValues_Int32:
                if (!HasValue(m->array_values_as_Int32())) return "SyncVar: Int32 values missing";
                count = m->array_values_as_Int32()->value()->size();
                break;
            case ArrayValues_Uint32:
                if (!HasValue(m->array_values_as_Uint32())) return "SyncVar: Uint32 values missing";
                count = m->array_values_as_Uint32()->value()->size();
                break;
            case ArrayValues_Float:
                if (!HasValue(m->array_values_as_Float())) return "SyncVar: Float values missing";
                count = m->array_values_as_Float()->value()->size();
                break;
            case ArrayValues_Int8:
                if (!HasValue(m->array_values_as_Int8())) return "SyncVar: Int8 values missing";
                count = m->array_values_as_Int8()->value()->size();
                break;
            case ArrayValues_Uint8:
                if (!HasValue(m->array_values_as_Uint8())) return "SyncVar: Uint8 values missing";
                count = m->array_values_as_Uint8()->value()->size();
                break;
            case ArrayValues_Int16:
                if (!HasValue(m->array_values_as_Int16())) return "SyncVar: Int16 values missing";
                count = m->array_values_as_Int16()->value()->size();
                break;
            case ArrayValues_Uint16:
                if (!HasValue(m->array_values_as_Uint16())) return "SyncVar: Uint16 values missing";
                count = m->array_values_as_Uint16()->value()->size();
                break;
            default:
                return "SyncVar: unknown array values type";
            }

            // array_start + i is used as the element index when writing the game variable. The game's
            // own array length is not known here (unknown), but the index must at least not wrap.
            if (count > 0 && static_cast<uint64_t>(m->array_start()) + count - 1 > UINT32_MAX) {
                return "SyncVar: array_start + size overflows";
            }

            return nullptr;
        }

        inline bool HasBytes(const flatbuffers::Vector<int8_t> *v, size_t need) { return v != nullptr && v->size() >= need; }
        inline bool HasBytes(const flatbuffers::Vector<uint8_t> *v, size_t need) { return v != nullptr && v->size() >= need; }
    } // namespace detail

    // Returns nullptr when the message is safe to hand to its handler, otherwise a short reason.
    inline const char *ValidateMessage(const DynamiteMessage::MessageWrapper *w, const MessageLimits &limits) {
        using namespace DynamiteMessage;

        if (w == nullptr) {
            return "no message wrapper";
        }

        if (w->msg_type() == Message_NONE) {
            return "message type NONE";
        }

        if (w->msg() == nullptr) {
            return "message body missing";
        }

        switch (w->msg_type()) {
        case Message_Ping:
        case Message_RemoveUserMarker:
        case Message_SetSightMarker:
        case Message_SendBossQuietDamage:
            return nullptr; // scalars only

        case Message_AddFixedUserMarker:
            return w->msg_as_AddFixedUserMarker()->pos() == nullptr ? "AddFixedUserMarker: pos missing" : nullptr;

        case Message_AddFollowUserMarker:
            return w->msg_as_AddFollowUserMarker()->pos() == nullptr ? "AddFollowUserMarker: pos missing" : nullptr;

        case Message_BodyPosition:
            return w->msg_as_BodyPosition()->pos() == nullptr ? "BodyPosition: pos missing" : nullptr;

        case Message_RequestVar: {
            const auto m = w->msg_as_RequestVar();
            if (m->category() == nullptr || m->name() == nullptr) {
                return "RequestVar: category or name missing";
            }
            return nullptr;
        }

        case Message_SyncVar: {
            const auto m = w->msg_as_SyncVar();
            if (m->category() == nullptr || m->name() == nullptr) {
                return "SyncVar: category or name missing";
            }
            return detail::ValidateSyncVarValues(m);
        }

        case Message_SendEmblem:
            return w->msg_as_SendEmblem()->emblem_info() == nullptr ? "SendEmblem: emblem_info missing" : nullptr;

        case Message_SendBossQuietActionCommand:
            return detail::HasBytes(w->msg_as_SendBossQuietActionCommand()->action_command(), limits.bossQuietActionCommandSize)
                       ? nullptr
                       : "SendBossQuietActionCommand: action_command missing or too short";

        case Message_SendBossQuietExtraActionCommand:
            return detail::HasBytes(w->msg_as_SendBossQuietExtraActionCommand()->action_command(), limits.bossQuietActionCommandSize)
                       ? nullptr
                       : "SendBossQuietExtraActionCommand: action_command missing or too short";

        case Message_SendBossQuietSetNextActionTask:
            return detail::HasBytes(w->msg_as_SendBossQuietSetNextActionTask()->action_task(), limits.bossQuietActionTaskSize)
                       ? nullptr
                       : "SendBossQuietSetNextActionTask: action_task missing or too short";

        case Message_SendBossQuietSetActionTask:
            return detail::HasBytes(w->msg_as_SendBossQuietSetActionTask()->task(), limits.bossQuietActionTaskSize)
                       ? nullptr
                       : "SendBossQuietSetActionTask: task missing or too short";

        default:
            return "unknown message type";
        }
    }

} // namespace Dynamite

#endif // HOOK_DYNAMITE_MESSAGEVALIDATION_H

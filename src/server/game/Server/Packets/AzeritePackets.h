/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef TRINITYCORE_AZERITE_ITEM_PACKETS_H
#define TRINITYCORE_AZERITE_ITEM_PACKETS_H

#include "Packet.h"
#include "ItemDefines.h"
#include "ObjectGuid.h"
#include "Optional.h"

namespace WorldPackets
{
    namespace Azerite
    {
        class PlayerAzeriteItemGains final : public ServerPacket
        {
        public:
            explicit PlayerAzeriteItemGains() : ServerPacket(SMSG_PLAYER_AZERITE_ITEM_GAINS, 16 + 8) { }

            WorldPacket const* Write() override;

            ObjectGuid ItemGUID;
            uint64 XP = 0;
        };

        class AzeriteEssenceUnlockMilestone final : public ClientPacket
        {
        public:
            explicit AzeriteEssenceUnlockMilestone(WorldPacket&& packet) : ClientPacket(CMSG_AZERITE_ESSENCE_UNLOCK_MILESTONE, std::move(packet)) { }

            void Read() override;

            int32 AzeriteItemMilestonePowerID = 0;
        };

        class AzeriteEssenceActivateEssence final : public ClientPacket
        {
        public:
            explicit AzeriteEssenceActivateEssence(WorldPacket&& packet) : ClientPacket(CMSG_AZERITE_ESSENCE_ACTIVATE_ESSENCE, std::move(packet)) { }

            void Read() override;

            int32 AzeriteEssenceID = 0;
            uint8 Slot = 0;
        };

        class ActivateEssenceFailed final : public ServerPacket
        {
        public:
            explicit ActivateEssenceFailed() : ServerPacket(SMSG_ACTIVATE_ESSENCE_FAILED, 1 + 4 + 4 + 1) { }

            WorldPacket const* Write() override;

            AzeriteEssenceActivateResult Reason = AzeriteEssenceActivateResult::None;
            int32 Arg = 0;
            int32 AzeriteEssenceID = 0;
            Optional<uint8> Slot;
        };

        class AzeriteEmpoweredItemViewed final : public ClientPacket
        {
        public:
            explicit AzeriteEmpoweredItemViewed(WorldPacket&& packet) : ClientPacket(CMSG_AZERITE_EMPOWERED_ITEM_VIEWED, std::move(packet)) { }

            void Read() override;

            ObjectGuid ItemGUID;
        };

        class AzeriteEmpoweredItemSelectPower final : public ClientPacket
        {
        public:
            explicit AzeriteEmpoweredItemSelectPower(WorldPacket&& packet) : ClientPacket(CMSG_AZERITE_EMPOWERED_ITEM_SELECT_POWER, std::move(packet)) { }

            void Read() override;

            uint8 Tier = 0;
            int32 AzeritePowerID = 0;
            uint8 ContainerSlot = 0;
            uint8 Slot = 0;
        };

        class TC_GAME_API PlayerAzeriteItemEquippedStatusChanged final : public ServerPacket
        {
        public:
            explicit PlayerAzeriteItemEquippedStatusChanged() : ServerPacket(SMSG_PLAYER_AZERITE_ITEM_EQUIPPED_STATUS_CHANGED, 1) { }

            WorldPacket const* Write() override;

            bool IsHeartEquipped = false;
        };
    }
}

#endif // TRINITYCORE_AZERITE_ITEM_PACKETS_H

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

#include "CharacterCache.h"
#include "DB2Stores.h"
#include "WorldSession.h"
#include "GameTime.h"
#include "Group.h"
#include "LFGMgr.h"
#include "LFGPackets.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"

void WorldSession::HandleLfgJoinOpcode(WorldPackets::LFG::DFJoin& dfJoin)
{
    if (!sLFGMgr->isOptionEnabled(lfg::LFG_OPTION_ENABLE_DUNGEON_FINDER | lfg::LFG_OPTION_ENABLE_RAID_BROWSER) ||
        (GetPlayer()->GetGroup() && GetPlayer()->GetGroup()->GetLeaderGUID() != GetPlayer()->GetGUID() &&
        (GetPlayer()->GetGroup()->GetMembersCount() == MAX_GROUP_SIZE || !GetPlayer()->GetGroup()->isLFGGroup())))
        return;

    if (dfJoin.Slots.empty())
    {
        TC_LOG_DEBUG("lfg", "CMSG_DF_JOIN {} no dungeons selected", GetPlayerInfo());
        return;
    }

    lfg::LfgDungeonSet newDungeons;
    for (uint32 slot : dfJoin.Slots)
    {
        uint32 dungeon = slot & 0x00FFFFFF;
        if (sLFGDungeonsStore.LookupEntry(dungeon))
            newDungeons.insert(dungeon);
    }

    TC_LOG_DEBUG("lfg", "CMSG_DF_JOIN {} roles: {}, Dungeons: {}", GetPlayerInfo(), dfJoin.Roles, uint8(newDungeons.size()));

    sLFGMgr->JoinLfg(GetPlayer(), uint8(dfJoin.Roles), newDungeons);
}

void WorldSession::HandleLfgLeaveOpcode(WorldPackets::LFG::DFLeave& dfLeave)
{
    Group* group = GetPlayer()->GetGroup();

    TC_LOG_DEBUG("lfg", "CMSG_DF_LEAVE {} in group: {} sent guid {}.",
        GetPlayerInfo(), group ? 1 : 0, dfLeave.Ticket.RequesterGuid.ToString());

    // Check cheating - only leader can leave the queue
    if (!group || group->GetLeaderGUID() == dfLeave.Ticket.RequesterGuid)
        sLFGMgr->LeaveLfg(dfLeave.Ticket.RequesterGuid);
}

void WorldSession::HandleLfgProposalResultOpcode(WorldPackets::LFG::DFProposalResponse& dfProposalResponse)
{
    TC_LOG_DEBUG("lfg", "CMSG_LFG_PROPOSAL_RESULT {} proposal: {} accept: {}",
        GetPlayerInfo(), dfProposalResponse.ProposalID, dfProposalResponse.Accepted ? 1 : 0);
    sLFGMgr->UpdateProposal(dfProposalResponse.ProposalID, GetPlayer()->GetGUID(), dfProposalResponse.Accepted);
}

void WorldSession::HandleLfgSetRolesOpcode(WorldPackets::LFG::DFSetRoles& dfSetRoles)
{
    ObjectGuid guid = GetPlayer()->GetGUID();
    Group* group = GetPlayer()->GetGroup();
    if (!group)
    {
        TC_LOG_DEBUG("lfg", "CMSG_DF_SET_ROLES {} Not in group",
            GetPlayerInfo());
        return;
    }
    ObjectGuid gguid = group->GetGUID();
    TC_LOG_DEBUG("lfg", "CMSG_DF_SET_ROLES: Group {}, Player {}, Roles: {}",
        gguid.ToString(), GetPlayerInfo(), dfSetRoles.RolesDesired);
    sLFGMgr->UpdateRoleCheck(gguid, guid, dfSetRoles.RolesDesired);
}

void WorldSession::HandleLfgSetBootVoteOpcode(WorldPackets::LFG::DFBootPlayerVote& dfBootPlayerVote)
{
    ObjectGuid guid = GetPlayer()->GetGUID();
    TC_LOG_DEBUG("lfg", "CMSG_LFG_SET_BOOT_VOTE {} agree: {}",
        GetPlayerInfo(), dfBootPlayerVote.Vote ? 1 : 0);
    sLFGMgr->UpdateBoot(guid, dfBootPlayerVote.Vote);
}

void WorldSession::HandleLfgTeleportOpcode(WorldPackets::LFG::DFTeleport& dfTeleport)
{
    TC_LOG_DEBUG("lfg", "CMSG_DF_TELEPORT {} out: {}",
        GetPlayerInfo(), dfTeleport.TeleportOut ? 1 : 0);
    sLFGMgr->TeleportPlayer(GetPlayer(), dfTeleport.TeleportOut, true);
}

void WorldSession::HandleDFGetSystemInfo(WorldPackets::LFG::DFGetSystemInfo& dfGetSystemInfo)
{
    TC_LOG_DEBUG("lfg", "CMSG_DF_GET_SYSTEM_INFO {} for {}", GetPlayerInfo(), (dfGetSystemInfo.Player ? "player" : "party"));

    if (dfGetSystemInfo.Player)
        SendLfgPlayerLockInfo();
    else
        SendLfgPartyLockInfo();
}

void WorldSession::HandleDFGetJoinStatus(WorldPackets::LFG::DFGetJoinStatus& /*dfGetJoinStatus*/)
{
    TC_LOG_DEBUG("lfg", "CMSG_DF_GET_JOIN_STATUS {}", GetPlayerInfo());

    if (!GetPlayer()->isUsingLfg())
        return;

    ObjectGuid guid = GetPlayer()->GetGUID();
    lfg::LfgUpdateData updateData = sLFGMgr->GetLfgStatus(guid);

    if (GetPlayer()->GetGroup())
    {
        SendLfgUpdateStatus(updateData, true);
        updateData.dungeons.clear();
        SendLfgUpdateStatus(updateData, false);
    }
    else
    {
        SendLfgUpdateStatus(updateData, false);
        updateData.dungeons.clear();
        SendLfgUpdateStatus(updateData, true);
    }
}

void WorldSession::SendLfgPlayerLockInfo()
{
    TC_LOG_DEBUG("lfg", "SMSG_LFG_PLAYER_INFO {}", GetPlayerInfo());

    // Get Random dungeons that can be done at a certain level and expansion
    uint8 level = GetPlayer()->GetLevel();
    uint32 contentTuningReplacementConditionMask = GetPlayer()->m_playerData->CtrOptions->ConditionalFlags;
    lfg::LfgDungeonSet const& randomDungeons = sLFGMgr->GetRandomAndSeasonalDungeons(level, GetExpansion(), contentTuningReplacementConditionMask);

    WorldPackets::LFG::LfgPlayerInfo lfgPlayerInfo;

    // Get player locked Dungeons
    for (auto const& lock : sLFGMgr->GetLockedDungeons(_player->GetGUID()))
        lfgPlayerInfo.BlackList.Slot.emplace_back(lock.first, lock.second.lockStatus, lock.second.requiredItemLevel, lock.second.currentItemLevel, 0);

    for (uint32 slot : randomDungeons)
    {
        lfgPlayerInfo.Dungeon.emplace_back();
        WorldPackets::LFG::LfgPlayerDungeonInfo& playerDungeonInfo = lfgPlayerInfo.Dungeon.back();
        playerDungeonInfo.Slot = slot;
        playerDungeonInfo.CompletionQuantity = 1;
        playerDungeonInfo.CompletionLimit = 1;
        playerDungeonInfo.CompletionCurrencyID = 0;
        playerDungeonInfo.SpecificQuantity = 0;
        playerDungeonInfo.SpecificLimit = 1;
        playerDungeonInfo.OverallQuantity = 0;
        playerDungeonInfo.OverallLimit = 1;
        playerDungeonInfo.PurseWeeklyQuantity = 0;
        playerDungeonInfo.PurseWeeklyLimit = 0;
        playerDungeonInfo.PurseQuantity = 0;
        playerDungeonInfo.PurseLimit = 0;
        playerDungeonInfo.Quantity = 1;
        playerDungeonInfo.CompletedMask = 0;
        playerDungeonInfo.EncounterMask = 0;

        if (lfg::LfgReward const* reward = sLFGMgr->GetRandomDungeonReward(slot, level))
        {
            if (Quest const* quest = sObjectMgr->GetQuestTemplate(reward->firstQuest))
            {
                playerDungeonInfo.FirstReward = !GetPlayer()->CanRewardQuest(quest, false);
                if (!playerDungeonInfo.FirstReward)
                    quest = sObjectMgr->GetQuestTemplate(reward->otherQuest);

                if (quest)
                {
                    playerDungeonInfo.Rewards.RewardMoney = _player->GetQuestMoneyReward(quest);
                    playerDungeonInfo.Rewards.RewardXP = _player->GetQuestXPReward(quest);
                    for (uint8 i = 0; i < QUEST_REWARD_ITEM_COUNT; ++i)
                        if (uint32 itemId = quest->RewardItemId[i])
                            playerDungeonInfo.Rewards.Item.emplace_back(itemId, quest->RewardItemCount[i]);

                    for (uint32 i = 0; i < QUEST_REWARD_CURRENCY_COUNT; ++i)
                        if (uint32 curencyId = quest->RewardCurrencyId[i])
                            playerDungeonInfo.Rewards.Currency.emplace_back(curencyId, quest->RewardCurrencyCount[i]);
                }
            }
        }
    }

    SendPacket(lfgPlayerInfo.Write());;
}

void WorldSession::SendLfgPartyLockInfo()
{
    ObjectGuid guid = GetPlayer()->GetGUID();
    Group* group = GetPlayer()->GetGroup();
    if (!group)
        return;

    WorldPackets::LFG::LfgPartyInfo lfgPartyInfo;

    // Get the locked dungeons of the other party members
    for (GroupReference const& itr : group->GetMembers())
    {
        Player* plrg = itr.GetSource();
        ObjectGuid pguid = plrg->GetGUID();
        if (pguid == guid)
            continue;

        lfgPartyInfo.Player.emplace_back();
        WorldPackets::LFG::LFGBlackList& lfgBlackList = lfgPartyInfo.Player.back();
        lfgBlackList.PlayerGuid = pguid;
        for (auto const& lock : sLFGMgr->GetLockedDungeons(pguid))
            lfgBlackList.Slot.emplace_back(lock.first, lock.second.lockStatus, lock.second.requiredItemLevel, lock.second.currentItemLevel, 0);
    }

    TC_LOG_DEBUG("lfg", "SMSG_LFG_PARTY_INFO {}", GetPlayerInfo());
    SendPacket(lfgPartyInfo.Write());;
}

void WorldSession::SendLfgUpdateStatus(lfg::LfgUpdateData const& updateData, bool party)
{
    bool join = false;
    bool queued = false;

    switch (updateData.updateType)
    {
        case lfg::LFG_UPDATETYPE_JOIN_QUEUE_INITIAL:            // Joined queue outside the dungeon
            join = true;
            break;
        case lfg::LFG_UPDATETYPE_JOIN_QUEUE:
        case lfg::LFG_UPDATETYPE_ADDED_TO_QUEUE:                // Rolecheck Success
            join = true;
            queued = true;
            break;
        case lfg::LFG_UPDATETYPE_PROPOSAL_BEGIN:
            join = true;
            break;
        case lfg::LFG_UPDATETYPE_UPDATE_STATUS:
            join = updateData.state != lfg::LFG_STATE_ROLECHECK && updateData.state != lfg::LFG_STATE_NONE;
            queued = updateData.state == lfg::LFG_STATE_QUEUED;
            break;
        default:
            break;
    }

    TC_LOG_DEBUG("lfg", "SMSG_LFG_UPDATE_STATUS {} updatetype: {}, party {}",
        GetPlayerInfo(), updateData.updateType, party ? "true" : "false");

    WorldPackets::LFG::LFGUpdateStatus lfgUpdateStatus;
    if (WorldPackets::LFG::RideTicket const* ticket = sLFGMgr->GetTicket(_player->GetGUID()))
        lfgUpdateStatus.Ticket = *ticket;

    lfgUpdateStatus.SubType = lfg::LFG_QUEUE_DUNGEON; // other types not implemented
    lfgUpdateStatus.Reason = updateData.updateType;
    std::transform(updateData.dungeons.begin(), updateData.dungeons.end(), std::back_inserter(lfgUpdateStatus.Slots), [](uint32 dungeonId)
    {
        return sLFGMgr->GetLFGDungeonEntry(dungeonId);
    });
    lfgUpdateStatus.RequestedRoles = sLFGMgr->GetRoles(_player->GetGUID());
    //lfgUpdateStatus.SuspendedPlayers;
    lfgUpdateStatus.IsParty = party;
    lfgUpdateStatus.NotifyUI = true;
    lfgUpdateStatus.Joined = join;
    lfgUpdateStatus.LfgJoined = updateData.updateType != lfg::LFG_UPDATETYPE_REMOVED_FROM_QUEUE;
    lfgUpdateStatus.Queued = queued;
    lfgUpdateStatus.QueueMapID = sLFGMgr->GetDungeonMapId(_player->GetGUID());

    SendPacket(lfgUpdateStatus.Write());
}

void WorldSession::SendLfgRoleChosen(ObjectGuid guid, uint8 roles)
{
    TC_LOG_DEBUG("lfg", "SMSG_LFG_ROLE_CHOSEN {} guid: {} roles: {}",
        GetPlayerInfo(), guid.ToString(), roles);

    WorldPackets::LFG::RoleChosen roleChosen;
    roleChosen.Player = guid;
    roleChosen.RoleMask = roles;
    roleChosen.Accepted = roles > 0;
    SendPacket(roleChosen.Write());
}

void WorldSession::SendLfgRoleCheckUpdate(lfg::LfgRoleCheck const& roleCheck)
{
    lfg::LfgDungeonSet dungeons;
    if (roleCheck.rDungeonId)
        dungeons.insert(roleCheck.rDungeonId);
    else
        dungeons = roleCheck.dungeons;

    TC_LOG_DEBUG("lfg", "SMSG_LFG_ROLE_CHECK_UPDATE {}", GetPlayerInfo());
    WorldPackets::LFG::LFGRoleCheckUpdate lfgRoleCheckUpdate;
    lfgRoleCheckUpdate.PartyIndex = 127;
    lfgRoleCheckUpdate.RoleCheckStatus = roleCheck.state;
    std::transform(dungeons.begin(), dungeons.end(), std::back_inserter(lfgRoleCheckUpdate.JoinSlots), [](uint32 dungeonId)
    {
        return sLFGMgr->GetLFGDungeonEntry(dungeonId);
    });
    lfgRoleCheckUpdate.GroupFinderActivityID = 0;
    if (!roleCheck.roles.empty())
    {
        // Leader info MUST be sent 1st :S
        uint8 roles = roleCheck.roles.find(roleCheck.leader)->second;
        lfgRoleCheckUpdate.Members.emplace_back(roleCheck.leader, roles, ASSERT_NOTNULL(sCharacterCache->GetCharacterCacheByGuid(roleCheck.leader))->Level, roles > 0);

        for (lfg::LfgRolesMap::const_iterator it = roleCheck.roles.begin(); it != roleCheck.roles.end(); ++it)
        {
            if (it->first == roleCheck.leader)
                continue;

            roles = it->second;
            lfgRoleCheckUpdate.Members.emplace_back(it->first, roles, ASSERT_NOTNULL(sCharacterCache->GetCharacterCacheByGuid(it->first))->Level, roles > 0);
        }
    }

    SendPacket(lfgRoleCheckUpdate.Write());
}

void WorldSession::SendLfgJoinResult(lfg::LfgJoinResultData const& joinData)
{
    TC_LOG_DEBUG("lfg", "SMSG_LFG_JOIN_RESULT {} checkResult: {} checkValue: {}",
        GetPlayerInfo(), joinData.result, joinData.state);

    WorldPackets::LFG::LFGJoinResult lfgJoinResult;
    if (WorldPackets::LFG::RideTicket const* ticket = sLFGMgr->GetTicket(GetPlayer()->GetGUID()))
        lfgJoinResult.Ticket = *ticket;
    lfgJoinResult.Result = joinData.result;
    if (joinData.result == lfg::LFG_JOIN_ROLE_CHECK_FAILED)
        lfgJoinResult.ResultDetail = joinData.state;
    else if (joinData.result == lfg::LFG_JOIN_NO_SLOTS)
        lfgJoinResult.BlackListNames = joinData.playersMissingRequirement;

    for (lfg::LfgLockPartyMap::const_iterator it = joinData.lockmap.begin(); it != joinData.lockmap.end(); ++it)
    {
        lfgJoinResult.BlackList.emplace_back();
        WorldPackets::LFG::LFGBlackList& blackList = lfgJoinResult.BlackList.back();
        blackList.PlayerGuid = it->first;

        for (lfg::LfgLockMap::const_iterator itr = it->second.begin(); itr != it->second.end(); ++itr)
        {
            TC_LOG_TRACE("lfg", "SendLfgJoinResult:: {} DungeonID: {} Lock status: {} Required itemLevel: {} Current itemLevel: {}",
                it->first.ToString(), (itr->first & 0x00FFFFFF), itr->second.lockStatus, itr->second.requiredItemLevel, itr->second.currentItemLevel);

            blackList.Slot.emplace_back(itr->first, itr->second.lockStatus, itr->second.requiredItemLevel, itr->second.currentItemLevel, 0);
        }
    }

    SendPacket(lfgJoinResult.Write());
}

void WorldSession::SendLfgQueueStatus(lfg::LfgQueueStatusData const& queueData)
{
    TC_LOG_DEBUG("lfg", "SMSG_LFG_QUEUE_STATUS {} state: {}, dungeon: {}, waitTime: {}, "
        "avgWaitTime: {}, waitTimeTanks: {}, waitTimeHealer: {}, waitTimeDps: {}, "
        "queuedTime: {}, tanks: {}, healers: {}, dps: {}",
        GetPlayerInfo(), lfg::GetStateString(sLFGMgr->GetState(GetPlayer()->GetGUID())), queueData.dungeonId, queueData.waitTime, queueData.waitTimeAvg,
        queueData.waitTimeTank, queueData.waitTimeHealer, queueData.waitTimeDps,
        queueData.queuedTime, queueData.tanks, queueData.healers, queueData.dps);

    WorldPackets::LFG::LFGQueueStatus lfgQueueStatus;
    if (WorldPackets::LFG::RideTicket const* ticket = sLFGMgr->GetTicket(GetPlayer()->GetGUID()))
        lfgQueueStatus.Ticket = *ticket;
    lfgQueueStatus.Slot = sLFGMgr->GetLFGDungeonEntry(queueData.dungeonId);
    lfgQueueStatus.AvgWaitTimeMe = queueData.waitTime;
    lfgQueueStatus.AvgWaitTime = queueData.waitTimeAvg;
    lfgQueueStatus.AvgWaitTimeByRole[0] = queueData.waitTimeTank;
    lfgQueueStatus.AvgWaitTimeByRole[1] = queueData.waitTimeHealer;
    lfgQueueStatus.AvgWaitTimeByRole[2] = queueData.waitTimeDps;
    lfgQueueStatus.LastNeeded[0] = queueData.tanks;
    lfgQueueStatus.LastNeeded[1] = queueData.healers;
    lfgQueueStatus.LastNeeded[2] = queueData.dps;
    lfgQueueStatus.QueuedTime = queueData.queuedTime;
    SendPacket(lfgQueueStatus.Write());
}

void WorldSession::SendLfgPlayerReward(lfg::LfgPlayerRewardData const& rewardData)
{
    if (!rewardData.rdungeonEntry || !rewardData.sdungeonEntry || !rewardData.quest)
        return;

    TC_LOG_DEBUG("lfg", "SMSG_LFG_PLAYER_REWARD {} rdungeonEntry: {}, sdungeonEntry: {}, done: {}",
        GetPlayerInfo(), rewardData.rdungeonEntry, rewardData.sdungeonEntry, rewardData.done);

    WorldPackets::LFG::LFGPlayerReward lfgPlayerReward;
    lfgPlayerReward.QueuedSlot = rewardData.rdungeonEntry;
    lfgPlayerReward.ActualSlot = rewardData.sdungeonEntry;
    lfgPlayerReward.RewardMoney = GetPlayer()->GetQuestMoneyReward(rewardData.quest);
    lfgPlayerReward.AddedXP = GetPlayer()->GetQuestXPReward(rewardData.quest);

    for (uint8 i = 0; i < QUEST_REWARD_ITEM_COUNT; ++i)
        if (uint32 itemId = rewardData.quest->RewardItemId[i])
            lfgPlayerReward.Rewards.emplace_back(itemId, rewardData.quest->RewardItemCount[i], 0, false);

    for (uint32 i = 0; i < QUEST_REWARD_CURRENCY_COUNT; ++i)
        if (uint32 curencyId = rewardData.quest->RewardCurrencyId[i])
            lfgPlayerReward.Rewards.emplace_back(curencyId, rewardData.quest->RewardCurrencyCount[i], 0, true);

    SendPacket(lfgPlayerReward.Write());
}

void WorldSession::SendLfgBootProposalUpdate(lfg::LfgPlayerBoot const& boot)
{
    lfg::LfgAnswer playerVote = boot.votes.find(GetPlayer()->GetGUID())->second;
    uint8 votesNum = 0;
    uint8 agreeNum = 0;
    int32 secsleft = int32((boot.cancelTime - GameTime::GetGameTime()) / 1000);
    for (const auto& vote : boot.votes)
    {
        if (vote.second != lfg::LFG_ANSWER_PENDING)
        {
            ++votesNum;
            if (vote.second == lfg::LFG_ANSWER_AGREE)
                ++agreeNum;
        }
    }
    TC_LOG_DEBUG("lfg", "SMSG_LFG_BOOT_PROPOSAL_UPDATE {} inProgress: {} - "
        "didVote: {} - agree: {} - victim: {} votes: {} - agrees: {} - left: {} - "
        "needed: {} - reason {}",
        GetPlayerInfo(), uint8(boot.inProgress), uint8(playerVote != lfg::LFG_ANSWER_PENDING),
        uint8(playerVote == lfg::LFG_ANSWER_AGREE), boot.victim.ToString(), votesNum, agreeNum,
        secsleft, lfg::LFG_GROUP_KICK_VOTES_NEEDED, boot.reason);

    WorldPackets::LFG::LfgBootPlayer lfgBootPlayer;
    lfgBootPlayer.Info.VoteInProgress = boot.inProgress;
    lfgBootPlayer.Info.VotePassed = agreeNum >= lfg::LFG_GROUP_KICK_VOTES_NEEDED;
    lfgBootPlayer.Info.MyVoteCompleted = playerVote != lfg::LFG_ANSWER_PENDING;
    lfgBootPlayer.Info.MyVote = playerVote == lfg::LFG_ANSWER_AGREE;
    lfgBootPlayer.Info.Target = boot.victim;
    lfgBootPlayer.Info.TotalVotes = votesNum;
    lfgBootPlayer.Info.BootVotes = agreeNum;
    lfgBootPlayer.Info.TimeLeft = secsleft;
    lfgBootPlayer.Info.VotesNeeded = lfg::LFG_GROUP_KICK_VOTES_NEEDED;
    lfgBootPlayer.Info.Reason = boot.reason;
    SendPacket(lfgBootPlayer.Write());
}

void WorldSession::SendLfgUpdateProposal(lfg::LfgProposal const& proposal)
{
    ObjectGuid guid = GetPlayer()->GetGUID();
    ObjectGuid gguid = proposal.players.find(guid)->second.group;
    bool silent = !proposal.isNew && gguid == proposal.group;
    uint32 dungeonEntry = proposal.dungeonId;

    TC_LOG_DEBUG("lfg", "SMSG_LFG_PROPOSAL_UPDATE {} state: {}",
        GetPlayerInfo(), proposal.state);

    // show random dungeon if player selected random dungeon and it's not lfg group
    if (!silent)
    {
        lfg::LfgDungeonSet const& playerDungeons = sLFGMgr->GetSelectedDungeons(guid);
        if (playerDungeons.find(proposal.dungeonId) == playerDungeons.end())
            dungeonEntry = (*playerDungeons.begin());
    }

    WorldPackets::LFG::LFGProposalUpdate lfgProposalUpdate;
    if (WorldPackets::LFG::RideTicket const* ticket = sLFGMgr->GetTicket(GetPlayer()->GetGUID()))
        lfgProposalUpdate.Ticket = *ticket;
    lfgProposalUpdate.InstanceID = 0;
    lfgProposalUpdate.ProposalID = proposal.id;
    lfgProposalUpdate.Slot = sLFGMgr->GetLFGDungeonEntry(dungeonEntry);
    lfgProposalUpdate.State = proposal.state;
    lfgProposalUpdate.CompletedMask = proposal.encounters;
    lfgProposalUpdate.ValidCompletedMask = true;
    lfgProposalUpdate.ProposalSilent = silent;
    lfgProposalUpdate.FailedByMyParty = !proposal.isNew;

    for (auto const& player : proposal.players)
    {
        lfgProposalUpdate.Players.emplace_back();
        auto& proposalPlayer = lfgProposalUpdate.Players.back();
        proposalPlayer.Roles = player.second.role;
        proposalPlayer.Me = player.first == guid;
        proposalPlayer.MyParty = !player.second.group.IsEmpty() && player.second.group == proposal.group;
        proposalPlayer.SameParty = !player.second.group.IsEmpty() && player.second.group == gguid;
        proposalPlayer.Responded = player.second.accept != lfg::LFG_ANSWER_PENDING;
        proposalPlayer.Accepted = player.second.accept == lfg::LFG_ANSWER_AGREE;
    }

    SendPacket(lfgProposalUpdate.Write());
}

void WorldSession::SendLfgDisabled()
{
    TC_LOG_DEBUG("lfg", "SMSG_LFG_DISABLED {}", GetPlayerInfo());
    SendPacket(WorldPackets::LFG::LFGDisabled().Write());
}

void WorldSession::SendLfgOfferContinue(uint32 dungeonEntry)
{
    TC_LOG_DEBUG("lfg", "SMSG_LFG_OFFER_CONTINUE {} dungeon entry: {}",
        GetPlayerInfo(), dungeonEntry);
    SendPacket(WorldPackets::LFG::LFGOfferContinue(sLFGMgr->GetLFGDungeonEntry(dungeonEntry)).Write());
}

void WorldSession::SendLfgTeleportError(lfg::LfgTeleportResult err)
{
    TC_LOG_DEBUG("lfg", "SMSG_LFG_TELEPORT_DENIED {} reason: {}",
        GetPlayerInfo(), err);
    SendPacket(WorldPackets::LFG::LFGTeleportDenied(err).Write());
}

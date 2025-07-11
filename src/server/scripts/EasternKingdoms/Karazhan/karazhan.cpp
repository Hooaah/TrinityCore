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

/* ScriptData
SDName: Karazhan
SD%Complete: 100
SDComment: Support for Barnes (Opera controller) and Berthold (Doorman), Support for Quest 9645.
SDCategory: Karazhan
EndScriptData */

/* ContentData
npc_barnes
npc_image_of_medivh
EndContentData */

#include "ScriptMgr.h"
#include "InstanceScript.h"
#include "karazhan.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptedEscortAI.h"
#include "ScriptedGossip.h"
#include "TemporarySummon.h"

enum Spells
{
    // Barnes
    SPELL_SPOTLIGHT             = 25824,
    SPELL_TUXEDO                = 32616,

    // Berthold
    SPELL_TELEPORT              = 39567,

    // Image of Medivh
    SPELL_FIRE_BALL             = 30967,
    SPELL_UBER_FIREBALL         = 30971,
    SPELL_CONFLAGRATION_BLAST   = 30977,
    SPELL_MANA_SHIELD           = 31635
};

enum Creatures
{
    NPC_ARCANAGOS               = 17652,
    NPC_SPOTLIGHT               = 19525
};

/*######
# npc_barnesAI
######*/

enum Misc
{
    OZ_GOSSIP1_MID              = 7421, // I'm not an actor.
    OZ_GOSSIP1_OID              = 0,
    OZ_GOSSIP2_MID              = 7422, // Ok, I'll give it a try, then.
    OZ_GOSSIP2_OID              = 0,
};

#define OZ_GM_GOSSIP1       "[GM] Change event to EVENT_OZ"
#define OZ_GM_GOSSIP2       "[GM] Change event to EVENT_HOOD"
#define OZ_GM_GOSSIP3       "[GM] Change event to EVENT_RAJ"

struct Dialogue
{
    int32 textid;
    uint32 timer;
};

static Dialogue OzDialogue[]=
{
    {0, 6000},
    {1, 18000},
    {2, 9000},
    {3, 15000}
};

static Dialogue HoodDialogue[]=
{
    {4, 6000},
    {5, 10000},
    {6, 14000},
    {7, 15000}
};

static Dialogue RAJDialogue[]=
{
    {8, 5000},
    {9, 7000},
    {10, 14000},
    {11, 14000}
};

// Entries and spawn locations for creatures in Oz event
float Spawns[6][2]=
{
    {17535, -10896},                                        // Dorothee
    {17546, -10891},                                        // Roar
    {17547, -10884},                                        // Tinhead
    {17543, -10902},                                        // Strawman
    {17603, -10892},                                        // Grandmother
    {17534, -10900},                                        // Julianne
};

#define SPAWN_Z             90.5f
#define SPAWN_Y             -1758
#define SPAWN_O             4.738f

static constexpr uint32 PATH_ESCORT_BARNES = 134498;

class npc_barnes : public CreatureScript
{
public:
    npc_barnes() : CreatureScript("npc_barnes") { }

    struct npc_barnesAI : public EscortAI
    {
        npc_barnesAI(Creature* creature) : EscortAI(creature)
        {
            Initialize();
            RaidWiped = false;
            m_uiEventId = 0;
            instance = creature->GetInstanceScript();
        }

        void Initialize()
        {
            m_uiSpotlightGUID.Clear();

            TalkCount = 0;
            TalkTimer = 2000;
            WipeTimer = 5000;

            PerformanceReady = false;
        }

        InstanceScript* instance;

        ObjectGuid m_uiSpotlightGUID;

        uint32 TalkCount;
        uint32 TalkTimer;
        uint32 WipeTimer;
        uint32 m_uiEventId;

        bool PerformanceReady;
        bool RaidWiped;

        void Reset() override
        {
            Initialize();

            m_uiEventId = instance->GetData(DATA_OPERA_PERFORMANCE);
        }

        void StartEvent()
        {
            instance->SetBossState(DATA_OPERA_PERFORMANCE, IN_PROGRESS);

            //resets count for this event, in case earlier failed
            if (m_uiEventId == EVENT_OZ)
                instance->SetData(DATA_OPERA_OZ_DEATHCOUNT, IN_PROGRESS);

            LoadPath(PATH_ESCORT_BARNES);
            Start(false);
        }

        void JustEngagedWith(Unit* /*who*/) override { }

        void WaypointReached(uint32 waypointId, uint32 /*pathId*/) override
        {
            switch (waypointId)
            {
                case 0:
                    DoCast(me, SPELL_TUXEDO, false);
                    instance->DoUseDoorOrButton(instance->GetGuidData(DATA_GO_STAGEDOORLEFT));
                    break;
                case 4:
                    TalkCount = 0;
                    SetEscortPaused(true);

                    if (Creature* spotlight = me->SummonCreature(NPC_SPOTLIGHT,
                        me->GetPositionX(), me->GetPositionY(), me->GetPositionZ(), 0.0f,
                        TEMPSUMMON_TIMED_OR_DEAD_DESPAWN, 1min))
                    {
                        spotlight->SetUninteractible(true);
                        spotlight->CastSpell(spotlight, SPELL_SPOTLIGHT, false);
                        m_uiSpotlightGUID = spotlight->GetGUID();
                    }
                    break;
                case 8:
                    instance->DoUseDoorOrButton(instance->GetGuidData(DATA_GO_STAGEDOORLEFT));
                    PerformanceReady = true;
                    break;
                case 9:
                    PrepareEncounter();
                    instance->DoUseDoorOrButton(instance->GetGuidData(DATA_GO_CURTAINS));
                    break;
            }
        }

        void Talk(uint32 count)
        {
            int32 text = 0;

            switch (m_uiEventId)
            {
                case EVENT_OZ:
                    if (OzDialogue[count].textid)
                         text = OzDialogue[count].textid;
                    if (OzDialogue[count].timer)
                        TalkTimer = OzDialogue[count].timer;
                    break;

                case EVENT_HOOD:
                    if (HoodDialogue[count].textid)
                        text = HoodDialogue[count].textid;
                    if (HoodDialogue[count].timer)
                        TalkTimer = HoodDialogue[count].timer;
                    break;

                case EVENT_RAJ:
                     if (RAJDialogue[count].textid)
                         text = RAJDialogue[count].textid;
                    if (RAJDialogue[count].timer)
                        TalkTimer = RAJDialogue[count].timer;
                    break;
            }

            if (text)
                 CreatureAI::Talk(text);
        }

        void PrepareEncounter()
        {
            TC_LOG_DEBUG("scripts", "Barnes Opera Event - Introduction complete - preparing encounter {}", m_uiEventId);
            uint8 index = 0;
            uint8 count = 0;

            switch (m_uiEventId)
            {
                case EVENT_OZ:
                    index = 0;
                    count = 4;
                    break;
                case EVENT_HOOD:
                    index = 4;
                    count = index+1;
                    break;
                case EVENT_RAJ:
                    index = 5;
                    count = index+1;
                    break;
            }

            for (; index < count; ++index)
            {
                uint32 entry = ((uint32)Spawns[index][0]);
                float PosX = Spawns[index][1];

                if (Creature* creature = me->SummonCreature(entry, PosX, SPAWN_Y, SPAWN_Z, SPAWN_O, TEMPSUMMON_TIMED_OR_DEAD_DESPAWN, 2h))
                    creature->SetUnitFlag(UNIT_FLAG_NON_ATTACKABLE);
            }

            RaidWiped = false;
        }

        void UpdateAI(uint32 diff) override
        {
            EscortAI::UpdateAI(diff);

            if (HasEscortState(STATE_ESCORT_PAUSED))
            {
                if (TalkTimer <= diff)
                {
                    if (TalkCount > 3)
                    {
                        if (Creature* pSpotlight = ObjectAccessor::GetCreature(*me, m_uiSpotlightGUID))
                            pSpotlight->DespawnOrUnsummon();

                        SetEscortPaused(false);
                        return;
                    }

                    Talk(TalkCount);
                    ++TalkCount;
                } else TalkTimer -= diff;
            }

            if (PerformanceReady)
            {
                if (!RaidWiped)
                {
                    if (WipeTimer <= diff)
                    {
                        Map::PlayerList const& PlayerList = me->GetMap()->GetPlayers();
                        if (PlayerList.empty())
                            return;

                        RaidWiped = true;
                        for (Map::PlayerList::const_iterator i = PlayerList.begin(); i != PlayerList.end(); ++i)
                        {
                            if (i->GetSource()->IsAlive() && !i->GetSource()->IsGameMaster())
                            {
                                RaidWiped = false;
                                break;
                            }
                        }

                        if (RaidWiped)
                        {
                            EnterEvadeMode(EvadeReason::Other);
                            return;
                        }

                        WipeTimer = 15000;
                    } else WipeTimer -= diff;
                }
            }
        }

        bool OnGossipSelect(Player* player, uint32 /*menuId*/, uint32 gossipListId) override
        {
            uint32 const action = player->PlayerTalkClass->GetGossipOptionAction(gossipListId);
            ClearGossipMenuFor(player);

            switch (action)
            {
                case GOSSIP_ACTION_INFO_DEF + 1:
                    InitGossipMenuFor(player, OZ_GOSSIP2_MID);
                    AddGossipItemFor(player, OZ_GOSSIP2_MID, OZ_GOSSIP2_OID, GOSSIP_SENDER_MAIN, GOSSIP_ACTION_INFO_DEF + 2);
                    SendGossipMenuFor(player, 8971, me->GetGUID());
                    break;
                case GOSSIP_ACTION_INFO_DEF + 2:
                    CloseGossipMenuFor(player);
                    m_uiEventId = urand(EVENT_OZ, EVENT_RAJ);
                    StartEvent();
                    break;
                case GOSSIP_ACTION_INFO_DEF + 3:
                    CloseGossipMenuFor(player);
                    m_uiEventId = EVENT_OZ;
                    TC_LOG_DEBUG("scripts", "player ({}) manually set Opera event to EVENT_OZ", player->GetGUID().ToString());
                    break;
                case GOSSIP_ACTION_INFO_DEF + 4:
                    CloseGossipMenuFor(player);
                    m_uiEventId = EVENT_HOOD;
                    TC_LOG_DEBUG("scripts", "player ({}) manually set Opera event to EVENT_HOOD", player->GetGUID().ToString());
                    break;
                case GOSSIP_ACTION_INFO_DEF + 5:
                    CloseGossipMenuFor(player);
                    m_uiEventId = EVENT_RAJ;
                    TC_LOG_DEBUG("scripts", "player ({}) manually set Opera event to EVENT_RAJ", player->GetGUID().ToString());
                    break;
            }

            return true;
        }

        bool OnGossipHello(Player* player) override
        {
            InitGossipMenuFor(player, OZ_GOSSIP1_MID);
            // Check for death of Moroes and if opera event is not done already
            if (instance->GetBossState(DATA_MOROES) == DONE && instance->GetBossState(DATA_OPERA_PERFORMANCE) != DONE)
            {
                AddGossipItemFor(player, OZ_GOSSIP1_MID, OZ_GOSSIP1_OID, GOSSIP_SENDER_MAIN, GOSSIP_ACTION_INFO_DEF + 1);

                if (player->IsGameMaster())
                {
                    AddGossipItemFor(player, GossipOptionNpc::None, OZ_GM_GOSSIP1, GOSSIP_SENDER_MAIN, GOSSIP_ACTION_INFO_DEF + 3);
                    AddGossipItemFor(player, GossipOptionNpc::None, OZ_GM_GOSSIP2, GOSSIP_SENDER_MAIN, GOSSIP_ACTION_INFO_DEF + 4);
                    AddGossipItemFor(player, GossipOptionNpc::None, OZ_GM_GOSSIP3, GOSSIP_SENDER_MAIN, GOSSIP_ACTION_INFO_DEF + 5);
                }

                if (!RaidWiped)
                    SendGossipMenuFor(player, 8970, me->GetGUID());
                else
                    SendGossipMenuFor(player, 8975, me->GetGUID());

                return true;
            }

            SendGossipMenuFor(player, 8978, me->GetGUID());
            return true;
        }
    };

    CreatureAI* GetAI(Creature* creature) const override
    {
        return GetKarazhanAI<npc_barnesAI>(creature);
    }
};

/*###
# npc_image_of_medivh
####*/

enum
{
    SAY_DIALOG_MEDIVH_1             = 0,
    SAY_DIALOG_ARCANAGOS_2          = 0,
    SAY_DIALOG_MEDIVH_3             = 1,
    SAY_DIALOG_ARCANAGOS_4          = 1,
    SAY_DIALOG_MEDIVH_5             = 2,
    SAY_DIALOG_ARCANAGOS_6          = 2,
    EMOTE_DIALOG_MEDIVH_7           = 3,
    SAY_DIALOG_ARCANAGOS_8          = 3,
    SAY_DIALOG_MEDIVH_9             = 4
};

static float MedivPos[4] = {-11161.49f, -1902.24f, 91.48f, 1.94f};
static float ArcanagosPos[4] = {-11169.75f, -1881.48f, 95.39f, 4.83f};

class npc_image_of_medivh : public CreatureScript
{
public:
    npc_image_of_medivh() : CreatureScript("npc_image_of_medivh") { }

    CreatureAI* GetAI(Creature* creature) const override
    {
        return GetKarazhanAI<npc_image_of_medivhAI>(creature);
    }

    struct npc_image_of_medivhAI : public ScriptedAI
    {
        npc_image_of_medivhAI(Creature* creature) : ScriptedAI(creature)
        {
            Initialize();
            Step = 0;
            FireArcanagosTimer = 0;
            FireMedivhTimer = 0;
            instance = creature->GetInstanceScript();
        }

        void Initialize()
        {
            ArcanagosGUID.Clear();
            EventStarted = false;
            YellTimer = 0;
        }

        InstanceScript* instance;

        ObjectGuid ArcanagosGUID;

        uint32 YellTimer;
        uint32 Step;
        uint32 FireMedivhTimer;
        uint32 FireArcanagosTimer;

        bool EventStarted;

        void Reset() override
        {
            Initialize();
            me->SetUnitFlag(UNIT_FLAG_NON_ATTACKABLE);

            if (instance->GetGuidData(DATA_IMAGE_OF_MEDIVH).IsEmpty())
            {
                instance->SetGuidData(DATA_IMAGE_OF_MEDIVH, me->GetGUID());
                me->GetMotionMaster()->MovePoint(1, MedivPos[0], MedivPos[1], MedivPos[2]);
                Step = 0;
            }
            else
            {
                me->DespawnOrUnsummon();
            }
        }
        void JustEngagedWith(Unit* /*who*/) override { }

        void MovementInform(uint32 type, uint32 id) override
        {
            if (type != POINT_MOTION_TYPE)
                return;
            if (id == 1)
            {
                StartEvent();
                me->SetOrientation(MedivPos[3]);
                me->SetOrientation(MedivPos[3]);
            }
        }

        void StartEvent()
        {
            Step = 1;
            EventStarted = true;
            Creature* Arcanagos = me->SummonCreature(NPC_ARCANAGOS, ArcanagosPos[0], ArcanagosPos[1], ArcanagosPos[2], 0, TEMPSUMMON_CORPSE_TIMED_DESPAWN, 20s);
            if (!Arcanagos)
                return;
            ArcanagosGUID = Arcanagos->GetGUID();
            Arcanagos->SetDisableGravity(true);
            Arcanagos->GetMotionMaster()->MovePoint(0, ArcanagosPos[0], ArcanagosPos[1], ArcanagosPos[2]);
            Arcanagos->SetOrientation(ArcanagosPos[3]);
            me->SetOrientation(MedivPos[3]);
            YellTimer = 10000;
        }

        uint32 NextStep(uint32 step)
        {
            switch (step)
            {
            case 0: return 9999999;
            case 1:
                Talk(SAY_DIALOG_MEDIVH_1);
                return 10000;
            case 2:
                if (Creature* arca = ObjectAccessor::GetCreature(*me, ArcanagosGUID))
                    arca->AI()->Talk(SAY_DIALOG_ARCANAGOS_2);
                return 20000;
            case 3:
                Talk(SAY_DIALOG_MEDIVH_3);
                return 10000;
            case 4:
                if (Creature* arca = ObjectAccessor::GetCreature(*me, ArcanagosGUID))
                    arca->AI()->Talk(SAY_DIALOG_ARCANAGOS_4);
                return 20000;
            case 5:
                Talk(SAY_DIALOG_MEDIVH_5);
                return 20000;
            case 6:
                if (Creature* arca = ObjectAccessor::GetCreature(*me, ArcanagosGUID))
                    arca->AI()->Talk(SAY_DIALOG_ARCANAGOS_6);
                return 10000;
            case 7:
                FireArcanagosTimer = 500;
                return 5000;
            case 8:
                FireMedivhTimer = 500;
                DoCast(me, SPELL_MANA_SHIELD);
                return 10000;
            case 9:
                Talk(EMOTE_DIALOG_MEDIVH_7);
                return 10000;
            case 10:
                if (Creature* arca = ObjectAccessor::GetCreature(*me, ArcanagosGUID))
                    DoCast(arca, SPELL_CONFLAGRATION_BLAST, false);
                return 1000;
            case 11:
                if (Creature* arca = ObjectAccessor::GetCreature(*me, ArcanagosGUID))
                    arca->AI()->Talk(SAY_DIALOG_ARCANAGOS_8);
                return 5000;
            case 12:
                if (Creature* arca = ObjectAccessor::GetCreature(*me, ArcanagosGUID))
                {
                    arca->GetMotionMaster()->MovePoint(0, -11010.82f, -1761.18f, 156.47f);
                    arca->setActive(true);
                    arca->SetFarVisible(true);
                    arca->InterruptNonMeleeSpells(true);
                    arca->SetSpeedRate(MOVE_FLIGHT, 2.0f);
                }
                return 10000;
            case 13:
                Talk(SAY_DIALOG_MEDIVH_9);
                return 10000;
            case 14:
            {
                me->SetVisible(false);
                me->ClearInCombat();

                InstanceMap::PlayerList const& PlayerList = me->GetMap()->GetPlayers();
                for (InstanceMap::PlayerList::const_iterator i = PlayerList.begin(); i != PlayerList.end(); ++i)
                {
                    if (i->GetSource()->IsAlive())
                    {
                        if (i->GetSource()->GetQuestStatus(9645) == QUEST_STATUS_INCOMPLETE)
                            i->GetSource()->CompleteQuest(9645);
                    }
                }
                return 50000;
            }
            case 15:
                if (Creature* arca = ObjectAccessor::GetCreature(*me, ArcanagosGUID))
                    arca->KillSelf();
                return 5000;
            default:
                return 9999999;
            }
        }

        void UpdateAI(uint32 diff) override
        {
            if (YellTimer <= diff)
            {
                if (EventStarted)
                    YellTimer = NextStep(Step++);
            } else YellTimer -= diff;

            if (Step >= 7 && Step <= 12)
            {
                Unit* arca = ObjectAccessor::GetUnit(*me, ArcanagosGUID);

                if (FireArcanagosTimer <= diff)
                {
                    if (arca)
                        arca->CastSpell(me, SPELL_FIRE_BALL, false);
                    FireArcanagosTimer = 6000;
                } else FireArcanagosTimer -= diff;

                if (FireMedivhTimer <= diff)
                {
                    if (arca)
                        DoCast(arca, SPELL_FIRE_BALL);
                    FireMedivhTimer = 5000;
                } else FireMedivhTimer -= diff;
            }
        }
    };
};

void AddSC_karazhan()
{
    new npc_barnes();
    new npc_image_of_medivh();
}

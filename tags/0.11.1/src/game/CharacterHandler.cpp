/* 
 * Copyright (C) 2005-2008 MaNGOS <http://www.mangosproject.org/>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "Common.h"
#include "Database/DatabaseEnv.h"
#include "WorldPacket.h"
#include "WorldSocket.h"
#include "WorldSession.h"
#include "Opcodes.h"
#include "Log.h"
#include "World.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Guild.h"
#include "UpdateMask.h"
#include "Auth/md5.h"
#include "MapManager.h"
#include "ObjectAccessor.h"
#include "Group.h"
#include "Database/DatabaseImpl.h"
#include "PlayerDump.h"
#include "SocialMgr.h"

class LoginQueryHolder : public SqlQueryHolder
{
    private:
        uint32 m_accountId;
        uint64 m_guid;
    public:
        LoginQueryHolder(uint32 accountId, uint64 guid)
            : m_accountId(accountId), m_guid(guid) { }
        uint64 GetGuid() const { return m_guid; }
        uint32 GetAccountId() const { return m_accountId; }
        bool Initialize();
};

bool LoginQueryHolder::Initialize()
{
    SetSize(MAX_PLAYER_LOGIN_QUERY);

    bool res = true;

    // NOTE: all fields in `characters` must be read to prevent lost character data at next save in case wrong DB structure.
    // !!! NOTE: including unused `zone`,`online`
    res &= SetPQuery(PLAYER_LOGIN_QUERY_LOADFROM,            "SELECT guid,account,data,name,race,class,position_x,position_y,position_z,map,orientation,taximask,cinematic,totaltime,leveltime,rest_bonus,logout_time,is_logout_resting,resettalents_cost,resettalents_time,trans_x,trans_y,trans_z,trans_o, transguid,gmstate,stable_slots,at_login,zone,online,death_expire_time,taxi_path FROM characters WHERE guid = '%u'", GUID_LOPART(m_guid));
    res &= SetPQuery(PLAYER_LOGIN_QUERY_LOADGROUP,           "SELECT leaderGuid FROM group_member WHERE memberGuid='%u'", GUID_LOPART(m_guid));
    res &= SetPQuery(PLAYER_LOGIN_QUERY_LOADBOUNDINSTANCES,  "SELECT map,instance,leader FROM character_instance WHERE guid = '%u'", GUID_LOPART(m_guid));
    res &= SetPQuery(PLAYER_LOGIN_QUERY_LOADAURAS,           "SELECT caster_guid,spell,effect_index,amount,maxduration,remaintime,remaincharges FROM character_aura WHERE guid = '%u'", GUID_LOPART(m_guid));
    res &= SetPQuery(PLAYER_LOGIN_QUERY_LOADSPELLS,          "SELECT spell,slot,active FROM character_spell WHERE guid = '%u'", GUID_LOPART(m_guid));
    res &= SetPQuery(PLAYER_LOGIN_QUERY_LOADQUESTSTATUS,     "SELECT quest,status,rewarded,explored,timer,mobcount1,mobcount2,mobcount3,mobcount4,itemcount1,itemcount2,itemcount3,itemcount4 FROM character_queststatus WHERE guid = '%u'", GUID_LOPART(m_guid));
    res &= SetPQuery(PLAYER_LOGIN_QUERY_LOADDAILYQUESTSTATUS,"SELECT quest,time FROM character_queststatus_daily WHERE guid = '%u'", GUID_LOPART(m_guid));
    res &= SetPQuery(PLAYER_LOGIN_QUERY_LOADTUTORIALS,       "SELECT tut0,tut1,tut2,tut3,tut4,tut5,tut6,tut7 FROM character_tutorial WHERE guid = '%u'", GUID_LOPART(m_guid));
    res &= SetPQuery(PLAYER_LOGIN_QUERY_LOADREPUTATION,      "SELECT faction,standing,flags FROM character_reputation WHERE guid = '%u'", GUID_LOPART(m_guid));
    res &= SetPQuery(PLAYER_LOGIN_QUERY_LOADINVENTORY,       "SELECT data,bag,slot,item,item_template FROM character_inventory JOIN item_instance ON character_inventory.item = item_instance.guid WHERE character_inventory.guid = '%u' ORDER BY bag,slot", GUID_LOPART(m_guid));
    res &= SetPQuery(PLAYER_LOGIN_QUERY_LOADACTIONS,         "SELECT button,action,type,misc FROM character_action WHERE guid = '%u' ORDER BY button", GUID_LOPART(m_guid));
    res &= SetPQuery(PLAYER_LOGIN_QUERY_LOADMAILCOUNT,       "SELECT COUNT(id) FROM mail WHERE receiver = '%u' AND checked = 0 AND deliver_time <= '" I64FMTD "'", GUID_LOPART(m_guid),(uint64)time(NULL));
    res &= SetPQuery(PLAYER_LOGIN_QUERY_LOADMAILDATE,        "SELECT MIN(deliver_time) FROM mail WHERE receiver = '%u' AND checked = 0", GUID_LOPART(m_guid));
    res &= SetPQuery(PLAYER_LOGIN_QUERY_LOADSOCIALLIST,      "SELECT friend,flags,note FROM character_social WHERE guid = '%u' LIMIT 255", GUID_LOPART(m_guid));
    res &= SetPQuery(PLAYER_LOGIN_QUERY_LOADHOMEBIND,        "SELECT map,zone,position_x,position_y,position_z FROM character_homebind WHERE guid = '%u'", GUID_LOPART(m_guid));
    res &= SetPQuery(PLAYER_LOGIN_QUERY_LOADSPELLCOOLDOWNS,  "SELECT spell,item,time FROM character_spell_cooldown WHERE guid = '%u'", GUID_LOPART(m_guid));

    // in other case still be dummy query

    res &= SetPQuery(PLAYER_LOGIN_QUERY_LOADGUILD,           "SELECT guildid,rank FROM guild_member WHERE guid = '%u'", GUID_LOPART(m_guid));

    return res;
}

// don't call WorldSession directly
// it may get deleted before the query callbacks get executed
// instead pass an account id to this handler
class CharacterHandler
{
    public:
        void HandleCharEnumCallback(QueryResult * result, uint32 account)
        {
            WorldSession * session = sWorld.FindSession(account);
            if(!session)
            {
                delete result;
                return;
            }
            session->HandleCharEnum(result);
        }
        void HandlePlayerLoginCallback(QueryResult * /*dummy*/, SqlQueryHolder * holder)
        {
            if (!holder) return;
            WorldSession *session = sWorld.FindSession(((LoginQueryHolder*)holder)->GetAccountId());
            if(!session)
            {
                delete holder;
                return;
            }
            session->HandlePlayerLogin((LoginQueryHolder*)holder);
        }
} chrHandler;

void WorldSession::HandleCharEnum(QueryResult * result)
{
    // keys can be non cleared if player open realm list and close it by 'cancel'
    loginDatabase.PExecute("UPDATE account SET v = '0', s = '0' WHERE id = '%u'", GetAccountId());

    WorldPacket data(SMSG_CHAR_ENUM, 100);                  // we guess size

    uint8 num = 0;

    data << num;

    if( result )
    {
        Player *plr = new Player(this);
        do
        {
            sLog.outDetail("Loading char guid %u from account %u.",(*result)[0].GetUInt32(),GetAccountId());

            if(plr->MinimalLoadFromDB( result, (*result)[0].GetUInt32() ))
            {
                plr->BuildEnumData( result, &data );
                ++num;
            }
        }
        while( result->NextRow() );

        delete plr;
        delete result;
    }

    data.put<uint8>(0, num);

    SendPacket( &data );
}

void WorldSession::HandleCharEnumOpcode( WorldPacket & /*recv_data*/ )
{
    /// get all the data necessary for loading all characters (along with their pets) on the account
    CharacterDatabase.AsyncPQuery(&chrHandler, &CharacterHandler::HandleCharEnumCallback, GetAccountId(),
    //          0                1                2                      3                      4                      5               6                     7                     8
        "SELECT characters.data, characters.name, characters.position_x, characters.position_y, characters.position_z, characters.map, characters.totaltime, characters.leveltime, characters.at_login, "
    //   9                    10                     11
        "character_pet.entry, character_pet.modelid, character_pet.level "
        "FROM characters LEFT JOIN character_pet ON characters.guid=character_pet.owner AND character_pet.slot='0' "
        "WHERE characters.account = '%u' ORDER BY characters.guid", GetAccountId());
}

void WorldSession::HandleCharCreateOpcode( WorldPacket & recv_data )
{
    // in Player::Create:
    //uint8 race,class_,gender,skin,face,hairStyle,hairColor,facialHair,outfitId;
    //data >> name
    //data >> race >> class_ >> gender >> skin >> face;
    //data >> hairStyle >> hairColor >> facialHair >> outfitId;

    CHECK_PACKET_SIZE(recv_data,1+1+1+1+1+1+1+1+1+1);

    std::string name;
    uint8 race_,class_;
    bool pTbc = this->IsTBC() && sWorld.getConfig(CONFIG_EXPANSION) > 0;
    recv_data >> name;

    // recheck with known string size
    CHECK_PACKET_SIZE(recv_data,(name.size()+1)+1+1+1+1+1+1+1+1+1);

    recv_data >> race_;
    recv_data >> class_;

    WorldPacket data(SMSG_CHAR_CREATE, 1);                  // returned with diff.values in all cases

    if (!sChrClassesStore.LookupEntry(class_)||
        !sChrRacesStore.LookupEntry(race_))
    {
        data << (uint8)CHAR_CREATE_FAILED;
        SendPacket( &data );
        sLog.outError("Class: %u or Race %u not found in DBC (Wrong DBC files?) or Cheater?", class_, race_);
        return;
    }

    // prevent character creating Expansion race without Expansion account
    if (!pTbc&&(race_>RACE_TROLL))
    {
        data << (uint8)CHAR_CREATE_EXPANSION;
        sLog.outError("No Expansion Account:[%d] but tried to Create TBC character",GetAccountId());
        SendPacket( &data );
        return;
    }

    // prevent character creating with invalid name
    if(name.empty())
    {
        data << (uint8)CHAR_NAME_INVALID_CHARACTER;
        SendPacket( &data );
        sLog.outError("Account:[%d] but tried to Create character with empty [name] ",GetAccountId());
        return;
    }

    normalizePlayerName(name);

    // check name limitations
    if(!ObjectMgr::IsValidName(name))
    {
        data << (uint8)CHAR_NAME_INVALID_CHARACTER;
        SendPacket( &data );
        return;
    }

    if(GetSecurity() == SEC_PLAYER && objmgr.IsReservedName(name))
    {
        data << (uint8)CHAR_NAME_RESERVED;
        SendPacket( &data );
        return;
    }

    if(objmgr.GetPlayerGUIDByName(name))
    {
        data << (uint8)CHAR_CREATE_NAME_IN_USE;
        SendPacket( &data );
        return;
    }

    QueryResult *result = CharacterDatabase.PQuery("SELECT COUNT(guid) FROM characters WHERE account = '%d'", GetAccountId());
    uint8 charcount = 0;
    if ( result )
    {
        Field *fields=result->Fetch();
        charcount = fields[0].GetUInt8();
        delete result;

        if (charcount >= 10)
        {
            data << (uint8)CHAR_CREATE_ACCOUNT_LIMIT;
            SendPacket( &data );
            return;
        }
    }

    bool AllowTwoSideAccounts = !sWorld.IsPvPRealm() || sWorld.getConfig(CONFIG_ALLOW_TWO_SIDE_ACCOUNTS) || GetSecurity() > SEC_PLAYER;
    uint32 skipCinematics = sWorld.getConfig(CONFIG_SKIP_CINEMATICS);

    bool have_same_race = false;
    if(!AllowTwoSideAccounts || skipCinematics == 1)
    {
        QueryResult *result2 = CharacterDatabase.PQuery("SELECT DISTINCT race FROM characters WHERE account = '%u' %s", GetAccountId(),skipCinematics == 1 ? "" : "LIMIT 1");
        if(result2)
        {
            uint32 team_= Player::TeamForRace(race_);

            Field* field = result2->Fetch();
            uint8 race = field[0].GetUInt32();

            // need to check team only for first character
            // TODO: what to if account already has characters of both races?
            if (!AllowTwoSideAccounts)
            {
                uint32 team=0;
                if(race > 0)
                    team = Player::TeamForRace(race);

                if(team != team_)
                {
                    data << (uint8)CHAR_CREATE_PVP_TEAMS_VIOLATION;
                    SendPacket( &data );
                    delete result2;
                    return;
                }
            }

            if (skipCinematics == 1)
            {
                // TODO: check if cinematic already shown? (already logged in?; cinematic field)
                while (race_ != race && result2->NextRow())
                {
                    field = result2->Fetch();
                    race = field[0].GetUInt32();
                }
                have_same_race = race_ == race;
            }
            delete result2;
        }
    }

    Player * pNewChar = new Player(this);
    recv_data.rpos(0);

    if(pNewChar->Create( objmgr.GenerateLowGuid(HIGHGUID_PLAYER), recv_data ))
    {
        if(have_same_race && skipCinematics == 1 || skipCinematics == 2)
        {
            pNewChar->setCinematic(1);                      // not show intro
        }

        // Player create
        pNewChar->SaveToDB();
        charcount+=1;

        loginDatabase.PExecute("DELETE FROM realmcharacters WHERE acctid= '%d' AND realmid = '%d'", GetAccountId(), realmID);
        loginDatabase.PExecute("INSERT INTO realmcharacters (numchars, acctid, realmid) VALUES (%u, %u, %u)",  charcount, GetAccountId(), realmID);

        delete pNewChar;
    }
    else
    {
        // Player not create (race/class problem?)
        delete pNewChar;

        data << (uint8)CHAR_CREATE_ERROR;
        SendPacket( &data );

        return;
    }

    data << (uint8)CHAR_CREATE_SUCCESS;
    SendPacket( &data );

    std::string IP_str = _socket ? _socket->GetRemoteAddress().c_str() : "-";
    sLog.outBasic("Account: %d (IP: %s) Create Character:[%s]",GetAccountId(),IP_str.c_str(),name.c_str());
    sLog.outChar("Account: %d (IP: %s) Create Character:[%s]",GetAccountId(),IP_str.c_str(),name.c_str());
}

void WorldSession::HandleCharDeleteOpcode( WorldPacket & recv_data )
{
    CHECK_PACKET_SIZE(recv_data,8);

    uint64 guid;
    recv_data >> guid;

    // can't delete loaded character
    if(objmgr.GetPlayer(guid))
        return;

    uint32 accountId = 0;
    std::string name;

    QueryResult *result = CharacterDatabase.PQuery("SELECT account,name FROM characters WHERE guid='%u'", GUID_LOPART(guid));
    if(result)
    {
        Field *fields = result->Fetch();
        accountId = fields[0].GetUInt32();
        name = fields[1].GetCppString();
        delete result;
    }

    // prevent deleting other players' characters using cheating tools
    if(accountId != GetAccountId())
        return;

    std::string IP_str = _socket ? _socket->GetRemoteAddress().c_str() : "-";
    sLog.outBasic("Account: %d (IP: %s) Delete Character:[%s] (guid:%u)",GetAccountId(),IP_str.c_str(),name.c_str(),GUID_LOPART(guid));
    sLog.outChar("Account: %d (IP: %s) Delete Character:[%s] (guid: %u)",GetAccountId(),IP_str.c_str(),name.c_str(),GUID_LOPART(guid));

    if(sLog.IsOutCharDump())                                // optimize GetPlayerDump call
    {
        std::string dump = PlayerDumpWriter().GetDump(GUID_LOPART(guid));
        sLog.outCharDump(dump.c_str(),GetAccountId(),GUID_LOPART(guid),name.c_str());
    }

    Player::DeleteFromDB(guid, GetAccountId());

    WorldPacket data(SMSG_CHAR_DELETE, 1);
    data << (uint8)CHAR_DELETE_SUCCESS;
    SendPacket( &data );
}

void WorldSession::HandlePlayerLoginOpcode( WorldPacket & recv_data )
{
    CHECK_PACKET_SIZE(recv_data,8);

    m_playerLoading = true;
    uint64 playerGuid = 0;

    DEBUG_LOG( "WORLD: Recvd Player Logon Message" );

    recv_data >> playerGuid;

    LoginQueryHolder *holder = new LoginQueryHolder(GetAccountId(), playerGuid);
    if(!holder->Initialize())
    {
        delete holder;                                      // delete all unprocessed queries
        m_playerLoading = false;
        return;
    }

    CharacterDatabase.DelayQueryHolder(&chrHandler, &CharacterHandler::HandlePlayerLoginCallback, holder);
}

void WorldSession::HandlePlayerLogin(LoginQueryHolder * holder)
{
    uint64 playerGuid = holder->GetGuid();

    Player* pCurrChar = new Player(this);
    pCurrChar->GetMotionMaster()->Initialize();

    // "GetAccountId()==db stored account id" checked in LoadFromDB (prevent login not own character using cheating tools)
    if(!pCurrChar->LoadFromDB(GUID_LOPART(playerGuid), holder))
    {
        KickPlayer();                                       // disconnect client, player no set to session and it will not deleted or saved at kick
        delete pCurrChar;                                   // delete it manually
        delete holder;                                      // delete all unprocessed queries
        m_playerLoading = false;
        return;
    }

    SetPlayer(pCurrChar);

    pCurrChar->SendDungeonDifficulty();

    WorldPacket data( SMSG_LOGIN_VERIFY_WORLD, 20 );
    data << pCurrChar->GetMapId();
    data << pCurrChar->GetPositionX();
    data << pCurrChar->GetPositionY();
    data << pCurrChar->GetPositionZ();
    data << pCurrChar->GetOrientation();
    SendPacket(&data);

    data.Initialize( SMSG_ACCOUNT_DATA_TIMES, 128 );
    for(int i = 0; i < 32; i++)
        data << uint32(0);
    SendPacket(&data);

    pCurrChar->GetSocial()->SendSocialList();

    data.Initialize(SMSG_FEATURE_SYSTEM_STATUS, 2);         // added in 2.2.0
    data << uint8(2);                                       // unknown value
    data << uint8(0);                                       // enable(1)/disable(0) voice chat interface in client
    SendPacket(&data);

    // Send MOTD
    {
        data.Initialize(SMSG_MOTD, 50);                     // new in 2.0.1
        data << (uint32)0;

        uint32 linecount=0;
        std::string str_motd = sWorld.GetMotd();
        std::string::size_type pos, nextpos;

        pos = 0;
        while ( (nextpos= str_motd.find('@',pos)) != std::string::npos )
        {
            if (nextpos != pos)
            {
                data << str_motd.substr(pos,nextpos-pos);
                ++linecount;
            }
            pos = nextpos+1;
        }

        if (pos<str_motd.length())
        {
            data << str_motd.substr(pos);
            ++linecount;
        }

        data.put(0, linecount);

        SendPacket( &data );
        DEBUG_LOG( "WORLD: Sent motd (SMSG_MOTD)" );
    }

    if(pCurrChar->GetGuildId() != 0)
    {
        Guild* guild = objmgr.GetGuildById(pCurrChar->GetGuildId());
        if(guild)
        {
            data.Initialize(SMSG_GUILD_EVENT, (2+guild->GetMOTD().size()+1));
            data << (uint8)GE_MOTD;
            data << (uint8)1;
            data << guild->GetMOTD();
            SendPacket(&data);
            DEBUG_LOG( "WORLD: Sent guild-motd (SMSG_GUILD_EVENT)" );

            data.Initialize(SMSG_GUILD_EVENT, (5+10));      // we guess size
            data<<(uint8)GE_SIGNED_ON;
            data<<(uint8)1;
            data<<pCurrChar->GetName();
            data<<pCurrChar->GetGUID();
            guild->BroadcastPacket(&data);
            DEBUG_LOG( "WORLD: Sent guild-signed-on (SMSG_GUILD_EVENT)" );

            // Increment online members of the guild
            guild->IncOnlineMemberCount();
        }
        else
        {
            // remove wrong guild data
            sLog.outError("Player %s (GUID: %u) marked as member not existed guild (id: %u), removing guild membership for player.",pCurrChar->GetName(),pCurrChar->GetGUIDLow(),pCurrChar->GetGuildId());
            pCurrChar->SetUInt32Value(PLAYER_GUILDID,0);
            pCurrChar->SetUInt32ValueInDB(PLAYER_GUILDID,0,pCurrChar->GetGUID());
        }
    }

    if(!pCurrChar->isAlive())
        pCurrChar->SendCorpseReclaimDelay(true);

    pCurrChar->SendInitialPacketsBeforeAddToMap();

    //Show cinematic at the first time that player login
    if( !pCurrChar->getCinematic() )
    {
        pCurrChar->setCinematic(1);

        ChrRacesEntry const* rEntry = sChrRacesStore.LookupEntry(pCurrChar->getRace());
        if(rEntry)
        {
            data.Initialize( SMSG_TRIGGER_CINEMATIC,4 );
            data << uint32(rEntry->startmovie);
            SendPacket( &data );
        }
    }

    //QueryResult *result = CharacterDatabase.PQuery("SELECT guildid,rank FROM guild_member WHERE guid = '%u'",pCurrChar->GetGUIDLow());
    QueryResult *resultGuild = holder->GetResult(PLAYER_LOGIN_QUERY_LOADGUILD);

    if(resultGuild)
    {
        Field *fields = resultGuild->Fetch();
        pCurrChar->SetInGuild(fields[0].GetUInt32());
        pCurrChar->SetRank(fields[1].GetUInt32());
        delete resultGuild;
    }
    else if(pCurrChar->GetGuildId())                        // clear guild related fields in case wrong data about non existed membership
    {
        pCurrChar->SetInGuild(0);
        pCurrChar->SetRank(0);
    }

    if (!MapManager::Instance().GetMap(pCurrChar->GetMapId(), pCurrChar)->AddInstanced(pCurrChar))
    {
        // TODO : Teleport to zone-in area
    }

    MapManager::Instance().GetMap(pCurrChar->GetMapId(), pCurrChar)->Add(pCurrChar);
    ObjectAccessor::Instance().AddObject(pCurrChar);
    //sLog.outDebug("Player %s added to Map.",pCurrChar->GetName());

    pCurrChar->SendInitialPacketsAfterAddToMap();

    CharacterDatabase.PExecute("UPDATE characters SET online = 1 WHERE guid = '%u'", pCurrChar->GetGUIDLow());
    loginDatabase.PExecute("UPDATE account SET online = 1 WHERE id = '%u'", GetAccountId());
    pCurrChar->SetInGameTime( getMSTime() );

    // announce group about member online (must be after add to player list to receive announce to self)
    if(pCurrChar->GetGroup())
    {
        //pCurrChar->groupInfo.group->SendInit(this); // useless
        pCurrChar->GetGroup()->SendUpdate();
    }

    // friend status
    sSocialMgr.SendFriendStatus(pCurrChar, FRIEND_ONLINE, pCurrChar->GetGUIDLow(), "", true);

    // Place character in world (and load zone) before some object loading
    pCurrChar->LoadCorpse();

    // setting Ghost+speed if dead
    //if ( pCurrChar->m_deathState == DEAD )
    if (pCurrChar->m_deathState != ALIVE)
    {
        // not blizz like, we must correctly save and load player instead...
        if(pCurrChar->getRace() == RACE_NIGHTELF)
            pCurrChar->CastSpell(pCurrChar, 20584, true, 0);// auras SPELL_AURA_INCREASE_SPEED(+speed in wisp form), SPELL_AURA_INCREASE_SWIM_SPEED(+swim speed in wisp form), SPELL_AURA_TRANSFORM (to wisp form)
        pCurrChar->CastSpell(pCurrChar, 8326, true, 0);     // auras SPELL_AURA_GHOST, SPELL_AURA_INCREASE_SPEED(why?), SPELL_AURA_INCREASE_SWIM_SPEED(why?)

        //pCurrChar->SetUInt32Value(UNIT_FIELD_AURA+41, 8326);
        //pCurrChar->SetUInt32Value(UNIT_FIELD_AURA+42, 20584);
        //pCurrChar->SetUInt32Value(UNIT_FIELD_AURAFLAGS+6, 238);
        //pCurrChar->SetUInt32Value(UNIT_FIELD_AURALEVELS+11, 514);
        //pCurrChar->SetUInt32Value(UNIT_FIELD_AURAAPPLICATIONS+11, 65535);
        //pCurrChar->SetUInt32Value(UNIT_FIELD_DISPLAYID, 1825);
        //if (pCurrChar->getRace() == RACE_NIGHTELF)
        //{
        //    pCurrChar->SetSpeed(MOVE_RUN,  1.5f*1.2f, true);
        //    pCurrChar->SetSpeed(MOVE_SWIM, 1.5f*1.2f, true);
        //}
        //else
        //{
        //    pCurrChar->SetSpeed(MOVE_RUN,  1.5f, true);
        //    pCurrChar->SetSpeed(MOVE_SWIM, 1.5f, true);
        //}
        pCurrChar->SetMovement(MOVE_WATER_WALK);
    }

    if(uint32 sourceNode = pCurrChar->m_taxi.GetTaxiSource())
    {

        sLog.outDebug( "WORLD: Restart character %u taxi flight", pCurrChar->GetGUIDLow() );

        uint32 MountId = objmgr.GetTaxiMount(sourceNode, pCurrChar->GetTeam());
        uint32 path = pCurrChar->m_taxi.GetCurrentTaxiPath();

        // search appropriate start path node
        uint32 startNode = 0;

        TaxiPathNodeList const& nodeList = sTaxiPathNodesByPath[path];

        float distPrev = MAP_SIZE*MAP_SIZE;
        float distNext =
            (nodeList[0].x-pCurrChar->GetPositionX())*(nodeList[0].x-pCurrChar->GetPositionX())+
            (nodeList[0].y-pCurrChar->GetPositionY())*(nodeList[0].y-pCurrChar->GetPositionY())+
            (nodeList[0].z-pCurrChar->GetPositionZ())*(nodeList[0].z-pCurrChar->GetPositionZ());

        for(uint32 i = 1; i < nodeList.size(); ++i)
        {
            TaxiPathNode const& node = nodeList[i];
            TaxiPathNode const& prevNode = nodeList[i-1];

            // skip nodes at another map
            if(node.mapid != pCurrChar->GetMapId())
                continue;

            distPrev = distNext;

            distNext =
                (node.x-pCurrChar->GetPositionX())*(node.x-pCurrChar->GetPositionX())+
                (node.y-pCurrChar->GetPositionY())*(node.y-pCurrChar->GetPositionY())+
                (node.z-pCurrChar->GetPositionZ())*(node.z-pCurrChar->GetPositionZ());

            float distNodes =
                (node.x-prevNode.x)*(node.x-prevNode.x)+
                (node.y-prevNode.y)*(node.y-prevNode.y)+
                (node.z-prevNode.z)*(node.z-prevNode.z);

            if(distNext + distPrev < distNodes)
            {
                startNode = i;
                break;
            }
        }

        SendDoFlight( MountId, path, startNode );
    }

    // Load pet if any and player is alive and not in taxi flight
    if(pCurrChar->isAlive() && pCurrChar->m_taxi.GetTaxiSource()==0)
        pCurrChar->LoadPet();

    // Set FFA PvP for non GM in non-rest mode
    if(sWorld.IsFFAPvPRealm() && !pCurrChar->isGameMaster() && !pCurrChar->HasFlag(PLAYER_FLAGS,PLAYER_FLAGS_RESTING) )
        pCurrChar->SetFlag(PLAYER_FLAGS,PLAYER_FLAGS_FFA_PVP);

    // Apply at_login requests
    if(pCurrChar->HasAtLoginFlag(AT_LOGIN_RESET_SPELLS))
    {
        pCurrChar->resetSpells();
        SendNotification("Spells has been reset.");
    }

    if(pCurrChar->HasAtLoginFlag(AT_LOGIN_RESET_TALENTS))
    {
        pCurrChar->resetTalents(true);
        SendNotification("Talents has been reset.");
    }

    // show time before shutdown if shutdown planned.
    if(sWorld.IsShutdowning())
        sWorld.ShutdownMsg(true,pCurrChar);

    if(pCurrChar->isGameMaster())
        SendNotification("GM mode is ON");

    std::string IP_str = _socket ? _socket->GetRemoteAddress().c_str() : "-";
    sLog.outChar("Account: %d (IP: %s) Login Character:[%s] (guid:%u)",GetAccountId(),IP_str.c_str(),pCurrChar->GetName() ,pCurrChar->GetGUID());

    m_playerLoading = false;
    delete holder;
}

void WorldSession::HandleSetFactionAtWar( WorldPacket & recv_data )
{
    CHECK_PACKET_SIZE(recv_data,4+1);

    DEBUG_LOG( "WORLD: Received CMSG_SET_FACTION_ATWAR" );

    uint32 repListID;
    uint8  flag;

    recv_data >> repListID;
    recv_data >> flag;

    FactionStateList::iterator itr = GetPlayer()->m_factions.find(repListID);
    if (itr == GetPlayer()->m_factions.end())
        return;

    // always invisible or hidden faction can't change war state
    if(itr->second.Flags & (FACTION_FLAG_INVISIBLE_FORCED|FACTION_FLAG_HIDDEN) )
        return;

    GetPlayer()->SetFactionAtWar(&itr->second,flag);
}

//I think this function is never used :/ I dunno, but i guess this opcode not exists
void WorldSession::HandleSetFactionCheat( WorldPacket & /*recv_data*/ )
{
    //CHECK_PACKET_SIZE(recv_data,4+4);

    //sLog.outDebug("WORLD SESSION: HandleSetFactionCheat");
    /*
        uint32 FactionID;
        uint32 Standing;

        recv_data >> FactionID;
        recv_data >> Standing;

        std::list<struct Factions>::iterator itr;

        for(itr = GetPlayer()->factions.begin(); itr != GetPlayer()->factions.end(); ++itr)
        {
            if(itr->ReputationListID == FactionID)
            {
                itr->Standing += Standing;
                itr->Flags = (itr->Flags | 1);
                break;
            }
        }
    */
    GetPlayer()->UpdateReputation();
}

void WorldSession::HandleMeetingStoneInfo( WorldPacket & /*recv_data*/ )
{
    DEBUG_LOG( "WORLD: Received CMSG_MEETING_STONE_INFO" );

    WorldPacket data(SMSG_MEETINGSTONE_SETQUEUE, 5);
    data << uint32(0) << uint8(6);
    SendPacket(&data);
}

void WorldSession::HandleTutorialFlag( WorldPacket & recv_data )
{
    CHECK_PACKET_SIZE(recv_data,4);

    uint32 iFlag;
    recv_data >> iFlag;

    uint32 wInt = (iFlag / 32);
    if (wInt >= 8)
    {
        //sLog.outError("CHEATER? Account:[%d] Guid[%u] tried to send wrong CMSG_TUTORIAL_FLAG", GetAccountId(),GetGUID());
        return;
    }
    uint32 rInt = (iFlag % 32);

    uint32 tutflag = GetPlayer()->GetTutorialInt( wInt );
    tutflag |= (1 << rInt);
    GetPlayer()->SetTutorialInt( wInt, tutflag );

    //sLog.outDebug("Received Tutorial Flag Set {%u}.", iFlag);
}

void WorldSession::HandleTutorialClear( WorldPacket & /*recv_data*/ )
{
    for ( uint32 iI = 0; iI < 8; iI++)
        GetPlayer()->SetTutorialInt( iI, 0xFFFFFFFF );
}

void WorldSession::HandleTutorialReset( WorldPacket & /*recv_data*/ )
{
    for ( uint32 iI = 0; iI < 8; iI++)
        GetPlayer()->SetTutorialInt( iI, 0x00000000 );
}

void WorldSession::HandleSetWatchedFactionIndexOpcode(WorldPacket & recv_data)
{
    CHECK_PACKET_SIZE(recv_data,4);

    DEBUG_LOG("WORLD: Received CMSG_SET_WATCHED_FACTION");
    uint32 fact;
    recv_data >> fact;
    GetPlayer()->SetUInt32Value(PLAYER_FIELD_WATCHED_FACTION_INDEX, fact);
}

void WorldSession::HandleSetWatchedFactionInactiveOpcode(WorldPacket & recv_data)
{
    CHECK_PACKET_SIZE(recv_data,4+1);

    DEBUG_LOG("WORLD: Received CMSG_SET_FACTION_INACTIVE");
    uint32 replistid;
    uint8 inactive;
    recv_data >> replistid >> inactive;

    FactionStateList::iterator itr = _player->m_factions.find(replistid);
    if (itr == _player->m_factions.end())
        return;

    _player->SetFactionInactive(&itr->second, inactive);
}

void WorldSession::HandleToggleHelmOpcode( WorldPacket & /*recv_data*/ )
{
    DEBUG_LOG("CMSG_TOGGLE_HELM for %s", _player->GetName());
    _player->ToggleFlag(PLAYER_FLAGS, PLAYER_FLAGS_HIDE_HELM);
}

void WorldSession::HandleToggleCloakOpcode( WorldPacket & /*recv_data*/ )
{
    DEBUG_LOG("CMSG_TOGGLE_CLOAK for %s", _player->GetName());
    _player->ToggleFlag(PLAYER_FLAGS, PLAYER_FLAGS_HIDE_CLOAK);
}

void WorldSession::HandleChangePlayerNameOpcode(WorldPacket& recv_data)
{

    uint64 guid;
    std::string newname;
    std::string oldname;

    CHECK_PACKET_SIZE(recv_data, 8+1);

    recv_data >> guid;
    recv_data >> newname;

    QueryResult *result = CharacterDatabase.PQuery("SELECT at_login FROM characters WHERE guid ='%u'", GUID_LOPART(guid));
    if (result)
    {
        uint32 at_loginFlags;
        Field *fields = result->Fetch();
        at_loginFlags = fields[0].GetUInt32();
        delete result;

        if (!(at_loginFlags & AT_LOGIN_RENAME))
        {
            WorldPacket data(SMSG_CHAR_RENAME, 1);
            data << (uint8)CHAR_CREATE_ERROR;
            SendPacket( &data );
            return;
        }
    }
    else
    {
        WorldPacket data(SMSG_CHAR_RENAME, 1);
        data << (uint8)CHAR_CREATE_ERROR;
        SendPacket( &data );
        return;
    }

    if(!objmgr.GetPlayerNameByGUID(guid, oldname))          // character not exist, because we have no name for this guid
    {
        WorldPacket data(SMSG_CHAR_RENAME, 1);
        data << (uint8)CHAR_LOGIN_NO_CHARACTER;
        SendPacket( &data );
        return;
    }

    // prevent character rename to invalid name
    if(newname.empty())                                     // checked by client
    {
        WorldPacket data(SMSG_CHAR_RENAME, 1);
        data << (uint8)CHAR_NAME_NO_NAME;
        SendPacket( &data );
        return;
    }

    normalizePlayerName(newname);

    if(!ObjectMgr::IsValidName(newname))
    {
        WorldPacket data(SMSG_CHAR_RENAME, 1);
        data << (uint8)CHAR_NAME_INVALID_CHARACTER;
        SendPacket( &data );
        return;
    }

    // check name limitations
    if(GetSecurity() == SEC_PLAYER && objmgr.IsReservedName(newname))
    {
        WorldPacket data(SMSG_CHAR_RENAME, 1);
        data << (uint8)CHAR_NAME_RESERVED;
        SendPacket( &data );
        return;
    }

    if(objmgr.GetPlayerGUIDByName(newname))                 // character with this name already exist
    {
        WorldPacket data(SMSG_CHAR_RENAME, 1);
        data << (uint8)CHAR_CREATE_ERROR;
        SendPacket( &data );
        return;
    }

    if(newname == oldname)                                  // checked by client
    {
        WorldPacket data(SMSG_CHAR_RENAME, 1);
        data << (uint8)CHAR_NAME_FAILURE;
        SendPacket( &data );
        return;
    }

    // we have to check character at_login_flag & AT_LOGIN_RENAME also (fake packets hehe)

    CharacterDatabase.escape_string(newname);
    CharacterDatabase.PExecute("UPDATE characters set name = '%s', at_login = at_login & ~ '%u' WHERE guid ='%u'", newname.c_str(), uint32(AT_LOGIN_RENAME),GUID_LOPART(guid));

    std::string IP_str = _socket ? _socket->GetRemoteAddress().c_str() : "-";
    sLog.outChar("Account: %d (IP: %s) Character:[%s] (guid:%u) Changed name to: %s",GetAccountId(),IP_str.c_str(),oldname.c_str(),GUID_LOPART(guid),newname.c_str());

    WorldPacket data(SMSG_CHAR_RENAME,1+8+(newname.size()+1));
    data << (uint8)RESPONSE_SUCCESS;
    data << guid;
    data << newname;
    SendPacket(&data);
}

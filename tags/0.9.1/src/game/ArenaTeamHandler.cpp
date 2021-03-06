/* 
 * Copyright (C) 2005,2006,2007 MaNGOS <http://www.mangosproject.org/>
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

#include "WorldSession.h"
#include "WorldPacket.h"
#include "Log.h"
#include "Database/DatabaseEnv.h"
#include "Player.h"
#include "ObjectMgr.h"
#include "ArenaTeam.h"
#include "World.h"

void WorldSession::HandleInspectArenaStatsOpcode(WorldPacket & recv_data)
{
    sLog.outDebug("MSG_INSPECT_ARENA_STATS");
    //recv_data.hexlike();

    CHECK_PACKET_SIZE(recv_data, 8);

    uint64 guid;
    recv_data >> guid;
    sLog.outDebug("Inspect Arena stats " I64FMTD, guid);

    Player *plr = objmgr.GetPlayer(guid);
    if(plr)
    {
        for (uint8 i = 0; i < 3; i++)
        {
            uint32 a_id = plr->GetArenaTeamId(i);
            if(a_id)
            {
                ArenaTeam *at = objmgr.GetArenaTeamById(a_id);
                if(at)
                    at->InspectStats(this);
            }
        }
    }
}

void WorldSession::HandleArenaTeamQueryOpcode(WorldPacket & recv_data)
{
    sLog.outDebug( "WORLD: Received CMSG_ARENA_TEAM_QUERY" );
    //recv_data.hexlike();

    CHECK_PACKET_SIZE(recv_data, 4);

    uint32 ArenaTeamId;
    recv_data >> ArenaTeamId;

    ArenaTeam *arenateam = objmgr.GetArenaTeamById(ArenaTeamId);
    if(!arenateam)                                          // arena team not found
        return;

    arenateam->Query(this);
    arenateam->Stats(this);
}

void WorldSession::HandleArenaTeamRosterOpcode(WorldPacket & recv_data)
{
    sLog.outDebug( "WORLD: Received CMSG_ARENA_TEAM_ROSTER" );
    //recv_data.hexlike();

    CHECK_PACKET_SIZE(recv_data, 1);

    uint8 team_slot;                                        // probably team_size or slot
    recv_data >> team_slot;

    ArenaTeam *arenateam = objmgr.GetArenaTeamById(_player->GetArenaTeamId(team_slot));
    if(!arenateam)
        return;

    arenateam->Roster(this);
}

void WorldSession::HandleArenaTeamAddMemberOpcode(WorldPacket & recv_data)
{
    sLog.outDebug("CMSG_ARENA_TEAM_ADD_MEMBER");
    //recv_data.hexlike();

    CHECK_PACKET_SIZE(recv_data, 1+1);

    uint8 team_slot;                                        // slot?
    std::string Invitedname;

    Player * player = NULL;

    recv_data >> team_slot >> Invitedname;

    if(!Invitedname.empty())
    {
        normalizePlayerName(Invitedname);

        player = ObjectAccessor::Instance().FindPlayerByName(Invitedname.c_str());
    }

    if(!player)
    {
        SendArenaTeamCommandResult(ERR_ARENA_TEAM_INVITE_SS, "", Invitedname, ERR_ARENA_TEAM_PLAYER_NOT_FOUND_S);
        return;
    }

    if(player->getLevel() != 70)
    {
        //SendArenaTeamCommandResult(ARENA_TEAM_INVITE_SS,"",Invitedname,ARENA_TEAM_PLAYER_NOT_FOUND_S);
                                                            // can't find related opcode
        SendNotification("%s is not high enough level to join your team", player->GetName());
        return;
    }

    ArenaTeam *arenateam = objmgr.GetArenaTeamById(_player->GetArenaTeamId(team_slot));
    if(!arenateam)
    {
        SendArenaTeamCommandResult(ERR_ARENA_TEAM_CREATE_S, "", "", ERR_ARENA_TEAM_PLAYER_NOT_IN_TEAM);
        return;
    }

    // OK result but not send invite
    if(player->HasInIgnoreList(GetPlayer()->GetGUID()))
        return;

    if (!sWorld.getConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_GUILD) && player->GetTeam() != GetPlayer()->GetTeam())
    {
        SendArenaTeamCommandResult(ERR_ARENA_TEAM_INVITE_SS, "", "", ERR_ARENA_TEAM_NOT_ALLIED);
        return;
    }

    if(player->GetArenaTeamId(team_slot))
    {
        SendArenaTeamCommandResult(ERR_ARENA_TEAM_INVITE_SS, player->GetName(), "", ERR_ALREADY_IN_ARENA_TEAM_S);
        return;
    }

    if(player->GetArenaTeamIdInvited())
    {
        SendArenaTeamCommandResult(ERR_ARENA_TEAM_INVITE_SS, player->GetName(), "", ERR_ALREADY_INVITED_TO_ARENA_TEAM_S);
        return;
    }

    sLog.outDebug("Player %s Invited %s to Join his ArenaTeam", GetPlayer()->GetName(), Invitedname.c_str());

    player->SetArenaTeamIdInvited(GetPlayer()->GetArenaTeamId(team_slot));

    WorldPacket data(SMSG_ARENA_TEAM_INVITE, (8+10));
    data << GetPlayer()->GetName();
    data << arenateam->GetName();
    player->GetSession()->SendPacket(&data);

    sLog.outDebug("WORLD: Sent SMSG_ARENA_TEAM_INVITE");
}

void WorldSession::HandleArenaTeamInviteAcceptOpcode(WorldPacket & /*recv_data*/)
{
    sLog.outDebug("CMSG_ARENA_TEAM_INVITE_ACCEPT");         // empty opcode

    ArenaTeam *at;

    at = objmgr.GetArenaTeamById(_player->GetArenaTeamIdInvited());
    if(!at)
    {
        // arena team not exist
        return;
    }

    if(_player->GetArenaTeamId(at->GetSlot()))
    {
        // already in arena team that size
        return;
    }

    // not let enemies sign petition
    if (!sWorld.getConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_GUILD) && _player->GetTeam() != objmgr.GetPlayerTeamByGUID(at->GetCaptain()))
        return;

    if(!at->AddMember(_player->GetGUID()))
        return;

    // event
    WorldPacket data;
    BuildArenaTeamEventPacket(&data, ERR_ARENA_TEAM_JOIN_SS, 2, _player->GetName(), at->GetName(), "");
    at->BroadcastPacket(&data);
}

void WorldSession::HandleArenaTeamInviteDeclineOpcode(WorldPacket & /*recv_data*/)
{
    sLog.outDebug("CMSG_ARENA_TEAM_INVITE_DECLINE");        // empty opcode

    _player->SetArenaTeamIdInvited(0);                      // no more invited
}

void WorldSession::HandleArenaTeamLeaveOpcode(WorldPacket & recv_data)
{
    sLog.outDebug("CMSG_ARENA_TEAM_LEAVE");
    //recv_data.hexlike();

    CHECK_PACKET_SIZE(recv_data, 1);

    uint8 team_slot;                                        // slot?
    recv_data >> team_slot;

    uint32 at_id = _player->GetArenaTeamId(team_slot);
    if(!at_id)                                              // not in arena team
    {
        // send command result
        return;
    }
    ArenaTeam *at = objmgr.GetArenaTeamById(at_id);
    if(!at)
    {
        // send command result
        return;
    }
    if(_player->GetGUID() == at->GetCaptain() && at->GetMembersSize() > 1)
    {
        // check for correctness
        SendArenaTeamCommandResult(ERR_ARENA_TEAM_QUIT_S, "", "", ERR_ARENA_TEAM_LEADER_LEAVE_S);
        return;
    }
    // arena team has only one member (=captain)
    if(_player->GetGUID() == at->GetCaptain())
    {
        at->Disband(this);
        delete at;
        return;
    }

    at->DelMember(_player->GetGUID());

    // event
    WorldPacket data;
    BuildArenaTeamEventPacket(&data, ERR_ARENA_TEAM_LEAVE_SS, 2, _player->GetName(), at->GetName(), "");
    at->BroadcastPacket(&data);

    //SendArenaTeamCommandResult(ERR_ARENA_TEAM_QUIT_S, at->GetName(), "", 0);
}

void WorldSession::HandleArenaTeamDisbandOpcode(WorldPacket & recv_data)
{
    sLog.outDebug("CMSG_ARENA_TEAM_DISBAND");
    //recv_data.hexlike();

    CHECK_PACKET_SIZE(recv_data, 1);

    uint8 team_slot;                                        // slot?
    recv_data >> team_slot;

    uint32 at_id = _player->GetArenaTeamId(team_slot);
    if(!at_id)
    {
        // arena team id not found
        return;
    }

    ArenaTeam *at = objmgr.GetArenaTeamById(at_id);
    if(!at)
    {
        // arena team not found
        return;
    }

    if(at->GetCaptain() != _player->GetGUID())
    {
        SendArenaTeamCommandResult(ERR_ARENA_TEAM_CREATE_S, "", "", ERR_ARENA_TEAM_PERMISSIONS);
        return;
    }

    at->Disband(this);
    delete at;
}

void WorldSession::HandleArenaTeamRemoveFromTeamOpcode(WorldPacket & recv_data)
{
    sLog.outDebug("CMSG_ARENA_TEAM_REMOVE_FROM_TEAM");
    //recv_data.hexlike();

    CHECK_PACKET_SIZE(recv_data, 1+1);

    uint8 team_slot;
    std::string name;

    recv_data >> team_slot;
    recv_data >> name;

    uint32 at_id = _player->GetArenaTeamId(team_slot);
    if(!at_id)
    {
        // arena team id not found
        return;
    }

    ArenaTeam *at = objmgr.GetArenaTeamById(at_id);
    if(!at)
    {
        // arena team not found
        return;
    }

    uint64 guid = objmgr.GetPlayerGUIDByName(name);
    if(!guid)
    {
        // player guid not found
        return;
    }

    if(at->GetCaptain() == guid)
    {
        // unsure
        SendArenaTeamCommandResult(ERR_ARENA_TEAM_CREATE_S, "", "", ERR_ARENA_TEAM_PERMISSIONS);
        return;
    }

    if(at->GetCaptain() != _player->GetGUID())
    {
        SendArenaTeamCommandResult(ERR_ARENA_TEAM_CREATE_S, "", "", ERR_ARENA_TEAM_PERMISSIONS);
        return;
    }

    if(at->GetCaptain() == guid)
    {
        SendArenaTeamCommandResult(ERR_ARENA_TEAM_CREATE_S, "", "", ERR_ARENA_TEAM_LEADER_LEAVE_S);
        return;
    }

    at->DelMember(guid);

    // event
    WorldPacket data;
    BuildArenaTeamEventPacket(&data, ERR_ARENA_TEAM_REMOVE_SSS, 3, name, at->GetName(), _player->GetName());
    at->BroadcastPacket(&data);
}

void WorldSession::HandleArenaTeamPromoteToCaptainOpcode(WorldPacket & recv_data)
{
    sLog.outDebug("CMSG_ARENA_TEAM_PROMOTE_TO_CAPTAIN");
    //recv_data.hexlike();

    CHECK_PACKET_SIZE(recv_data, 1+1);

    uint8 team_slot;
    std::string name;

    recv_data >> team_slot;
    recv_data >> name;

    uint32 at_id = _player->GetArenaTeamId(team_slot);
    if(!at_id)
    {
        // arena team id not found
        return;
    }

    ArenaTeam *at = objmgr.GetArenaTeamById(at_id);
    if(!at)
    {
        // arena team not found
        return;
    }

    uint64 guid = objmgr.GetPlayerGUIDByName(name);
    if(!guid)
    {
        // player guid not found
        return;
    }

    if(at->GetCaptain() == guid)
    {
        // target player already captain
        return;
    }

    if(at->GetCaptain() != _player->GetGUID())
    {
        SendArenaTeamCommandResult(ERR_ARENA_TEAM_CREATE_S, "", "", ERR_ARENA_TEAM_PERMISSIONS);
        return;
    }

    at->SetCaptain(guid);

    // event
    WorldPacket data;
    BuildArenaTeamEventPacket(&data, ERR_ARENA_TEAM_LEADER_CHANGED_SSS, 3, _player->GetName(), name, at->GetName());
    at->BroadcastPacket(&data);
}

void WorldSession::SendArenaTeamCommandResult(uint32 unk1, std::string str1, std::string str2, uint32 unk3)
{
    WorldPacket data(SMSG_ARENA_TEAM_COMMAND_RESULT, 4+str1.length()+1+str2.length()+1+4);
    data << unk1;
    data << str1;
    data << str2;
    data << unk3;
    SendPacket(&data);
}

void WorldSession::BuildArenaTeamEventPacket(WorldPacket *data, uint8 eventid, uint8 str_count, std::string str1, std::string str2, std::string str3)
{
    data->Initialize(SMSG_ARENA_TEAM_EVENT, 1+1+1);
    *data << eventid;
    *data << str_count;
    switch(str_count)
    {
        case 1:
            *data << str1;
            break;
        case 2:
            *data << str1;
            *data << str2;
            break;
        case 3:
            *data << str1;
            *data << str2;
            *data << str3;
            break;
        default:
            sLog.outError("Unhandled str_count %u in SendArenaTeamEvent()", str_count);
            return;
    }
}

void WorldSession::SendNotInArenaTeamPacket(uint8 type)
{
    WorldPacket data(SMSG_ARENA_NO_TEAM, 4+1);              // 886 - You are not in a %uv%u arena team
    data << uint32(0);                                      // unk(0)
    data << type;                                           // team type (2=2v2,3=3v3,5=5v5), can be used for custom types...
    SendPacket(&data);
}

/*
+ERR_ARENA_NO_TEAM_II "You are not in a %dv%d arena team"

+ERR_ARENA_TEAM_CREATE_S "%s created.  To disband, use /teamdisband [2v2, 3v3, 5v5]."
+ERR_ARENA_TEAM_INVITE_SS "You have invited %s to join %s"
+ERR_ARENA_TEAM_QUIT_S "You are no longer a member of %s"
ERR_ARENA_TEAM_FOUNDER_S "Congratulations, you are a founding member of %s!  To leave, use /teamquit [2v2, 3v3, 5v5]."

+ERR_ARENA_TEAM_INTERNAL "Internal arena team error"
+ERR_ALREADY_IN_ARENA_TEAM "You are already in an arena team of that size"
+ERR_ALREADY_IN_ARENA_TEAM_S "%s is already in an arena team of that size"
+ERR_INVITED_TO_ARENA_TEAM "You have already been invited into an arena team"
+ERR_ALREADY_INVITED_TO_ARENA_TEAM_S "%s has already been invited to an arena team"
+ERR_ARENA_TEAM_NAME_INVALID "That name contains invalid characters, please enter a new name"
+ERR_ARENA_TEAM_NAME_EXISTS_S "There is already an arena team named \"%s\""
+ERR_ARENA_TEAM_LEADER_LEAVE_S "You must promote a new team captain using /teamcaptain before leaving the team"
+ERR_ARENA_TEAM_PERMISSIONS "You don't have permission to do that"
+ERR_ARENA_TEAM_PLAYER_NOT_IN_TEAM "You are not in an arena team of that size"
+ERR_ARENA_TEAM_PLAYER_NOT_IN_TEAM_SS "%s is not in %s"
+ERR_ARENA_TEAM_PLAYER_NOT_FOUND_S "\"%s\" not found"
+ERR_ARENA_TEAM_NOT_ALLIED "You cannot invite players from the opposing alliance"

+ERR_ARENA_TEAM_JOIN_SS "%s has joined %s"
+ERR_ARENA_TEAM_YOU_JOIN_S "You have joined %s.  To leave, use /teamquit [2v2, 3v3, 5v5]."

+ERR_ARENA_TEAM_LEAVE_SS "%s has left %s"

+ERR_ARENA_TEAM_LEADER_IS_SS "%s is the captain of %s"
+ERR_ARENA_TEAM_LEADER_CHANGED_SSS "%s has made %s the new captain of %s"

+ERR_ARENA_TEAM_REMOVE_SSS "%s has been kicked out of %s by %s"

+ERR_ARENA_TEAM_DISBANDED_S "%s has disbanded %s"

ERR_ARENA_TEAM_TARGET_TOO_LOW_S "%s is not high enough level to join your team"

ERR_ARENA_TEAM_TOO_MANY_MEMBERS_S "%s is full"

ERR_ARENA_TEAM_LEVEL_TOO_LOW_I "You must be level %d to form an arena team"
*/

/**
 * =============================================================================
 * CS2Fixes
 * Copyright (C) 2023-2025 Source2ZE
 * =============================================================================
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3.0, as published by the
 * Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "gflbans.h"

#include "adminsystem.h"
#include "commands.h"
#include "ctimer.h"
#include "entity/ccsplayercontroller.h"
#include "entity/cgamerules.h"
#include "entwatch.h"
#include "httpmanager.h"
#include "vendor/nlohmann/json.hpp"
#include <regex>

using json = nlohmann::json;

extern IVEngineServer2* g_pEngineServer2;
extern CGameEntitySystem* g_pEntitySystem;
extern CGlobalVars* GetGlobals();
extern CCSGameRules* g_pGameRules;
extern CAdminSystem* g_pAdminSystem;

GFLBansSystem* g_pGFLBansSystem = nullptr;

static std::map<uint64, std::pair<std::shared_ptr<GFLBans_Report>, CTimerBase*>> mapPendingReports;
void ParseInfraction(const CCommand& args, CCSPlayerController* pAdmin, bool bAdding, InfType infType);
std::string GetActionPhrase(InfType typeInfraction, GrammarTense iTense, bool bAdding = true);

// --- Convars ---
CConVar<CUtlString> g_cvarGFLBansApiUrl("gflbans_api_url", FCVAR_NONE, "URL to interact with GFLBans API. Should end in \"api/\"", "https://bans.gflclan.com/api/");
CConVar<bool> g_cvarGFLBansIssueGlobal("gflbans_issue_global", FCVAR_NONE, "Infractions on the server will be made global if the server admin has permission to set global punishments", true);
CConVar<bool> g_cvarGFLBansAcceptGlobal("gflbans_accept_global", FCVAR_NONE, "Whether globally issued punishments apply to this server or not", true);
CConVar<int> g_cvarFilterGagDuration("gflbans_filtered_gag_duration", FCVAR_NONE, "Minutes to gag a player if they type a filtered message. Gags will only be issued with non-negative values", 60, true, -1, false, 0);
CConVar<int> g_cvarMinRealWorldDuration("gflbans_min_real_world_timed", FCVAR_NONE, "Minimum amount of minutes for a mute/gag duration to be real world timed. 0 forces all punishments to be game timed", 61, true, 0, false, 0);
CConVar<int> g_cvarGFLBansLogLevel("gflbans_log_level", FCVAR_NONE, "GFLBans logging level. This does not affect errors printed to admins when commands fail. 0 = None, 1 = Error (Recommended), 2 = Debug", static_cast<int>(LogLevel::Error), true, static_cast<int>(LogLevel::None), true, static_cast<int>(LogLevel::Debug));

// This only affects the CURRENT report being sent and is not logged in GFLBans. This means that
// if a report was sent 1 minute ago with a cooldown of 600 seconds, but a new report is sent with a
// 20 second cooldown, the new report will be successful. So for an emergency report that you need
// to force through, set this to 1, send the report, then change it back to the original value
CConVar<int> g_cvarGFLBansReportCooldown("gflbans_report_cooldown", FCVAR_NONE, "Minimum amount of seconds between c_report/c_calladmin usages. Minimum of 1 second", 600, true, 1, false, 0);

void UpdateGFLBansAuth(CConVar<CUtlString>* cvar, CSplitScreenSlot slot, const CUtlString* new_val, const CUtlString* old_val);
CConVar<CUtlString> g_cvarGFLBansServerID("gflbans_server_id", FCVAR_NONE, "GFLBans ID for the server", "999", &UpdateGFLBansAuth);
CConVar<CUtlString> g_cvarGFLBansServerKey("gflbans_server_key", FCVAR_PROTECTED, "GFLBans KEY for the server. DO NOT LEAK THIS", "1337", &UpdateGFLBansAuth);

// clang-format off
static std::vector<HTTPHeader>* g_rghdGFLBansAuth = new std::vector<HTTPHeader>
{
	HTTPHeader("Authorization", "SERVER " + std::string(g_cvarGFLBansServerID.Get().String())
		                        + " " + std::string(g_cvarGFLBansServerKey.Get().String()))
};
// clang-format on

void UpdateGFLBansAuth(CConVar<CUtlString>* cvar, CSplitScreenSlot slot, const CUtlString* new_val, const CUtlString* old_val)
{
	g_rghdGFLBansAuth->clear();
	g_rghdGFLBansAuth->push_back(
		HTTPHeader("Authorization", "SERVER " + std::string(g_cvarGFLBansServerID.Get().String()) + " "
										+ std::string(g_cvarGFLBansServerKey.Get().String())));
}

static std::regex g_regChatFilter("n+i+g+e+r+", std::regex_constants::ECMAScript | std::regex_constants::icase);
void UpdateFilterRegex(CConVar<CUtlString>* cvar, CSplitScreenSlot slot, const CUtlString* new_val, const CUtlString* old_val)
{
	g_regChatFilter = std::regex(cvar->Get().String(), std::regex_constants::ECMAScript | std::regex_constants::icase);
}
CConVar<CUtlString> g_cvarChatFilter("gflbans_filter_regex", FCVAR_PROTECTED, "The basic_regex (case insensitive) to delete any chat messages containing a match and punish the player that typed them. Invalid regex strings will crash the server", "n+i+g+e+r+", &UpdateFilterRegex);

static std::regex g_regChatFilterNoPunish("nigga", std::regex_constants::ECMAScript | std::regex_constants::icase);
void UpdateFilterNoPunishRegex(CConVar<CUtlString>* cvar, CSplitScreenSlot slot, const CUtlString* new_val, const CUtlString* old_val)
{
	g_regChatFilterNoPunish = std::regex(cvar->Get().String(), std::regex_constants::ECMAScript | std::regex_constants::icase);
}
CConVar<CUtlString> g_cvarChatFilterNoPunish("gflbans_filter_no_punish_regex", FCVAR_PROTECTED, "The basic_regex (case insensitive) to delete any chat messages containing a match. Invalid regex strings will crash the server", "nigga", &UpdateFilterNoPunishRegex);

// --- Helper/Utility Functions ---
void PrintGFLBansError(CCSPlayerController* pCaller, HTTPRequestHandle request, EHTTPStatusCode eStatusCode, json response)
{
	std::string strErrorDetail = response.value("detail", "");
	if (strErrorDetail.length() > 0)
		ClientPrint(pCaller, HUD_PRINTTALK, GFLBANS_PREFIX "Error code %i: %s", int(eStatusCode), strErrorDetail.c_str());
	else if (eStatusCode == 0)
		ClientPrint(pCaller, HUD_PRINTTALK, GFLBANS_PREFIX "GFLBans is currently not responding");
	else
		ClientPrint(pCaller, HUD_PRINTTALK, GFLBANS_PREFIX "Error code %i", int(eStatusCode));
}

void LogGFLBansError(std::string strName, HTTPRequestHandle request, EHTTPStatusCode eStatusCode, json response)
{
	if (g_cvarGFLBansLogLevel.Get() < static_cast<int>(LogLevel::Error))
		return;

	std::string strErrorDetail = "";
	if (!response.is_discarded() && !response.empty())
		strErrorDetail = response.value("detail", "");
	if (strErrorDetail.length() > 0)
		Message("GFLBans %s Error Code %i: %s\n", strName.c_str(), int(eStatusCode), strErrorDetail.c_str());
	else if (eStatusCode == 0)
		Message("GFLBans %s: GFLBans is currently not responding\n", strName.c_str());
	else
		Message("GFLBans %s: Error Code %i\n", strName.c_str(), int(eStatusCode));
}

void EchoMessage(CCSPlayerController* pAdmin, CCSPlayerController* pTarget, const char* pszPunishment, EchoType echo)
{
	const char* pszAdminName = pAdmin ? pAdmin->GetPlayerName() : CONSOLE_NAME;

	switch (echo)
	{
		case EchoType::All:
			ClientPrintAll(HUD_PRINTTALK, GFLBANS_PREFIX ADMIN_PREFIX "%s.", pszAdminName, pszPunishment);
			break;
		case EchoType::Admin:
			ClientPrint(pAdmin, HUD_PRINTTALK, GFLBANS_PREFIX ADMIN_PREFIX "%s.", pszAdminName, pszPunishment);
			break;
		case EchoType::Target:
			ClientPrint(pTarget, HUD_PRINTTALK, GFLBANS_PREFIX ADMIN_PREFIX "%s.", pszAdminName, pszPunishment);
			break;
		case EchoType::Console:
			ClientPrint(nullptr, HUD_PRINTTALK, GFLBANS_PREFIX ADMIN_PREFIX "%s.", pszAdminName, pszPunishment);
			break;
	}
}

// Returns a string matching the type of punishment and grammar tense specified
std::string GetActionPhrase(InfType typeInfraction, GrammarTense iTense, bool bAdding)
{
	switch (iTense)
	{
		case GrammarTense::PresentOrNoun:
			switch (typeInfraction)
			{
				case InfType::Ban:
					return bAdding ? "ban" : "unban";
				case InfType::Mute:
					return bAdding ? "mute" : "unmute";
				case InfType::Gag:
					return bAdding ? "gag" : "ungag";
				case InfType::Silence:
					return bAdding ? "silence" : "unsilence";
				case InfType::AdminChatGag:
					return bAdding ? "admin chat gag" : "admin chat ungag";
				case InfType::CallAdminBlock:
					return bAdding ? "call admin ban" : "call admin unban";
				case InfType::ItemBlock:
					return bAdding ? "item restriction" : "item unrestriction";
				case InfType::Warn:
					return bAdding ? "warning" : "un-warning";
				default:
					return bAdding ? "punishment" : "punishment removal";
			}
		case GrammarTense::Past:
			switch (typeInfraction)
			{
				case InfType::Ban:
					return bAdding ? "banned" : "unbanned";
				case InfType::Mute:
					return bAdding ? "muted" : "unmuted";
				case InfType::Gag:
					return bAdding ? "gagged" : "ungagged";
				case InfType::Silence:
					return bAdding ? "silenced" : "unsilenced";
				case InfType::AdminChatGag:
					return bAdding ? "admin chat gagged" : "admin chat ungagged";
				case InfType::CallAdminBlock:
					return bAdding ? "call admin banned" : "call admin unbanned";
				case InfType::ItemBlock:
					return bAdding ? "item restricted" : "item unrestricted";
				case InfType::Warn:
					return bAdding ? "warned" : "un-warned";
				default:
					return bAdding ? "punished" : "un-punishment";
			}
		case GrammarTense::Continuous:
			switch (typeInfraction)
			{
				case InfType::Ban:
					return bAdding ? "banning" : "unbanning";
				case InfType::Mute:
					return bAdding ? "muting" : "unmuting";
				case InfType::Gag:
					return bAdding ? "gagging" : "ungagging";
				case InfType::Silence:
					return bAdding ? "silencing" : "unsilencing";
				case InfType::AdminChatGag:
					return bAdding ? "admin chat gagging" : "admin chat ungagging";
				case InfType::CallAdminBlock:
					return bAdding ? "call admin banning" : "call admin unbanning";
				case InfType::ItemBlock:
					return bAdding ? "item restricting" : "item unrestricting";
				case InfType::Warn:
					return bAdding ? "warning" : "un-warning";
				default:
					return bAdding ? "punishing" : "un-punishing";
			}
		case GrammarTense::Command:
			switch (typeInfraction)
			{
				case InfType::Ban:
					return bAdding ? "ban" : "unban";
				case InfType::Mute:
					return bAdding ? "mute" : "unmute";
				case InfType::Gag:
					return bAdding ? "gag" : "ungag";
				case InfType::Silence:
					return bAdding ? "silence" : "unsilence";
				case InfType::AdminChatGag:
					return bAdding ? "agag" : "unagag";
				case InfType::CallAdminBlock:
					return bAdding ? "callban" : "uncallban";
				case InfType::ItemBlock:
					return bAdding ? "restrict" : "unrestrict";
				case InfType::Warn:
					return bAdding ? "warn" : "unwarn";
				default:
					return bAdding ? "punish" : "unpunish";
			}
	}
	return "Unimplemented punishment tense";
}

// Converts an InfType to a GFLBans infraction string
std::string LocalToWebInfraction(InfType typeInfraction)
{
	switch (typeInfraction)
	{
		case InfType::Mute:
			return "voice_block";
		case InfType::Gag:
			return "chat_block";
		case InfType::Ban:
			return "ban";
		case InfType::AdminChatGag:
			return "admin_chat_block";
		case InfType::CallAdminBlock:
			return "call_admin_block";
		case InfType::ItemBlock:
			return "item_block";
		case InfType::Silence:
			return "silence"; // Since Silence is both voice_block and chat_block, this needs to be fixed outside of the function
	}
	return "";
}

// Does a VERY basic check to make sure IP is kind of valid and not local
bool IsValidIP(std::string strIP)
{
	return strIP.length() > 6 && strIP != "127.0.0.1" && std::find_if(strIP.begin(), strIP.end(), [](unsigned char c) { return !std::isdigit(c) && c != '.'; }) == strIP.end();
}

// Returns json player objects for use in GFLBans Queries
json PlayerObj(ZEPlayer* zpPlayer, bool bUseIP)
{
	if (!zpPlayer)
		return json();

	json jRequestBody;
	jRequestBody["gs_service"] = "steam";

	if (zpPlayer->IsFakeClient())
		jRequestBody["gs_id"] = "BOT";
	else
		jRequestBody["gs_id"] = std::to_string(zpPlayer->IsAuthenticated() ? zpPlayer->GetSteamId64() : zpPlayer->GetUnauthenticatedSteamId64());

	if (bUseIP)
	{
		std::string strIP = zpPlayer->GetIpAddress();
		if (IsValidIP(strIP))
			jRequestBody["ip"] = strIP;
	}

	return jRequestBody;
}

// Returns json player objects for use in GFLBans Queries
json PlayerObj(CCSPlayerController* pPlayer, bool bUseIP)
{
	if (!pPlayer)
		return json();

	return PlayerObj(pPlayer->GetZEPlayer(), bUseIP);
}

// Returns json player objects for use in GFLBans Queries
json PlayerObj(CHandle<CCSPlayerController> hPlayer, bool bUseIP)
{
	if (!hPlayer)
		return json();

	return PlayerObj(hPlayer.Get(), bUseIP);
}

// Returns a URL to query zpPlayer's current GFLBans infractions
std::string PlayerQuery(ZEPlayer* zpPlayer, bool bUseIP)
{
	if (zpPlayer->IsFakeClient())
		return "";

	std::string strURL = g_cvarGFLBansApiUrl.Get().String();
	strURL.append("infractions/check?gs_service=steam&gs_id=");
	strURL.append(std::to_string(zpPlayer->IsAuthenticated() ? zpPlayer->GetSteamId64() : zpPlayer->GetUnauthenticatedSteamId64()));
	strURL.append(g_cvarGFLBansAcceptGlobal.Get() ? "&include_other_servers=true" : "&include_other_servers=false");

	if (bUseIP)
	{
		std::string strIP = zpPlayer->GetIpAddress();
		if (IsValidIP(strIP))
			strURL.append("&ip=" + strIP);
	}

	if (g_cvarGFLBansLogLevel.Get() == static_cast<int>(LogLevel::Debug))
		Message(("Request URL: " + strURL + "\n").c_str());

	return strURL;
}

// If bApplyBlock, will attempt to apply block on the server if json contains one
// Return Values are based on if jAllBlockInfo contains a block, not if it was applied
bool CheckJSONForBlock(ZEPlayer* zpPlayer, json jAllBlockInfo, InfType blockType,
					   bool bApplyBlock = true, bool bRemoveSession = true)
{
	if (!zpPlayer || zpPlayer->IsFakeClient())
		return false;

	if (LocalToWebInfraction(blockType).length() == 0)
		return false; // Unimplemented or Call Admin Blocks

	json jBlockInfo = jAllBlockInfo.value(LocalToWebInfraction(blockType), json());
	if (jBlockInfo.empty())
	{
		if (zpPlayer->IsAuthenticated())
		{
			switch (blockType)
			{
				case InfType::Mute:
					g_pAdminSystem->FindAndRemoveInfraction(zpPlayer, CInfractionBase::EInfractionType::Mute, bRemoveSession);
					break;
				case InfType::Gag:
					g_pAdminSystem->FindAndRemoveInfraction(zpPlayer, CInfractionBase::EInfractionType::Gag, bRemoveSession);
					break;
				case InfType::Ban:
					g_pAdminSystem->FindAndRemoveInfraction(zpPlayer, CInfractionBase::EInfractionType::Ban, bRemoveSession);
					break;
				case InfType::AdminChatGag:
					g_pAdminSystem->FindAndRemoveInfraction(zpPlayer, CInfractionBase::EInfractionType::AdminChatGag, bRemoveSession);
					break;
				case InfType::ItemBlock:
					g_pAdminSystem->FindAndRemoveInfraction(zpPlayer, CInfractionBase::EInfractionType::Eban, bRemoveSession);
					break;
			}
		}
		return false;
	}

	if (bApplyBlock)
	{
		time_t iDuration = jBlockInfo.value("expiration", std::time(nullptr)) - std::time(nullptr);
		iDuration = static_cast<time_t>(std::ceil(iDuration / 60.0));

		uint64 iSteamID = zpPlayer->IsAuthenticated() ? zpPlayer->GetSteamId64() : zpPlayer->GetUnauthenticatedSteamId64();

		CInfractionBase* infraction;
		switch (blockType)
		{
			case InfType::Mute:
				infraction = new CMuteInfraction(iDuration, iSteamID);
				break;
			case InfType::Gag:
				infraction = new CGagInfraction(iDuration, iSteamID);
				break;
			case InfType::Ban:
				infraction = new CBanInfraction(iDuration, iSteamID);
				break;
			case InfType::AdminChatGag:
				infraction = new CAdminChatGagInfraction(iDuration, iSteamID);
				break;
			case InfType::ItemBlock:
				infraction = new CEbanInfraction(iDuration, iSteamID);
				break;
			default:
				return true;
		}

		// Overwrite any existing infractions of the same type and update from web
		// This is in case a current infraction was edited on the web
		g_pAdminSystem->FindAndRemoveInfraction(zpPlayer, infraction->GetType(), bRemoveSession);
		g_pAdminSystem->AddInfraction(infraction);
		infraction->ApplyInfraction(zpPlayer);
	}
	return true;
}

// --- GFLBans Objects + Methods ---
GFLBans_Infraction::GFLBans_Infraction(InfType infType, CHandle<CCSPlayerController> hTarget,
									   std::string strReason, CHandle<CCSPlayerController> hAdmin,
									   int iDuration, bool bOnlineOnly) :
	GFLBans_InfractionBase(infType, hTarget, strReason, hAdmin),
	m_bOnlineOnly(bOnlineOnly)
{
	m_wCreated = std::time(nullptr);
	m_wExpires = m_wCreated + (iDuration * 60);
	m_gisScope = g_cvarGFLBansIssueGlobal.Get() ? Global : Server;
}

json GFLBans_Infraction::CreateInfractionJSON() const
{
	json jRequestBody;
	int iDuration = m_wExpires - m_wCreated;

	// Omit duration for perma
	if (iDuration > 0)
		jRequestBody["duration"] = iDuration;
	else if (IsSession())
		jRequestBody["session"] = true;

	jRequestBody["player"] = PlayerObj(m_hTarget, true);

	// Omit admin for block through CONSOLE
	if (m_hAdmin != nullptr)
	{
		json jAdmin;
		jAdmin["gs_admin"] = PlayerObj(m_hAdmin, false);
		jRequestBody["admin"] = jAdmin;
	}

	jRequestBody["reason"] = m_strReason;

	json jPunishments = json::array();
	if (m_infType == InfType::Silence)
	{
		jPunishments[0] = "voice_block";
		jPunishments[1] = "chat_block";
	}
	else if (m_infType != InfType::Warn)
		jPunishments[0] = LocalToWebInfraction(m_infType);
	jRequestBody["punishments"] = jPunishments;

	switch (m_gisScope)
	{
		case Server:
			jRequestBody["scope"] = "server";
			break;
		case Global:
			jRequestBody["scope"] = "global";
			break;
	}

	if (m_bOnlineOnly && m_infType != InfType::Ban && iDuration > 0)
		jRequestBody["dec_online_only"] = true;

	return jRequestBody;
}

json GFLBans_InfractionRemoval::CreateInfractionJSON() const
{
	json jRequestBody;

	jRequestBody["player"] = PlayerObj(m_hTarget, true);
	jRequestBody["remove_reason"] = m_strReason;
	jRequestBody["include_other_servers"] = g_cvarGFLBansAcceptGlobal.Get();

	// Omit admin for unblock through CONSOLE
	if (m_hAdmin != nullptr)
	{
		json jAdmin;
		jAdmin["gs_admin"] = PlayerObj(m_hAdmin, false);
		jRequestBody["admin"] = jAdmin;
	}

	json jPunishments = json::array();
	if (m_infType == InfType::Silence)
	{
		jPunishments[0] = "voice_block";
		jPunishments[1] = "chat_block";
	}
	else
		jPunishments[0] = LocalToWebInfraction(m_infType);
	jRequestBody["restrict_types"] = jPunishments;

	return jRequestBody;
}

GFLBans_Report::GFLBans_Report(CCSPlayerController* pCaller, std::string strMessage,
							   CCSPlayerController* pBadPerson)
{
	m_jCaller = PlayerObj(pCaller, false);
	m_strCallerName = pCaller ? pCaller->GetPlayerName() : "SYSTEM";
	m_strMessage = strMessage.length() == 0	  ? "No reason provided" :
				   strMessage.length() <= 120 ? strMessage :
												strMessage.substr(0, 120);
	m_jBadPerson = pBadPerson ? PlayerObj(pBadPerson, false) : json();
	m_strBadPersonName = pBadPerson ? pBadPerson->GetPlayerName() : "";
}

json GFLBans_Report::CreateReportJSON() const
{
	json jRequestBody;
	jRequestBody["caller"] = m_jCaller;
	jRequestBody["caller_name"] = m_strCallerName;
	jRequestBody["include_other_servers"] = g_cvarGFLBansAcceptGlobal.Get();
	jRequestBody["message"] = m_strMessage;

	// Omit for 10 minutes
	if (g_cvarGFLBansReportCooldown.Get() != 600)
		jRequestBody["cooldown"] = g_cvarGFLBansReportCooldown.Get();

	// Omit for generic call admin (no target of report)
	if (IsReport())
	{
		jRequestBody["report_target"] = m_jBadPerson;
		jRequestBody["report_target_name"] = m_strBadPersonName;
	}

	return jRequestBody;
}

void GFLBans_Report::CallAdmin(CCSPlayerController* pCaller)
{
	if (!pCaller)
		return;

	// Pass this into callback function, so we know if the response is for a report or calladmin query
	bool bIsReport = IsReport();

	std::string strName = m_strCallerName;
	std::string strTarget = bIsReport ? m_strBadPersonName : "";
	std::string strMessage = m_strMessage;
	CHandle<CCSPlayerController> hCaller = pCaller->GetHandle();

	if (g_cvarGFLBansLogLevel.Get() == static_cast<int>(LogLevel::Debug))
	{
		Message(("Report/CallAdmin Query:\n" + std::string(g_cvarGFLBansApiUrl.Get().String()) + "gs/calladmin/\n").c_str());
		if (g_rghdGFLBansAuth != nullptr)
			for (HTTPHeader header : *(g_rghdGFLBansAuth))
				Message("Header - %s: %s\n", header.GetName(), header.GetValue());
		Message((CreateReportJSON().dump(1) + "\n").c_str());
	}

	g_HTTPManager.Post(
		(std::string(g_cvarGFLBansApiUrl.Get().String()) + "gs/calladmin/").c_str(),
		CreateReportJSON().dump().c_str(),
		[hCaller, bIsReport, strName, strTarget, strMessage](HTTPRequestHandle request, json response) {
			if (g_cvarGFLBansLogLevel.Get() == static_cast<int>(LogLevel::Debug))
				Message(("Report/CallAdmin Response:\n" + response.dump(1) + "\n").c_str());

			CCSPlayerController* pCaller = hCaller ? hCaller.Get() : nullptr;

			if (hCaller && !pCaller)
				return;

			if (!response.value("sent", false))
			{
				if (response.value("is_banned", true))
					ClientPrint(pCaller, HUD_PRINTTALK, GFLBANS_PREFIX "You are banned from using !report and !calladmin. Use\x0E !status\1 for more information.");
				else if (response.value("cooldown", 0) > 0)
					ClientPrint(pCaller, HUD_PRINTTALK, GFLBANS_PREFIX "The /%s command was used recently and is on cooldown for \2%s\1.",
								bIsReport ? "report" : "calladmin", FormatTime(response.value("cooldown", 0)).c_str());
				else
					ClientPrint(pCaller, HUD_PRINTTALK, GFLBANS_PREFIX "Your %s failed to send. Please try again",
								bIsReport ? "report" : "admin call");
			}
			else
			{
				for (int i = 0; i < MAXPLAYERS; i++)
				{
					ZEPlayer* pPlayer = g_playerManager->GetPlayer(i);

					if (pPlayer && pPlayer->IsAdminFlagSet(ADMFLAG_GENERIC))
					{
						if (bIsReport)
							ClientPrint(CCSPlayerController::FromSlot(i), HUD_PRINTTALK, GFLBANS_PREFIX "\x0F%s reported\x02 %s\x0F (reason: \x09%s\x0F).", strName.c_str(), strTarget.c_str(), strMessage.c_str());
						else
							ClientPrint(CCSPlayerController::FromSlot(i), HUD_PRINTTALK, GFLBANS_PREFIX "\x0F%s called an admin (reason: \x09%s\x0F).", strName.c_str(), strMessage.c_str());
					}
				}

				ClientPrint(pCaller, HUD_PRINTTALK, GFLBANS_PREFIX "Your %s was sent. If an admin is available, they will help out as soon as possible.",
							bIsReport ? "report" : "admin request");
			}
		},
		[hCaller](HTTPRequestHandle request, EHTTPStatusCode eStatusCode, json response) {
			if (!hCaller || hCaller.Get())
				PrintGFLBansError(hCaller.Get(), request, eStatusCode, response);
		},
		g_rghdGFLBansAuth);
}

// --- Infraction Logic Functions ---

// Parses a chat command to add or remove an infType of punishment
void ParseInfraction(const CCommand& args, CCSPlayerController* pAdmin, bool bAdding, InfType infType)
{
	if (args.ArgC() < 2 || (bAdding && infType == InfType::Ban && args.ArgC() < 3))
	{
		ClientPrint(pAdmin, HUD_PRINTTALK, GFLBANS_PREFIX "Usage: !%s <name> %s[reason]",
					GetActionPhrase(infType, Command, bAdding).c_str(), bAdding ? "[duration] " : "");
		return;
	}

	int iDuration = bAdding ? ParseTimeInput(args[2], 30) : 0;
	int iNumClients = 0;
	int pSlots[MAXPLAYERS];
	ETargetType nType;

	uint64 iBlockedFlags = NO_RANDOM | NO_BOT | NO_UNAUTHENTICATED;
	if (bAdding)
		iBlockedFlags |= NO_SELF;

	// Only allow multiple targetting for mutes that aren't perma (ie. !mute @all 1) for stopping mass mic spam
	if (infType != InfType::Mute || (bAdding && iDuration == 0))
		iBlockedFlags |= NO_MULTIPLE;

	ETargetError eType = g_playerManager->GetPlayersFromString(pAdmin, args[1], iNumClients, pSlots, iBlockedFlags, nType);

	if (bAdding && iDuration == 0 && (eType == ETargetError::MULTIPLE || eType == ETargetError::RANDOM))
	{
		ClientPrint(pAdmin, HUD_PRINTTALK, CHAT_PREFIX "You may only permanently %s individuals.",
					GetActionPhrase(infType, GrammarTense::PresentOrNoun, bAdding).c_str());
		return;
	}
	else if (eType != ETargetError::NO_ERRORS)
	{
		ClientPrint(pAdmin, HUD_PRINTTALK, CHAT_PREFIX "%s", g_playerManager->GetErrorString(eType, (iNumClients == 0) ? 0 : pSlots[0]).c_str());
		return;
	}

	const char* pszCommandPlayerName = pAdmin ? pAdmin->GetPlayerName() : CONSOLE_NAME;

	// Dont allow session punishments for bans (since we don't want to allow admins map-banning players)
	// or for CallAdminBlocks (since these are handled by GFLBans, not the game server)
	if (bAdding && iDuration < 0 && (infType == InfType::Ban || infType == InfType::CallAdminBlock))
	{
		ClientPrint(pAdmin, HUD_PRINTTALK, GFLBANS_PREFIX "Invalid duration.");
		return;
	}

	std::string strReason = GetReason(args, bAdding ? 2 : 1, true);

	if (iNumClients > 1 && infType == InfType::Mute)
	{
		// Targetting a group of people. Do not log these on GFLBans.
		for (int i = 0; i < iNumClients; i++)
		{
			CCSPlayerController* pTarget = CCSPlayerController::FromSlot(pSlots[i]);

			if (!pTarget)
				continue;

			ZEPlayer* zpTarget = g_playerManager->GetPlayer(pSlots[i]);

			if (zpTarget->IsFakeClient())
				continue;

			if (!bAdding)
			{
				if (g_pAdminSystem->FindAndRemoveInfraction(zpTarget, CInfractionBase::Mute))
					// Prevent players with web mutes from speaking after a mass ummute
					g_pGFLBansSystem->CheckPlayerInfractions(zpTarget);
			}
			else
			{
				// We only allow mass muting for mutes, so don't need to set infraction type here
				CInfractionBase* infraction = new CMuteInfraction(iDuration < 0 ? 0 : iDuration,
																  zpTarget->GetSteamId64(),
																  false, true);

				// We're overwriting the infraction, so remove the previous one first
				g_pAdminSystem->FindAndRemoveInfraction(zpTarget, CInfractionBase::Mute);
				g_pAdminSystem->AddInfraction(infraction);
				infraction->ApplyInfraction(zpTarget);
				if (iDuration > 0)
					// Only run this once since we simply dont need to heartbeat it. We know when the punishment ends
					g_pAdminSystem->RemoveSessionPunishments(iDuration * 60.0 + 1);
			}
		}

		const char* pszCommandPlayerName = pAdmin ? pAdmin->GetPlayerName() : CONSOLE_NAME;
		if (bAdding)
		{
			char szAction[64];
			V_snprintf(szAction, sizeof(szAction), " for %s", FormatTime(iDuration, false).c_str());
			PrintMultiAdminAction(nType, pszCommandPlayerName, "muted", szAction, GFLBANS_PREFIX);
		}
		else
			PrintMultiAdminAction(nType, pszCommandPlayerName, "unmuted", "", GFLBANS_PREFIX);

		return;
	}

	// We should be targetting only a single player from this point on
	CCSPlayerController* pTarget = CCSPlayerController::FromSlot(pSlots[0]);
	ZEPlayer* zpTarget = pTarget->GetZEPlayer();

	if (bAdding)
	{
		bool bOnlineOnly = iDuration > 0 && (g_cvarMinRealWorldDuration.Get() <= 0 || iDuration < g_cvarMinRealWorldDuration.Get() || args[2][0] == '+');
		g_pGFLBansSystem->CreateInfraction(infType, EchoType::All, pAdmin, pTarget, strReason, iDuration, bOnlineOnly);
	}
	else
		g_pGFLBansSystem->RemoveInfraction(infType, EchoType::All, pAdmin, pTarget, strReason);
}

// https://github.com/gflze/GFLBans/wiki#standard-infractions
void GFLBansSystem::CreateInfraction(InfType infType, EchoType echo, CCSPlayerController* pAdmin,
									 CCSPlayerController* pBadPerson, std::string strReason,
									 int iDuration, bool bOnlineOnly, bool bPrintErrorsToAdmin)
{
	if (!pBadPerson)
		return;

	ZEPlayer* plyBadPerson = pBadPerson->GetZEPlayer();
	if (!plyBadPerson || plyBadPerson->IsFakeClient())
	{
		if (bPrintErrorsToAdmin)
			ClientPrint(pAdmin, HUD_PRINTTALK, GFLBANS_PREFIX "The player is not on the server...");
		return;
	}
	else if (!plyBadPerson->IsAuthenticated())
	{
		if (bPrintErrorsToAdmin)
			ClientPrint(pAdmin, HUD_PRINTTALK, GFLBANS_PREFIX "The player is not authenticated, please wait a bit and try again later.");
		return;
	}

	auto infPunishment = std::make_shared<GFLBans_Infraction>(infType, pBadPerson->GetHandle(), strReason,
															  pAdmin ? pAdmin->GetHandle() : nullptr,
															  iDuration, bOnlineOnly);

	if (infPunishment->IsSession())
	{
		// Make sure session punishments are applied. It would be nice to log them, but if GFLBans
		// is down, we still want these to stick.
		CInfractionBase* infraction;
		switch (infPunishment->GetInfractionType())
		{
			case InfType::Mute:
				infraction = new CMuteInfraction(0, plyBadPerson->GetSteamId64(), false, true);
				break;
			case InfType::Gag:
				infraction = new CGagInfraction(0, plyBadPerson->GetSteamId64(), false, true);
				break;
			case InfType::Ban:
				infraction = new CBanInfraction(0, plyBadPerson->GetSteamId64());
				break;
			case InfType::Silence:
				infraction = new CMuteInfraction(0, plyBadPerson->GetSteamId64(), false, true);
				g_pAdminSystem->AddInfraction(infraction);
				infraction->ApplyInfraction(plyBadPerson);
				infraction = new CGagInfraction(0, plyBadPerson->GetSteamId64(), false, true);
				break;
			case InfType::AdminChatGag:
				infraction = new CAdminChatGagInfraction(0, plyBadPerson->GetSteamId64(), false, true);
				break;
			case InfType::ItemBlock:
				infraction = new CEbanInfraction(0, plyBadPerson->GetSteamId64(), false, true);
				break;
			case InfType::CallAdminBlock:
			case InfType::Warn:
				infraction = nullptr;
				break;
			default:
				// This should never be reached, since we it means we are trying to apply an unimplemented block type
				if (bPrintErrorsToAdmin)
					ClientPrint(pAdmin, HUD_PRINTTALK, GFLBANS_PREFIX "Improper block type... Send to a dev with the command used.");
				return;
		}

		std::string strBadPlyName = pBadPerson->GetPlayerName();
		std::string strPunishment = GetActionPhrase(infPunishment->GetInfractionType(), GrammarTense::Past, true) + " " + strBadPlyName + " until the map changes";

		if (infPunishment->GetReason() != "No reason provided")
			strPunishment.append(" (\1reason: \x09" + infPunishment->GetReason() + "\1)");

		EchoMessage(pAdmin, pBadPerson, strPunishment.c_str(), echo);

		if (infraction)
		{
			// We're overwriting the infraction, so remove the previous one first
			g_pAdminSystem->AddInfraction(infraction);
			infraction->ApplyInfraction(plyBadPerson);
		}
	}

	CHandle<CCSPlayerController> hBadPerson = pBadPerson->GetHandle();
	CHandle<CCSPlayerController> hAdmin = pAdmin ? pAdmin->GetHandle() : nullptr;

	if (g_cvarGFLBansLogLevel.Get() == static_cast<int>(LogLevel::Debug))
	{
		Message(("Create Infraction Query:\n" + std::string(g_cvarGFLBansApiUrl.Get().String()) + "infractions/\n").c_str());
		if (g_rghdGFLBansAuth != nullptr)
			for (HTTPHeader header : *g_rghdGFLBansAuth)
				Message("Header - %s: %s\n", header.GetName(), header.GetValue());
		Message((infPunishment->CreateInfractionJSON().dump(1) + "\n").c_str());
	}

	g_HTTPManager.Post(
		(std::string(g_cvarGFLBansApiUrl.Get().String()) + "infractions/").c_str(),
		infPunishment->CreateInfractionJSON().dump().c_str(),
		[infPunishment, hBadPerson, hAdmin, echo, bPrintErrorsToAdmin](HTTPRequestHandle request, json response) {
			if (g_cvarGFLBansLogLevel.Get() == static_cast<int>(LogLevel::Debug))
				Message(("Create Infraction Response:\n" + response.dump(1) + "\n").c_str());

			if (infPunishment->IsSession())
				// Session punishments don't care about response, since GFLBans instantly expires them
				return;

			CCSPlayerController* pBadPerson = hBadPerson.Get();
			CCSPlayerController* pAdmin = hAdmin ? hAdmin.Get() : nullptr;

			if (!pBadPerson || (hAdmin && !pAdmin))
				return;

			ZEPlayer* plyBadPerson = pBadPerson->GetZEPlayer();
			if (!plyBadPerson || plyBadPerson->IsFakeClient() || !plyBadPerson->IsAuthenticated())
			{
				// This should only be hit if the player disconnected in the time between the query
				// being sent and GFLBans responding to the query. Punishment is logged, but nothing to apply.
				if (bPrintErrorsToAdmin)
					ClientPrint(pAdmin, HUD_PRINTTALK, GFLBANS_PREFIX "The player has left the server. Punishment has been logged on GFLBans.");
				return;
			}

			std::time_t iDuration = response.value("time_left", 0);
			if (iDuration == 0)
			{
				iDuration = response.value("expires", 0);
				if (iDuration != 0)
					iDuration -= response.value("created", 0);
			}
			iDuration = static_cast<int>(std::ceil(iDuration / 60.0)); // Convert from seconds to minutes

			CInfractionBase* infraction;
			switch (infPunishment->GetInfractionType())
			{
				case InfType::Mute:
					infraction = new CMuteInfraction(iDuration, plyBadPerson->GetSteamId64());
					break;
				case InfType::Gag:
					infraction = new CGagInfraction(iDuration, plyBadPerson->GetSteamId64());
					break;
				case InfType::Ban:
					infraction = new CBanInfraction(iDuration, plyBadPerson->GetSteamId64());
					break;
				case InfType::Silence:
					infraction = new CMuteInfraction(0, plyBadPerson->GetSteamId64());
					g_pAdminSystem->FindAndRemoveInfraction(plyBadPerson, infraction->GetType(), false);
					g_pAdminSystem->AddInfraction(infraction);
					infraction->ApplyInfraction(plyBadPerson);
					infraction = new CGagInfraction(0, plyBadPerson->GetSteamId64());
					break;
				case InfType::AdminChatGag:
					infraction = new CAdminChatGagInfraction(0, plyBadPerson->GetSteamId64());
					break;
				case InfType::ItemBlock:
					infraction = new CEbanInfraction(0, plyBadPerson->GetSteamId64());
					break;
				case InfType::CallAdminBlock:
				case InfType::Warn:
					infraction = nullptr;
					break;
				default:
					// This should never be reached, since we it means we are trying to apply an unimplemented block type
					if (bPrintErrorsToAdmin)
						ClientPrint(pAdmin, HUD_PRINTTALK, GFLBANS_PREFIX "Improper block type... Send to a dev with the command used.");
					return;
			}
			std::string strPunishment = GetActionPhrase(infPunishment->GetInfractionType(), GrammarTense::Past, true);
			std::string strBadPlyName = pBadPerson->GetPlayerName();

			if (iDuration == 0)
				strPunishment = "\2permanently\1 " + strPunishment + " " + strBadPlyName;
			else
				strPunishment.append(" " + strBadPlyName + " for \2" + FormatTime(iDuration, false) + "\1");

			if (infPunishment->GetReason() != "No reason provided")
				strPunishment.append(" (\1reason: \x09" + infPunishment->GetReason() + "\1)");

			EchoMessage(pAdmin, pBadPerson, strPunishment.c_str(), echo);

			if (infraction)
			{
				// We're overwriting the infraction, so remove the previous one first
				g_pAdminSystem->FindAndRemoveInfraction(plyBadPerson, infraction->GetType(), false);
				g_pAdminSystem->AddInfraction(infraction);
				infraction->ApplyInfraction(plyBadPerson);
			}
		},
		[infPunishment, hBadPerson, hAdmin, echo, bPrintErrorsToAdmin](HTTPRequestHandle request, EHTTPStatusCode eStatusCode, json response) {
			if (eStatusCode == 0 && !infPunishment->IsSession())
			{
				// GFLBans not responding
				CCSPlayerController* pBadPerson = hBadPerson.Get();
				CCSPlayerController* pAdmin = hAdmin ? hAdmin.Get() : nullptr;

				if (!pBadPerson || (hAdmin && !pAdmin))
					return;

				ZEPlayer* plyBadPerson = pBadPerson->GetZEPlayer();
				if (!plyBadPerson || plyBadPerson->IsFakeClient() || !plyBadPerson->IsAuthenticated())
				{
					if (bPrintErrorsToAdmin)
						ClientPrint(pAdmin, HUD_PRINTTALK, GFLBANS_PREFIX "The player has left the server. Punishment has not been logged as GFLBans is currently not responding.");
					return;
				}

				CInfractionBase* infraction;
				switch (infPunishment->GetInfractionType())
				{
					case InfType::Mute:
						infraction = new CMuteInfraction(0, plyBadPerson->GetSteamId64(), false, true);
						break;
					case InfType::Gag:
						infraction = new CGagInfraction(0, plyBadPerson->GetSteamId64(), false, true);
						break;
					case InfType::Ban:
						infraction = new CBanInfraction(0, plyBadPerson->GetSteamId64());
						break;
					case InfType::Silence:
						infraction = new CMuteInfraction(0, plyBadPerson->GetSteamId64(), false, true);
						g_pAdminSystem->AddInfraction(infraction);
						infraction->ApplyInfraction(plyBadPerson);
						infraction = new CGagInfraction(0, plyBadPerson->GetSteamId64(), false, true);
						break;
					case InfType::AdminChatGag:
						infraction = new CAdminChatGagInfraction(0, plyBadPerson->GetSteamId64(), false, true);
						break;
					case InfType::CallAdminBlock:
					case InfType::Warn:
						infraction = nullptr;
						break;
					default:
						// This should never be reached, since we it means we are trying to apply an unimplemented block type
						ClientPrint(pAdmin, HUD_PRINTTALK, GFLBANS_PREFIX "Improper block type... Send to a dev with the command used.");
						return;
				}

				std::string strBadPlyName = pBadPerson->GetPlayerName();
				std::string strPunishment = GetActionPhrase(infPunishment->GetInfractionType(), GrammarTense::Past, true) + " " + strBadPlyName + " until the map changes";

				if (infPunishment->GetReason() != "No reason provided")
					strPunishment.append(" (\1reason: \x09" + infPunishment->GetReason() + "\1)");

				strPunishment.append(" (\2GFLBans is currently not responding\1)");

				EchoMessage(pAdmin, pBadPerson, strPunishment.c_str(), echo);

				if (infraction)
				{
					// We're overwriting the infraction, so remove the previous one first
					g_pAdminSystem->AddInfraction(infraction);
					infraction->ApplyInfraction(plyBadPerson);
				}
			}
			else if (hAdmin && hAdmin.Get() && bPrintErrorsToAdmin)
				PrintGFLBansError(hAdmin.Get(), request, eStatusCode, response);
			else
				LogGFLBansError("CreateInfraction", request, eStatusCode, response);
		},
		g_rghdGFLBansAuth);
}

// https://github.com/gflze/GFLBans/wiki#removing-infractions
void GFLBansSystem::RemoveInfraction(InfType infType, EchoType echo, CCSPlayerController* pAdmin,
									 CCSPlayerController* pGoodPerson, std::string strReason,
									 bool bPrintErrorsToAdmin)
{
	if (!pGoodPerson)
		return;

	ZEPlayer* zpGoodPerson = pGoodPerson->GetZEPlayer();
	if (!zpGoodPerson || zpGoodPerson->IsFakeClient())
	{
		if (bPrintErrorsToAdmin)
			ClientPrint(pAdmin, HUD_PRINTTALK, GFLBANS_PREFIX "The player is not on the server...");
		return;
	}
	else if (!zpGoodPerson->IsAuthenticated())
	{
		if (bPrintErrorsToAdmin)
			ClientPrint(pAdmin, HUD_PRINTTALK, GFLBANS_PREFIX "The player is not authenticated, please wait a bit and try again later.");
		return;
	}

	auto infPunishment = std::make_shared<GFLBans_InfractionRemoval>(infType, pGoodPerson->GetHandle(), strReason,
																	 pAdmin ? pAdmin->GetHandle() : nullptr);

	CHandle<CCSPlayerController> hGoodPerson = pGoodPerson->GetHandle();
	CHandle<CCSPlayerController> hAdmin = pAdmin ? pAdmin->GetHandle() : nullptr;

	if (g_cvarGFLBansLogLevel.Get() == static_cast<int>(LogLevel::Debug))
	{
		Message(("Remove Infraction Query:\n" + std::string(g_cvarGFLBansApiUrl.Get().String()) + "infractions/remove\n").c_str());
		if (g_rghdGFLBansAuth != nullptr)
			for (HTTPHeader header : *g_rghdGFLBansAuth)
				Message("Header - %s: %s\n", header.GetName(), header.GetValue());
		Message((infPunishment->CreateInfractionJSON().dump(1) + "\n").c_str());
	}

	g_HTTPManager.Post(
		(std::string(g_cvarGFLBansApiUrl.Get().String()) + "infractions/remove").c_str(),
		infPunishment->CreateInfractionJSON().dump().c_str(),
		[infPunishment, hGoodPerson, hAdmin, echo, bPrintErrorsToAdmin](HTTPRequestHandle request, json response) {
			if (g_cvarGFLBansLogLevel.Get() == static_cast<int>(LogLevel::Debug))
				Message(("Remove Infraction Response:\n" + response.dump(1) + "\n").c_str());

			CCSPlayerController* pGoodPerson = hGoodPerson.Get();
			CCSPlayerController* pAdmin = hAdmin != nullptr ? hAdmin.Get() : nullptr;

			if (!pGoodPerson || (hAdmin && !pAdmin))
				return;

			ZEPlayer* zpGoodPerson = pGoodPerson->GetZEPlayer();
			if (!zpGoodPerson || zpGoodPerson->IsFakeClient())
			{
				// This should only be hit if the player disconnected in the time between the query being sent
				// and GFLBans responding to the query
				if (bPrintErrorsToAdmin)
					ClientPrint(pAdmin, HUD_PRINTTALK, GFLBANS_PREFIX "The player is not currently on the server. Any blocks have been removed from GFLBans.");
				return;
			}
			// Invalidate local punishments of infraction's type
			CInfractionBase::EInfractionType itypeToRemove = CInfractionBase::EInfractionType::Ban; // Initial type doesn't matter, this is just so intellisense shuts up about it being uninitialized
			bool bRemoveGagAndMute = false;
			bool bIsPunished = false;
			switch (infPunishment->GetInfractionType())
			{
				case InfType::Mute:
					bIsPunished = zpGoodPerson->IsMuted();
					itypeToRemove = CInfractionBase::EInfractionType::Mute;
					break;
				case InfType::Gag:
					bIsPunished = zpGoodPerson->IsGagged();
					itypeToRemove = CInfractionBase::EInfractionType::Gag;
					break;
				case InfType::Ban:
					// This should never be hit, since zpGoodPerson wouldn't be valid (connected) if they were banned
					itypeToRemove = CInfractionBase::EInfractionType::Ban;
					break;
				case InfType::Silence:
					bIsPunished = zpGoodPerson->IsGagged() || zpGoodPerson->IsMuted();
					itypeToRemove = CInfractionBase::EInfractionType::Mute;
					bRemoveGagAndMute = true;
					break;
				case InfType::AdminChatGag:
					bIsPunished = zpGoodPerson->IsAdminChatGagged();
					itypeToRemove = CInfractionBase::EInfractionType::AdminChatGag;
					break;
				case InfType::ItemBlock:
					bIsPunished = zpGoodPerson->IsEbanned();
					itypeToRemove = CInfractionBase::EInfractionType::Eban;
					break;
				case InfType::CallAdminBlock:
				case InfType::Warn:
					bIsPunished = true; // We dont store these locally, so just assume they were punished and now aren't. (/^.^)/
					break;
				default:
					// This should never be reached, since we it means we are trying to apply an unimplemented block type
					if (bPrintErrorsToAdmin)
						ClientPrint(pAdmin, HUD_PRINTTALK, GFLBANS_PREFIX "Improper block type... Send to a dev with the command used.");
					return;
			}

			if (!bIsPunished)
			{
				if (bPrintErrorsToAdmin)
					ClientPrint(pAdmin, HUD_PRINTTALK, GFLBANS_PREFIX "%s is not %s.", pGoodPerson->GetPlayerName(),
								GetActionPhrase(infPunishment->GetInfractionType(), GrammarTense::Past, true).c_str());
				return;
			}

			std::string strAction = GetActionPhrase(infPunishment->GetInfractionType(), GrammarTense::Past, false);
			strAction.append(" ");
			strAction.append(pGoodPerson->GetPlayerName());
			if (infPunishment->GetReason() != "No reason provided")
				strAction.append(" (\1reason: \x09" + infPunishment->GetReason() + "\1)");
			EchoMessage(pAdmin, pGoodPerson, strAction.c_str(), echo);

			if (infPunishment->GetInfractionType() != InfType::CallAdminBlock)
				g_pAdminSystem->RemoveInfractionType(zpGoodPerson, itypeToRemove, bRemoveGagAndMute);
		},
		[infPunishment, hGoodPerson, hAdmin, echo, bPrintErrorsToAdmin](HTTPRequestHandle request, EHTTPStatusCode eStatusCode, json response) {
			if (eStatusCode == 0)
			{
				// Remove punishment on server, but it will be automatically reapplied
				// when GFLBans comes back up if not removed on the web
				CCSPlayerController* pGoodPerson = hGoodPerson.Get();
				CCSPlayerController* pAdmin = hAdmin != nullptr ? hAdmin.Get() : nullptr;

				if (!pGoodPerson || (hAdmin && !pAdmin))
					return;

				ZEPlayer* zpGoodPerson = pGoodPerson->GetZEPlayer();
				if (!zpGoodPerson || zpGoodPerson->IsFakeClient())
				{
					// This should only be hit if the player disconnected in the time between the query and now
					PrintGFLBansError(pAdmin, request, eStatusCode, response);
					return;
				}

				bool bIsPunished = false;
				switch (infPunishment->GetInfractionType())
				{
					case InfType::Mute:
						bIsPunished = zpGoodPerson->IsMuted();
						g_pAdminSystem->RemoveInfractionType(zpGoodPerson, CInfractionBase::EInfractionType::Mute, false);
						break;
					case InfType::Gag:
						bIsPunished = zpGoodPerson->IsGagged();
						g_pAdminSystem->RemoveInfractionType(zpGoodPerson, CInfractionBase::EInfractionType::Gag, false);
						break;
					case InfType::Silence:
						bIsPunished = zpGoodPerson->IsGagged() || zpGoodPerson->IsMuted();
						g_pAdminSystem->RemoveInfractionType(zpGoodPerson, CInfractionBase::EInfractionType::Mute, false);
						g_pAdminSystem->RemoveInfractionType(zpGoodPerson, CInfractionBase::EInfractionType::Gag, false);
						break;
					case InfType::AdminChatGag:
						bIsPunished = zpGoodPerson->IsAdminChatGagged();
						g_pAdminSystem->RemoveInfractionType(zpGoodPerson, CInfractionBase::EInfractionType::AdminChatGag, false);
						break;
					case InfType::ItemBlock:
						bIsPunished = zpGoodPerson->IsEbanned();
						g_pAdminSystem->RemoveInfractionType(zpGoodPerson, CInfractionBase::EInfractionType::Eban, false);
						break;
				}
				if (bPrintErrorsToAdmin)
				{
					if (bIsPunished)
						ClientPrint(pAdmin, HUD_PRINTTALK, GFLBANS_PREFIX "Local block removed, but \2GFLBans is currently down\1. Any web blocks will be reapplied when GFLBans comes back online.");
					else
						ClientPrint(pAdmin, HUD_PRINTTALK, GFLBANS_PREFIX "\2GFLBans is currently down\1, so any blocks have not been removed.");
				}
			}
			else if (hAdmin && hAdmin.Get() && bPrintErrorsToAdmin)
				PrintGFLBansError(hAdmin.Get(), request, eStatusCode, response);
			else
				LogGFLBansError("RemoveInfraction", request, eStatusCode, response);
		},
		g_rghdGFLBansAuth);
}

// --- Commands ---
CON_COMMAND_F(c_reload_infractions, "- Reload infractions to sync with GFLBans", FCVAR_SPONLY | FCVAR_LINKED_CONCOMMAND)
{
	g_pAdminSystem->RemoveAllPunishments();
	for (int i = 0; i < MAXPLAYERS; i++)
	{
		ZEPlayer* pPlayer = g_playerManager->GetPlayer(i);

		if (!pPlayer || pPlayer->IsFakeClient())
			continue;

		pPlayer->CheckInfractions();
	}

	Message("Infractions queries sent to GFLBans\n");
}

CON_COMMAND_CHAT_FLAGS(ban, "<name> <duration> [reason] - Ban a player", ADMFLAG_BAN)
{
	ParseInfraction(args, player, true, InfType::Ban);
}

CON_COMMAND_CHAT_FLAGS(mute, "<name> [(+)duration] [reason] - Mute a player", ADMFLAG_CHAT)
{
	ParseInfraction(args, player, true, InfType::Mute);
}

CON_COMMAND_CHAT_FLAGS(unmute, "<name> [reason] - Unmute a player", ADMFLAG_CHAT)
{
	ParseInfraction(args, player, false, InfType::Mute);
}

CON_COMMAND_CHAT_FLAGS(gag, "<name> [(+)duration] [reason] - Gag a player", ADMFLAG_CHAT)
{
	ParseInfraction(args, player, true, InfType::Gag);
}

CON_COMMAND_CHAT_FLAGS(ungag, "<name> [reason] - Ungag a player", ADMFLAG_CHAT)
{
	ParseInfraction(args, player, false, InfType::Gag);
}

CON_COMMAND_CHAT_FLAGS(silence, "<name> [(+)duration] [reason] - Mute and gag a player", ADMFLAG_CHAT)
{
	ParseInfraction(args, player, true, InfType::Silence);
}

CON_COMMAND_CHAT_FLAGS(unsilence, "<name> [reason] - Unmute and ungag a player", ADMFLAG_CHAT)
{
	ParseInfraction(args, player, false, InfType::Silence);
}

CON_COMMAND_CHAT_FLAGS(agag, "<name> [(+)duration] [reason] - Gag a player from using adminchat", ADMFLAG_CHAT)
{
	ParseInfraction(args, player, true, InfType::AdminChatGag);
}

CON_COMMAND_CHAT_FLAGS(unagag, "<name> [reason] - Ungag a player from using adminchat", ADMFLAG_CHAT)
{
	ParseInfraction(args, player, false, InfType::AdminChatGag);
}

CON_COMMAND_CHAT_FLAGS(aungag, "<name> [reason] - Ungag a player from using adminchat", ADMFLAG_CHAT)
{
	ParseInfraction(args, player, false, InfType::AdminChatGag);
}

CON_COMMAND_CHAT_FLAGS(callban, "<name> <(+)duration> [reason] - Ban a player from using report and calladmin", ADMFLAG_CHAT)
{
	ParseInfraction(args, player, true, InfType::CallAdminBlock);
}

CON_COMMAND_CHAT_FLAGS(uncallban, "<name> [reason] - Unban a player from using report and calladmin", ADMFLAG_CHAT)
{
	ParseInfraction(args, player, false, InfType::CallAdminBlock);
}

CON_COMMAND_CHAT_FLAGS(callunban, "<name> [reason] - Unban a player from using report and calladmin", ADMFLAG_CHAT)
{
	ParseInfraction(args, player, false, InfType::CallAdminBlock);
}

CON_COMMAND_CHAT_FLAGS(eban, "<name> <(+)duration> [reason] - Restrict a player from picking up items", ADMFLAG_BAN)
{
	if (g_cvarEnableEntWatch.Get())
		ParseInfraction(args, player, true, InfType::ItemBlock);
}

CON_COMMAND_CHAT_FLAGS(restrict, "<name> <(+)duration> [reason] - Restrict a player from picking up items", ADMFLAG_BAN)
{
	if (g_cvarEnableEntWatch.Get())
		ParseInfraction(args, player, true, InfType::ItemBlock);
}

CON_COMMAND_CHAT_FLAGS(eunban, "<name> [reason] - Unrestrict a player from picking up items", ADMFLAG_BAN)
{
	if (g_cvarEnableEntWatch.Get())
		ParseInfraction(args, player, false, InfType::ItemBlock);
}

CON_COMMAND_CHAT_FLAGS(uneban, "<name> [reason] - Unrestrict a player from picking up items", ADMFLAG_BAN)
{
	if (g_cvarEnableEntWatch.Get())
		ParseInfraction(args, player, false, InfType::ItemBlock);
}

CON_COMMAND_CHAT_FLAGS(unrestrict, "<name> [reason] - Unrestrict a player from picking up items", ADMFLAG_BAN)
{
	if (g_cvarEnableEntWatch.Get())
		ParseInfraction(args, player, false, InfType::ItemBlock);
}

void HistoryCallback(CCSPlayerController* player, CCSPlayerController* pTarget, InfractionStatisticsReply history)
{
	// player and pTarget are guaranteed to be valid when this is called from
	// g_pGFLBansSystem->GetPunishmentHistory, so don't need to recheck them here

	bool bHasInfraction = false;
	for (InfType i = InfType::Ban; IsValidInfType(i); i++)
	{
		if (history.mapPunishmentCounts[i] > 0)
		{
			bHasInfraction = true;
			break;
		}
	}

	if (!bHasInfraction)
	{
		ClientPrint(player, HUD_PRINTTALK, GFLBANS_PREFIX "%s has no infraction history.", pTarget->GetPlayerName());
		return;
	}

	ClientPrint(player, HUD_PRINTTALK, GFLBANS_PREFIX "Check console for %s's punishment history.", pTarget->GetPlayerName());
	ClientPrint(player, HUD_PRINTCONSOLE, "%s's longest infractions:", pTarget->GetPlayerName());
	for (InfType i = InfType::Ban; IsValidInfType(i); i++)
	{
		if (history.mapPunishmentCounts[i] <= 0)
			continue;
		std::string strDuration = "Session";
		if (history.mapPunishmentLongest[i].has_value())
		{
			if (history.mapPunishmentLongest[i].value() > 0)
				strDuration = FormatTime(history.mapPunishmentLongest[i].value(), true);
			else if (history.mapPunishmentLongest[i].value() == 0)
				strDuration = "Permanent";
		}
		ClientPrint(player, HUD_PRINTCONSOLE, "\t%s: %s",
					GetActionPhrase(i, GrammarTense::PresentOrNoun, true).c_str(),
					strDuration.c_str());
	}
}

CON_COMMAND_CHAT_FLAGS(history, "<name> <reason> - Check a player's infraction history", ADMFLAG_GENERIC)
{
	if (args.ArgC() < 2)
	{
		ClientPrint(player, HUD_PRINTTALK, GFLBANS_PREFIX "Usage: !history <name>");
		return;
	}

	int iNumClients = 0;
	int pSlots[MAXPLAYERS];

	if (!g_playerManager->CanTargetPlayers(player, args[1], iNumClients, pSlots, NO_RANDOM | NO_MULTIPLE | NO_BOT | NO_UNAUTHENTICATED))
		return;

	CCSPlayerController* pTarget = CCSPlayerController::FromSlot(pSlots[0]);
	g_pGFLBansSystem->GetPunishmentStats(player, pTarget, false, &HistoryCallback, GetReason(args, 1, true));
}

CON_COMMAND_CHAT(report, "<name> <reason> - Report a player")
{
	if (args.ArgC() < 3)
	{
		ClientPrint(player, HUD_PRINTTALK, GFLBANS_PREFIX "Usage: /report <name> <reason>");
		return;
	}

	if (!player)
	{
		ClientPrint(player, HUD_PRINTTALK, GFLBANS_PREFIX "A calling player is required by GFLBans, so you may not report through console. Use \"c_info <name>\" to find their information instead.");
		return;
	}

	ZEPlayer* zpPlayer = player->GetZEPlayer();
	if (!zpPlayer->IsAuthenticated())
	{
		ClientPrint(player, HUD_PRINTTALK, GFLBANS_PREFIX "You are not authenticated yet. Please wait a bit and try again.");
		return;
	}

	int iNumClients = 0;
	int pSlots[MAXPLAYERS];

	if (!g_playerManager->CanTargetPlayers(player, args[1], iNumClients, pSlots, NO_RANDOM | NO_MULTIPLE | NO_SELF | NO_BOT | NO_UNAUTHENTICATED | NO_IMMUNITY))
		return;

	CCSPlayerController* pTarget = CCSPlayerController::FromSlot(pSlots[0]);
	ZEPlayer* zpTarget = pTarget->GetZEPlayer();

	std::string strMessage = GetReason(args, 1, true);

	if (strMessage.length() <= 0)
	{
		ClientPrint(player, HUD_PRINTTALK, GFLBANS_PREFIX "You must provide a reason for reporting.");
		return;
	}

	ClientPrint(player, HUD_PRINTTALK, " \7[GFLBans]\x0B Attempting to report \2%s \x0B(reason: \x09%s\x0B)...", pTarget->GetPlayerName(), strMessage.c_str());
	ClientPrint(player, HUD_PRINTTALK, GFLBANS_PREFIX "Type \x0E/confirm\1 within 30 seconds to send your pending report. Issuing false reports will result in a\x02 ban\1.");
	uint64 reportIndex = zpPlayer->GetSteamId64();
	if (mapPendingReports.find(reportIndex) != mapPendingReports.end())
	{
		for (int i = g_timers.Tail(); i != g_timers.InvalidIndex();)
		{
			auto timer = g_timers[i];

			int prevIndex = i;
			i = g_timers.Previous(i);

			// Delete existing timer
			if (timer == mapPendingReports[reportIndex].second)
			{
				delete timer;
				g_timers.Remove(prevIndex);
				break;
			}
		}
	}

	int iCommandPlayerSlot = player->GetPlayerSlot();
	CTimerBase* timer = new CTimer(30.0f, true, true, [reportIndex, iCommandPlayerSlot]() {
		auto player = CCSPlayerController::FromSlot(iCommandPlayerSlot);
		if (mapPendingReports.find(reportIndex) != mapPendingReports.end())
		{
			mapPendingReports.erase(reportIndex);
			if (g_cvarGFLBansLogLevel.Get() == static_cast<int>(LogLevel::Debug))
				Message("Deleted a report/admin call");

			if (!player)
				return -1.0f;

			ZEPlayer* zpPlayer = player->GetZEPlayer();

			if (!zpPlayer || zpPlayer->IsFakeClient())
				return -1.0f;

			if (zpPlayer->IsAuthenticated() ? zpPlayer->GetSteamId64() : zpPlayer->GetUnauthenticatedSteamId64() == reportIndex)
				ClientPrint(player, HUD_PRINTTALK, GFLBANS_PREFIX "Your admin call has been cancelled due to not using \x0E/confirm\1 within 30 seconds.");
		}
		return -1.0f;
	});

	mapPendingReports[reportIndex] =
		std::make_pair(std::make_shared<GFLBans_Report>(player, strMessage, pTarget), timer);
}

CON_COMMAND_CHAT(calladmin, "<reason> - Request for an admin to join the server")
{
	if (args.ArgC() < 2)
	{
		ClientPrint(player, HUD_PRINTTALK, GFLBANS_PREFIX "Usage: /calladmin <reason>");
		return;
	}

	if (!player)
	{
		ClientPrint(player, HUD_PRINTTALK, GFLBANS_PREFIX "A calling player is required by GFLBans, so you may not call admins through console.");
		return;
	}

	ZEPlayer* zpPlayer = player->GetZEPlayer();
	if (!zpPlayer->IsAuthenticated())
	{
		ClientPrint(player, HUD_PRINTTALK, GFLBANS_PREFIX "You are not authenticated yet. Please wait a bit and try again.");
		return;
	}

	std::string strMessage = GetReason(args, 0, true);
	if (strMessage.length() <= 0)
	{
		ClientPrint(player, HUD_PRINTTALK, GFLBANS_PREFIX "You must provide a reason for calling an admin.");
		return;
	}

	ClientPrint(player, HUD_PRINTTALK, " \7[GFLBans]\x0B Attempting to call an admin (reason: \x09%s\x0B)...", strMessage.c_str());
	ClientPrint(player, HUD_PRINTTALK, GFLBANS_PREFIX "Type \x0E/confirm\1 within 30 seconds to send your pending admin call. Abusing this feature will result in a\x02 ban\1.");
	uint64 reportIndex = zpPlayer->GetSteamId64();
	if (mapPendingReports.find(reportIndex) != mapPendingReports.end())
	{
		for (int i = g_timers.Tail(); i != g_timers.InvalidIndex();)
		{
			auto timer = g_timers[i];

			int prevIndex = i;
			i = g_timers.Previous(i);

			// Delete existing timer
			if (timer == mapPendingReports[reportIndex].second)
			{
				delete timer;
				g_timers.Remove(prevIndex);
				break;
			}
		}
	}

	int iCommandPlayerSlot = player->GetPlayerSlot();
	CTimerBase* timer = new CTimer(30.0f, true, true, [reportIndex, iCommandPlayerSlot]() {
		auto player = CCSPlayerController::FromSlot(iCommandPlayerSlot);
		if (mapPendingReports.find(reportIndex) != mapPendingReports.end())
		{
			mapPendingReports.erase(reportIndex);

			if (g_cvarGFLBansLogLevel.Get() == static_cast<int>(LogLevel::Debug))
				Message("Deleted a report/admin call");

			if (!player)
				return -1.0f;

			ZEPlayer* zpPlayer = player->GetZEPlayer();

			if (!zpPlayer || zpPlayer->IsFakeClient())
				return -1.0f;

			if (zpPlayer->IsAuthenticated() ? zpPlayer->GetSteamId64() : zpPlayer->GetUnauthenticatedSteamId64() == reportIndex)
				ClientPrint(player, HUD_PRINTTALK, GFLBANS_PREFIX "Your admin call has been cancelled due to not using \x0E/confirm\1 within 30 seconds.");
		}
		return -1.0f;
	});

	mapPendingReports[reportIndex] =
		std::make_pair(std::make_shared<GFLBans_Report>(player, strMessage), timer);
}

CON_COMMAND_CHAT(confirm, "- Send a report or admin call that you attempted to send within the last 30 seconds")
{
	if (!player)
	{
		ClientPrint(player, HUD_PRINTTALK, GFLBANS_PREFIX "A calling player is required by GFLBans, so reports can not be sent through console.");
		return;
	}

	ZEPlayer* zpPlayer = player->GetZEPlayer();

	if (!zpPlayer->IsAuthenticated())
	{
		ClientPrint(player, HUD_PRINTTALK, GFLBANS_PREFIX "You are not authenticated yet. Please wait a bit and try again.");
		return;
	}

	if (mapPendingReports.find(zpPlayer->GetSteamId64()) == mapPendingReports.end())
	{
		ClientPrint(player, HUD_PRINTTALK, GFLBANS_PREFIX "You do not have any pending reports or admin calls. Please create one with \x02/report\1 or \x02/calladmin\1 before using \x0E/confirm\1");
		return;
	}

	for (int i = g_timers.Tail(); i != g_timers.InvalidIndex();)
	{
		auto timer = g_timers[i];

		int prevIndex = i;
		i = g_timers.Previous(i);

		// Delete existing timer
		if (timer == mapPendingReports[zpPlayer->GetSteamId64()].second)
		{
			delete timer;
			g_timers.Remove(prevIndex);
			break;
		}
	}

	std::shared_ptr<GFLBans_Report> report = mapPendingReports[zpPlayer->GetSteamId64()].first;
	mapPendingReports.erase(zpPlayer->GetSteamId64());
	report->CallAdmin(player);
}

CON_COMMAND_CHAT_FLAGS(claim, "- Claim the most recent GFLBans report/calladmin query", ADMFLAG_KICK)
{
	json jClaim;
	jClaim["admin_name"] = player ? player->GetPlayerName() : "SYSTEM";

	CHandle<CCSPlayerController> hPlayer = player ? player->GetHandle() : nullptr;

	if (g_cvarGFLBansLogLevel.Get() == static_cast<int>(LogLevel::Debug))
	{
		Message(("Claim Query:\n" + std::string(g_cvarGFLBansApiUrl.Get().String()) + "gs/calladmin/claim\n").c_str());
		if (g_rghdGFLBansAuth != nullptr)
			for (HTTPHeader header : *(g_rghdGFLBansAuth))
				Message("Header - %s: %s\n", header.GetName(), header.GetValue());
		Message((jClaim.dump(1) + "\n").c_str());
	}

	g_HTTPManager.Post(
		(std::string(g_cvarGFLBansApiUrl.Get().String()) + "gs/calladmin/claim").c_str(),
		jClaim.dump().c_str(),
		[hPlayer](HTTPRequestHandle request, json response) {
			if (g_cvarGFLBansLogLevel.Get() == static_cast<int>(LogLevel::Debug))
				Message(("Claim Response:\n" + response.dump(1) + "\n").c_str());

			CCSPlayerController* player = hPlayer ? hPlayer.Get() : nullptr;

			if (hPlayer && !player)
				return;

			if (!response.value("success", false))
				if (response.value("msg", "").length() > 0)
					ClientPrint(player, HUD_PRINTTALK, GFLBANS_PREFIX "%s", response.value("msg", "").c_str());
				else
					ClientPrint(player, HUD_PRINTTALK, GFLBANS_PREFIX "Claim request failed. Are you sure there is an open admin call?");
			else if (response.value("msg", "").length() > 0)
				ClientPrint(player, HUD_PRINTTALK, GFLBANS_PREFIX "%s", response.value("msg", "").c_str());
			else
				ClientPrint(player, HUD_PRINTTALK, GFLBANS_PREFIX "Successfully claimed an admin call.");
		},
		[hPlayer](HTTPRequestHandle request, EHTTPStatusCode eStatusCode, json response) {
			if (!hPlayer || hPlayer.Get())
				PrintGFLBansError(hPlayer.Get(), request, eStatusCode, response);
		},
		g_rghdGFLBansAuth);
}

CON_COMMAND_CHAT(status, "<name> - List a player's active punishments. Non-admins may only check their own punishments")
{
	int iNumClients = 0;
	int pSlots[MAXPLAYERS];
	ETargetType nType;
	ZEPlayer* pTargetPlayer = nullptr;
	bool bIsAdmin = !player || player->GetZEPlayer()->IsAdminFlagSet(ADMFLAG_GENERIC);
	std::string strTarget = (!bIsAdmin || args.ArgC() < 2) ? "@me" : args[1];

	if (!g_playerManager->CanTargetPlayers(player, strTarget.c_str(), iNumClients, pSlots, NO_UNAUTHENTICATED | NO_MULTIPLE | NO_BOT, nType))
		return;

	CCSPlayerController* pTarget = CCSPlayerController::FromSlot(pSlots[0]);
	bool bSelfTarget = pTarget == player;
	strTarget = pTarget->GetPlayerName();

	// Send the requests
	std::string strURL = PlayerQuery(pTarget->GetZEPlayer(), true);

	if (strURL.length() == 0)
		return;

	CHandle<CCSPlayerController> hPlayer = player ? player->GetHandle() : nullptr;
	CHandle<CCSPlayerController> hTarget = pTarget->GetHandle();

	g_HTTPManager.Get(
		strURL.c_str(),
		[hPlayer, hTarget, strTarget, bSelfTarget](HTTPRequestHandle request, json response) {
			if (g_cvarGFLBansLogLevel.Get() == static_cast<int>(LogLevel::Debug))
				Message("Status response: %s\n", response.dump().c_str());

			CCSPlayerController* pPlayer = hPlayer ? hPlayer.Get() : nullptr;
			if (hPlayer && !pPlayer)
				return;

			CCSPlayerController* pTarget = hTarget.Get();
			if (!pTarget)
			{
				ClientPrint(pPlayer, HUD_PRINTTALK, GFLBANS_PREFIX "Target not found.");
				return;
			}

			ZEPlayer* zpTarget = pTarget->GetZEPlayer();
			if (!zpTarget)
			{
				ClientPrint(pPlayer, HUD_PRINTTALK, GFLBANS_PREFIX "Target not found.");
				return;
			}

			std::vector<std::string> rgstrPunishments;
			if (zpTarget->IsGagged())
				rgstrPunishments.push_back(GetActionPhrase(InfType::Gag, GrammarTense::Past, true));
			if (zpTarget->IsMuted())
				rgstrPunishments.push_back(GetActionPhrase(InfType::Mute, GrammarTense::Past, true));
			if (zpTarget->IsAdminChatGagged())
				rgstrPunishments.push_back(GetActionPhrase(InfType::AdminChatGag, GrammarTense::Past, true));
			if (zpTarget->IsEbanned())
				rgstrPunishments.push_back(GetActionPhrase(InfType::ItemBlock, GrammarTense::Past, true));
			if (response.contains("call_admin_block"))
				rgstrPunishments.push_back(GetActionPhrase(InfType::CallAdminBlock, GrammarTense::Past, true));

			std::string strPunishment = "";
			if (rgstrPunishments.size() == 1)
				strPunishment = "\2" + rgstrPunishments[0] + "\1";
			else if (rgstrPunishments.size() == 2)
				strPunishment = "\2" + rgstrPunishments[0] + "\1 and\2 " + rgstrPunishments[1] + "\1";
			else if (rgstrPunishments.size() > 2)
			{
				for (int i = 0; i < rgstrPunishments.size() - 1; i++)
					strPunishment.append("\2" + rgstrPunishments[i] + "\1, ");
				strPunishment.append("and\2 " + rgstrPunishments[rgstrPunishments.size() - 1] + "\1");
			}

			if (response.dump().length() < 5)
			{
				if (strPunishment.length() > 0)
					ClientPrint(pPlayer, HUD_PRINTTALK, GFLBANS_PREFIX "%s %s.",
								bSelfTarget ? "You are" : (strTarget + " is").c_str(),
								strPunishment.c_str());
				else
					ClientPrint(pPlayer, HUD_PRINTTALK, GFLBANS_PREFIX "%s no active punishments.",
								bSelfTarget ? "You have" : (strTarget + " has").c_str());
				return;
			}

			if (strPunishment.length() > 0)
				ClientPrint(pPlayer, HUD_PRINTTALK, GFLBANS_PREFIX "%s currently %s. Check console for more information.",
							bSelfTarget ? "You are" : (strTarget + " is").c_str(), strPunishment.c_str());
			else
				ClientPrint(pPlayer, HUD_PRINTTALK, GFLBANS_PREFIX "%s unsynced web punishments. Check console for more information.",
							bSelfTarget ? "You have" : (strTarget + " has").c_str());

			if (bSelfTarget)
				ClientPrint(pPlayer, HUD_PRINTCONSOLE, "[GFLBans] Your active punishments:");
			else
				ClientPrint(pPlayer, HUD_PRINTCONSOLE, "[GFLBans] Active punishments for %s:", (strTarget).c_str());

			// Does not list session punishments, as GFLBans will not return those.
			for (const auto& punishment : response.items())
			{
				time_t wExpiration = punishment.value().value("expiration", -1);
				std::string strPunishmentType;

				if (punishment.key() == "voice_block")
					strPunishmentType = GetActionPhrase(InfType::Mute, GrammarTense::Past, true);
				else if (punishment.key() == "chat_block")
					strPunishmentType = GetActionPhrase(InfType::Gag, GrammarTense::Past, true);
				else if (punishment.key() == "ban")
					strPunishmentType = GetActionPhrase(InfType::Ban, GrammarTense::Past, true);
				else if (punishment.key() == "admin_chat_block")
					strPunishmentType = GetActionPhrase(InfType::AdminChatGag, GrammarTense::Past, true);
				else if (punishment.key() == "call_admin_block")
					strPunishmentType = GetActionPhrase(InfType::CallAdminBlock, GrammarTense::Past, true);
				else if (punishment.key() == "item_block")
					strPunishmentType = GetActionPhrase(InfType::ItemBlock, GrammarTense::Past, true);
				else
					strPunishmentType = punishment.key();

				if (wExpiration <= 0)
					strPunishmentType = "Permanently " + strPunishmentType + ":";
				else
				{
					strPunishmentType.at(0) = std::toupper(strPunishmentType.at(0));
					strPunishmentType.append(" for " + FormatTime(wExpiration - std::time(nullptr)) + ":");
				}

				ClientPrint(pPlayer, HUD_PRINTCONSOLE, strPunishmentType.c_str());

				for (const auto& val : punishment.value().items())
				{
					std::string desc;
					if (val.key() == "expiration")
						continue;
					else if (val.key() == "admin_name")
						desc = "\tAdmin: ";
					else if (val.key() == "reason")
					{
						if (val.value().dump() == "\"No reason provided\"")
							continue;
						desc = "\tReason: ";
					}
					else
						desc = "\t" + val.key() + ": ";
					std::string temp = val.value().dump();
					desc.append(temp.substr(1, temp.length() - 2));
					ClientPrint(pPlayer, HUD_PRINTCONSOLE, desc.c_str());
				}
			}
		},
		[hPlayer](HTTPRequestHandle request, EHTTPStatusCode eStatusCode, json response) {
			if (!hPlayer || hPlayer.Get())
				PrintGFLBansError(hPlayer.Get(), request, eStatusCode, response);
		},
		g_rghdGFLBansAuth);
}

// --- GFLBans Generic Functions ---
bool GFLBansSystem::FilterMessage(CCSPlayerController* pChatter, const CCommand& args)
{
	bool bMatched = std::regex_search(args.GetCommandString(), g_regChatFilter);
	if (bMatched && g_cvarFilterGagDuration.Get() >= 0)
	{
		if (!pChatter)
			return true;

		ZEPlayer* zpChatter = pChatter->GetZEPlayer();

		if (zpChatter->IsFakeClient() || !zpChatter->IsAuthenticated())
			return true;

		g_pGFLBansSystem->CreateInfraction(InfType::Gag, EchoType::Target, nullptr, pChatter, "Filtered chat message", g_cvarFilterGagDuration.Get(), true);
	}

	if (!bMatched)
		bMatched = std::regex_search(args.GetCommandString(), g_regChatFilterNoPunish);

	return bMatched;
}

// https://github.com/gflze/GFLBans/wiki#heartbeat
bool GFLBansSystem::Heartbeat()
{
	if (!GetGlobals() || GetGlobals()->maxClients < 2)
		return false;

	json jHeartbeat;

	ConVarRefAbstract ccvarHostname("hostname");
	if (ccvarHostname.IsValidRef())
		jHeartbeat["hostname"] = ccvarHostname.GetString().String();
	else
		jHeartbeat["hostname"] = "CS2 Server";

	jHeartbeat["max_slots"] = GetGlobals()->maxClients;

	json jPlayers = json::array();
	for (int i = 0; i < GetGlobals()->maxClients; i++)
	{
		ZEPlayer* zpPlayer = g_playerManager->GetPlayer(i);

		if (!zpPlayer || zpPlayer->IsFakeClient())
			continue;

		jPlayers.push_back(PlayerObj(zpPlayer, true));
	}
	jHeartbeat["players"] = jPlayers;

#ifdef _WIN32
	jHeartbeat["operating_system"] = "windows";
#else
	jHeartbeat["operating_system"] = "linux";
#endif

	jHeartbeat["mod"] = "cs2"; // Should this be "cs2" or "csgo"?
	jHeartbeat["map"] = GetGlobals()->mapname.ToCStr();

	ConVarRefAbstract ccvarPassword("sv_password");
	jHeartbeat["locked"] = (ccvarHostname.IsValidRef() && ccvarPassword.GetString().Length() > 0);
	jHeartbeat["include_other_servers"] = g_cvarGFLBansAcceptGlobal.Get();

	if (g_cvarGFLBansLogLevel.Get() == static_cast<int>(LogLevel::Debug))
	{
		if (g_rghdGFLBansAuth != nullptr)
			for (HTTPHeader header : *g_rghdGFLBansAuth)
				Message("Heartbeat Header - %s: %s\n", header.GetName(), header.GetValue());
		Message(("Heartbeat Query:\nURL: " + std::string(g_cvarGFLBansApiUrl.Get().String()) + "gs/heartbeat\nPOST JSON:\n" + jHeartbeat.dump(1) + "\n").c_str());
	}

	g_HTTPManager.Post(
		(std::string(g_cvarGFLBansApiUrl.Get().String()) + "gs/heartbeat").c_str(),
		jHeartbeat.dump().c_str(),
		[](HTTPRequestHandle request, json response) {
			if (g_cvarGFLBansLogLevel.Get() == static_cast<int>(LogLevel::Debug))
				Message(("Heartbeat Response:\n" + response.dump(1) + "\n").c_str());

			for (auto& [key, heartbeatChange] : response.items())
			{
				json jInfractions = heartbeatChange.value("check", json());
				json jPly = heartbeatChange.value("player", json());
				if (jPly.empty() || jPly.value("gs_service", "") != "steam")
					continue;

				std::string strSteamID = jPly.value("gs_id", "");
				if (strSteamID.length() != 17 || std::find_if(strSteamID.begin(), strSteamID.end(), [](unsigned char c) { return !std::isdigit(c); }) != strSteamID.end())
					continue;
				// stoll should be exception safe with above check
				uint64 iSteamID = std::stoll(strSteamID);

				ZEPlayer* zpPlayer = g_playerManager->GetPlayerFromSteamId(iSteamID, true);

				if (!zpPlayer || zpPlayer->IsFakeClient())
					continue;

				CCSPlayerController* pPlayer = CCSPlayerController::FromSlot(zpPlayer->GetPlayerSlot());

				bool bWasPunished = zpPlayer->IsMuted();
				if (!CheckJSONForBlock(zpPlayer, jInfractions, InfType::Mute, true, false)
					&& bWasPunished && !zpPlayer->IsMuted())
					ClientPrint(pPlayer, HUD_PRINTTALK, GFLBANS_PREFIX "You are no longer %s. You may talk again.",
								GetActionPhrase(InfType::Mute, GrammarTense::Past, true).c_str());

				bWasPunished = zpPlayer->IsGagged();
				if (!CheckJSONForBlock(zpPlayer, jInfractions, InfType::Gag, true, false)
					&& bWasPunished && !zpPlayer->IsGagged())
					ClientPrint(pPlayer, HUD_PRINTTALK, GFLBANS_PREFIX "You are no longer %s. You may type in chat again.",
								GetActionPhrase(InfType::Gag, GrammarTense::Past, true).c_str());

				bWasPunished = zpPlayer->IsAdminChatGagged();
				if (!CheckJSONForBlock(zpPlayer, jInfractions, InfType::AdminChatGag, true, false)
					&& bWasPunished && !zpPlayer->IsAdminChatGagged())
					ClientPrint(pPlayer, HUD_PRINTTALK, GFLBANS_PREFIX "You are no longer %s. You may type in admin chat again.",
								GetActionPhrase(InfType::AdminChatGag, GrammarTense::Past, true).c_str());

				bWasPunished = zpPlayer->IsEbanned();
				if (!CheckJSONForBlock(zpPlayer, jInfractions, InfType::ItemBlock, true, false)
					&& bWasPunished && !zpPlayer->IsEbanned())
					ClientPrint(pPlayer, HUD_PRINTTALK, GFLBANS_PREFIX "You are no longer %s. You may pick up map items again.",
								GetActionPhrase(InfType::ItemBlock, GrammarTense::Past, true).c_str());
				// We dont need to check to check for or apply a Call Admin Block, since that is all handled by GFLBans itself

				// Ban should be checked last, since it could make zpPlayer point at garbage
				CheckJSONForBlock(zpPlayer, jInfractions, InfType::Ban);
			}
		},
		[](HTTPRequestHandle request, EHTTPStatusCode eStatusCode, json response) {
			LogGFLBansError("Heartbeat", request, eStatusCode, response);
		},
		g_rghdGFLBansAuth);
	return true;
}

// https://github.com/gflze/GFLBans/wiki#checking-player-infractions
void GFLBansSystem::CheckPlayerInfractions(ZEPlayer* zpPlayer)
{
	if (zpPlayer == nullptr || zpPlayer->IsFakeClient())
		return;

	// Check against current infractions on the server and remove expired ones
	g_pAdminSystem->ApplyInfractions(zpPlayer);

	// We dont care if zpPlayer is authenticated or not at this point.
	// We are just fetching active punishments rather than applying anything new, so innocents
	// cannot be hurt by someone faking a steamid here
	std::string strURL = PlayerQuery(zpPlayer, true);
	if (strURL.length() == 0)
		return;
	ZEPlayerHandle zphPlayer = zpPlayer->GetHandle();

	g_HTTPManager.Get(
		strURL.c_str(),
		[zphPlayer](HTTPRequestHandle request, json response) {
			if (g_cvarGFLBansLogLevel.Get() == static_cast<int>(LogLevel::Debug))
				Message(("Check Infraction Response:\n" + response.dump(1) + "\n").c_str());

			ZEPlayer* zpPlayer = zphPlayer.Get();
			if (!zpPlayer)
				return;

			CheckJSONForBlock(zpPlayer, response, InfType::Mute, true, false);
			CheckJSONForBlock(zpPlayer, response, InfType::Gag, true, false);
			CheckJSONForBlock(zpPlayer, response, InfType::AdminChatGag, true, false);
			CheckJSONForBlock(zpPlayer, response, InfType::ItemBlock, true, false);
			// We dont need to check to apply a Call Admin Block server side, since that is all handled by GFLBans itself

			// Ban should be checked last, since it could make zpPlayer point at garbage
			CheckJSONForBlock(zpPlayer, response, InfType::Ban, true, false);
		},
		[](HTTPRequestHandle request, EHTTPStatusCode eStatusCode, json response) {
			LogGFLBansError("CheckPlayerInfraction", request, eStatusCode, response);
		},
		g_rghdGFLBansAuth);
}

void GFLBansSystem::GetPunishmentStats(CCSPlayerController* pAdmin, CCSPlayerController* pBadPerson,
									   bool bOnlineOnly,
									   std::function<void(CCSPlayerController*, CCSPlayerController*, InfractionStatisticsReply)> funcLogic,
									   std::string strReason)
{
	ZEPlayer* zpBadPerson = pBadPerson->GetZEPlayer();
	if (!zpBadPerson || zpBadPerson->IsFakeClient() || !zpBadPerson->IsAuthenticated())
		return;

	std::string strURL = std::string(g_cvarGFLBansApiUrl.Get().String())
						 + "infractions/stats?gs_service=steam&active_only=false&count_only=false&exclude_removed=true&gs_id="
						 + std::to_string(zpBadPerson->GetSteamId64());

	if (bOnlineOnly)
		strURL.append("&online_only=true");

	std::string strIP = zpBadPerson->GetIpAddress();
	if (strIP.length() > 0 && IsValidIP(strIP))
		strURL.append("&ip=" + strIP);

	if (strReason.length() > 0)
		strURL.append("&reason=" + strReason);

	CHandle<CCSPlayerController> hAdmin = pAdmin ? pAdmin->GetHandle() : nullptr;
	CHandle<CCSPlayerController> hBadPerson = pBadPerson ? pBadPerson->GetHandle() : nullptr;

	if (g_cvarGFLBansLogLevel.Get() == static_cast<int>(LogLevel::Debug))
		Message("GetPunishmentHistory Query: %s\n", strURL.c_str());

	g_HTTPManager.Get(
		strURL.c_str(),
		[hAdmin, hBadPerson, funcLogic](HTTPRequestHandle request, json response) {
			if (g_cvarGFLBansLogLevel.Get() == static_cast<int>(LogLevel::Debug))
				Message("GetPunishmentHistory response: %s\n", response.dump().c_str());

			CCSPlayerController* pAdmin = hAdmin ? hAdmin.Get() : nullptr;
			CCSPlayerController* pBadPerson = hBadPerson ? hBadPerson.Get() : nullptr;
			if ((hAdmin && !pAdmin) || (hBadPerson && !pBadPerson))
				return;

			InfractionStatisticsReply history;

			history.mapPunishmentCounts[InfType::Ban] = response.value("ban_count", 0);
			if (history.mapPunishmentCounts[InfType::Ban] > 0)
				history.mapPunishmentLongest[InfType::Ban] = response.value("ban_longest", -1);

			history.mapPunishmentCounts[InfType::Mute] = response.value("voice_block_count", 0);
			if (history.mapPunishmentCounts[InfType::Mute] > 0)
				history.mapPunishmentLongest[InfType::Mute] = response.value("voice_block_longest", -1);

			history.mapPunishmentCounts[InfType::Gag] = response.value("text_block_count", 0);
			if (history.mapPunishmentCounts[InfType::Gag] > 0)
				history.mapPunishmentLongest[InfType::Gag] = response.value("text_block_longest", -1);

			history.mapPunishmentCounts[InfType::AdminChatGag] = response.value("admin_chat_block_count", 0);
			if (history.mapPunishmentCounts[InfType::AdminChatGag] > 0)
				history.mapPunishmentLongest[InfType::AdminChatGag] = response.value("admin_chat_block_longest", -1);

			history.mapPunishmentCounts[InfType::CallAdminBlock] = response.value("call_admin_block_count", 0);
			if (history.mapPunishmentCounts[InfType::CallAdminBlock] > 0)
				history.mapPunishmentLongest[InfType::CallAdminBlock] = response.value("call_admin_block_longest", -1);

			history.mapPunishmentCounts[InfType::ItemBlock] = response.value("item_block_count", 0);
			if (history.mapPunishmentCounts[InfType::ItemBlock] > 0)
				history.mapPunishmentLongest[InfType::ItemBlock] = response.value("item_block_longest", -1);

			history.mapPunishmentCounts[InfType::Warn] = response.value("warning_count", 0);
			if (history.mapPunishmentCounts[InfType::Warn] > 0)
				history.mapPunishmentLongest[InfType::Warn] = response.value("warning_longest", -1);

			funcLogic(pAdmin, pBadPerson, history);
		},
		nullptr,
		g_rghdGFLBansAuth);
}

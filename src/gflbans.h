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

#pragma once

#include "ctimer.h"
#include "entity/ccsplayercontroller.h"
#include "httpmanager.h"
#include "vendor/nlohmann/json.hpp"

using json = nlohmann::json;

#define GFLBANS_PREFIX " \x07[GFLBans]\1 "

enum class LogLevel
{
	None = 0,
	Error = 1,
	Debug = 2
};

enum class InfType
{
	Ban = 0, // Always guarantee Ban is first, as some logic depends on it
	Mute = 1,
	Gag = 2,
	Silence = 3,
	AdminChatGag = 4,
	CallAdminBlock = 5,
	ItemBlock = 6,
	Warn = 7,
	Invalid = 8
};

inline bool IsValidInfType(InfType iType) noexcept
{
	return iType >= InfType::Ban && iType < InfType::Invalid;
}

inline InfType& operator++(InfType& iType)
{
	if (iType == InfType::Invalid)
		return iType = InfType::Invalid;
	return iType = static_cast<InfType>(static_cast<int>(iType) + 1);
}

inline InfType operator++(InfType& iType, int)
{
	InfType temp(iType);
	++iType;
	return temp;
}

enum class EchoType
{
	None,
	All,
	Admin,
	Target,
	Console
};

enum class InfractionFlags
{
	SYSTEM = 1 << 0,
	GLOBAL = 1 << 1,
	COMMUNITY = 1 << 2,
	PERMANENT = 1 << 3,
	VPN = 1 << 4,
	WEB = 1 << 5,
	REMOVED = 1 << 6,
	VOICE_BLOCK = 1 << 7,
	CHAT_BLOCK = 1 << 8,
	BAN = 1 << 9,
	ADMIN_CHAT_BLOCK = 1 << 10,
	CALL_ADMIN_BAN = 1 << 11,
	SESSION = 1 << 12,
	PLAYTIME_BASED = 1 << 13,
	ITEM_BLOCK = 1 << 14,
	AUTO_TIER = 1 << 16,
	NOT_WARNING = (VOICE_BLOCK | CHAT_BLOCK | BAN | ADMIN_CHAT_BLOCK | CALL_ADMIN_BAN | ITEM_BLOCK)
};

class GFLBans_InfractionBase
{
public:
	enum GFLInfractionScope
	{
		Server,
		Global
	};

	GFLBans_InfractionBase(InfType infType, CHandle<CCSPlayerController> hTarget, std::string strReason,
						   CHandle<CCSPlayerController> hAdmin = nullptr) :
		m_infType(infType),
		m_hTarget(hTarget), m_hAdmin(hAdmin)
	{
		m_strReason = strReason.length() == 0	? "No reason provided" :
					  strReason.length() <= 280 ? strReason :
												  strReason.substr(0, 280);
	}

	virtual json CreateInfractionJSON() const = 0;
	InfType GetInfractionType() const noexcept { return m_infType; }
	std::string GetReason() const noexcept { return m_strReason; }
	virtual ~GFLBans_InfractionBase() {}

protected:
	InfType m_infType;
	CHandle<CCSPlayerController> m_hTarget;
	CHandle<CCSPlayerController> m_hAdmin;
	std::string m_strReason;
};

class GFLBans_Infraction : public GFLBans_InfractionBase
{
public:
	GFLBans_Infraction(InfType infType, CHandle<CCSPlayerController> hTarget,
					   std::string strReason, CHandle<CCSPlayerController> hAdmin = nullptr,
					   int iDuration = -1, bool bPlaytimeBased = false);

	inline bool IsSession() const noexcept { return m_wExpires < m_wCreated && m_infType != InfType::Ban; }

	// Creates a JSON object to pass in a POST request to GFLBans
	virtual json CreateInfractionJSON() const override;

private:
	std::string m_strID;
	uint m_wCreated; // UNIX timestamp
	uint m_wExpires; // UNIX timestamp
	GFLInfractionScope m_gisScope;
	bool m_bPlaytimeBased;
};

class GFLBans_InfractionRemoval : public GFLBans_InfractionBase
{
public:
	GFLBans_InfractionRemoval(InfType infType, CHandle<CCSPlayerController> hTarget, std::string strReason,
							  CHandle<CCSPlayerController> hAdmin = nullptr) :
		GFLBans_InfractionBase(infType, hTarget, strReason, hAdmin)
	{}

	// Creates a JSON object to pass in a POST request to GFLBans
	virtual json CreateInfractionJSON() const override;
};

class GFLBans_Report
{
public:
	GFLBans_Report(CCSPlayerController* pCaller, std::string strMessage, CCSPlayerController* pBadPerson = nullptr);

	json CreateReportJSON() const;
	inline bool IsReport() const noexcept { return m_jBadPerson != nullptr && !m_jBadPerson.empty(); }
	void CallAdmin(CCSPlayerController* pCaller);
	virtual ~GFLBans_Report() {}

protected:
	json m_jCaller;
	std::string m_strCallerName;
	std::string m_strMessage;
	json m_jBadPerson;
	std::string m_strBadPersonName;
};

struct InfractionStatisticsReply
{
public:
	InfractionStatisticsReply()
	{
		for (InfType i = InfType::Ban; IsValidInfType(i); ++i)
		{
			mapPunishmentCounts[i] = 0;
			mapPunishmentLongest[i] = std::nullopt;
		}
	}

	std::map<InfType, std::time_t> mapPunishmentCounts;
	std::map<InfType, std::optional<std::time_t>> mapPunishmentLongest;
};

class GFLBansSystem
{
public:
	// returns true if ATTEMPTS to heartbeat, false otherwise. This is not based on if GFLBans responds
	// https://github.com/gflze/GFLBans/wiki#heartbeat
	bool Heartbeat();

	// Update g_pAdminSystem with infractions from the web.
	// https://github.com/gflze/GFLBans/wiki#checking-player-infractions
	void CheckPlayerInfractions(ZEPlayer* player);

	// Creates a new infraction of type infType on the server and adds it to GFLBans.
	// https://github.com/gflze/GFLBans/wiki#standard-infractions
	void CreateInfraction(InfType infType, EchoType echo, CCSPlayerController* pAdmin,
						  CCSPlayerController* pBadPerson, std::string strReason, int iDuration,
						  bool bPlaytimeBased, bool bPrintErrorsToAdmin = true);

	// Removes all infractions of type infType both on the server and on GFLBans
	// if admin has permission on GFLBans to remove them
	// https://github.com/gflze/GFLBans/wiki#removing-infractions
	void RemoveInfraction(InfType infType, EchoType echo, CCSPlayerController* pAdmin,
						  CCSPlayerController* pGoodPerson, std::string strReason,
						  bool bPrintErrorsToAdmin = true);

	// Returns true if a chat message should be filtered and false if not
	// If gflbans_filtered_gag_duration is non-negative, pChatter will be gagged for that duration if true return value
	bool FilterMessage(CCSPlayerController* pChatter, const CCommand& args);

	// Passes pBadPerson's past infraction info into funcLogic
	// https://github.com/gflze/GFLBans/wiki#getting-infractions-stats
	void GetPunishmentStats(CCSPlayerController* pAdmin, CCSPlayerController* pBadPerson, bool bPlaytimeBased,
							std::function<void(CCSPlayerController*, CCSPlayerController*, InfractionStatisticsReply)> funcLogic,
							std::string strReason = "");
};

extern GFLBansSystem* g_pGFLBansSystem;
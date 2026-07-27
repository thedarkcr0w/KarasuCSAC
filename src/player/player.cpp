#include "player.h"

CCSPlayerController *Player::GetController()
{
	if (!GameEntitySystem())
	{
		return nullptr;
	}
	auto *entity = static_cast<CBaseEntity *>(GameEntitySystem()->GetEntityInstance(CEntityIndex(index)));
	return entity && entity->IsController() ? static_cast<CCSPlayerController *>(entity) : nullptr;
}

CBasePlayerPawn *Player::GetCurrentPawn()
{
	auto *controller = GetController();
	return controller ? controller->GetCurrentPawn() : nullptr;
}

CCSPlayerPawn *Player::GetPlayerPawn()
{
	auto *controller = GetController();
	return controller ? controller->GetPlayerPawn() : nullptr;
}

CServerSideClient *Player::GetClient()
{
	return g_pCS2ACUtils ? g_pCS2ACUtils->GetClientBySlot(GetPlayerSlot()) : nullptr;
}

bool Player::IsConnected()
{
	auto *client = GetClient();
	return client && client->IsConnected();
}

bool Player::IsInGame()
{
	auto *client = GetClient();
	return client && client->IsInGame();
}

bool Player::IsFakeClient()
{
	auto *client = GetClient();
	return client && client->IsFakeClient();
}

bool Player::IsCSTV()
{
	auto *client = GetClient();
	return client && client->IsHLTV();
}

const char *Player::GetName()
{
	auto *client = GetClient();
	sanitizedName = client ? client->GetClientName() : "<unknown>";
	sanitizedName.Trim();
	return sanitizedName.Get();
}

u64 Player::GetSteamId64(bool validated)
{
	auto *client = GetClient();
	if (client)
	{
		return client->GetClientSteamID().ConvertToUint64();
	}
	// The remaining sources are not authenticated: the controller's copy, and the
	// raw xuid captured on client activation. Those are fine for logging and for
	// announcements, but a caller that is about to act against somebody's account -
	// the Karasu ban relay - passes validated and must get nothing back rather than
	// an identity that could belong to a different player.
	if (validated)
	{
		return 0;
	}
	auto *controller = GetController();
	return controller ? controller->m_steamID() : unauthenticatedSteamID;
}

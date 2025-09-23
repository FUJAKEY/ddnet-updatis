/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.		*/
#include <base/math.h>

#include <engine/client.h>
#include <engine/shared/config.h>

#include <game/client/components/camera.h>
#include <game/client/components/chat.h>
#include <game/client/components/menus.h>
#include <game/client/components/scoreboard.h>
#include <game/client/gameclient.h>
#include <game/collision.h>
#include <game/mapitems.h>

#include <base/vmath.h>

#include "controls.h"

// Predict fewer ticks ahead so the player can approach freeze walls more closely
static constexpr int g_PredictFreezeTicks = 3;
// Rage hook rescue needs a longer horizon to react before the player reaches freeze tiles
static constexpr int g_HookRescuePredictTicks = 10;

static constexpr float DegToRad(float Deg)
{
        return Deg * pi / 180.0f;
}

static bool IsFreezeTileIndex(int Tile)
{
        return Tile == TILE_FREEZE || Tile == TILE_DFREEZE || Tile == TILE_LFREEZE;
}

static bool IsHookPointOnFreeze(const vec2 &Pos, CCollision *pCollision)
{
        if(!pCollision)
                return false;

        const int Index = pCollision->GetPureMapIndex(Pos);
        return IsFreezeTileIndex(pCollision->GetTileIndex(Index)) || IsFreezeTileIndex(pCollision->GetFrontTileIndex(Index));
}

static bool ValidateHookTarget(const CCharacterCore &Core, const vec2 &Target, CCollision *pCollision, float HookLength)
{
        if(length(Target) < 0.01f || !pCollision)
                return false;

        vec2 HookPoint = Target;
        vec2 Before;
        const int HitTile = pCollision->IntersectLineTeleHook(Core.m_Pos, Target, &HookPoint, &Before);
        if(HitTile == 0 || HitTile == TILE_NOHOOK)
                return false;
        if(IsFreezeTileIndex(HitTile))
                return false;
        if(IsHookPointOnFreeze(HookPoint, pCollision))
                return false;

        return distance(Core.m_Pos, HookPoint) <= HookLength + 1.0f;
}

static bool CheckFreeze(const vec2 &Pos, CCollision *pCollision, vec2 *pHit = nullptr)
{
        static const vec2 s_aOffsets[] = {
                vec2(0.0f, 0.0f),
                vec2(14.0f, 0.0f),
		vec2(-14.0f, 0.0f),
		vec2(0.0f, 14.0f),
		vec2(0.0f, -14.0f)};
	for(const vec2 &Off : s_aOffsets)
	{
		int Index = pCollision->GetPureMapIndex(Pos + Off);
		int Tile = pCollision->GetTileIndex(Index);
		int FrontTile = pCollision->GetFrontTileIndex(Index);
		if(Tile == TILE_FREEZE || Tile == TILE_DFREEZE || Tile == TILE_LFREEZE ||
			FrontTile == TILE_FREEZE || FrontTile == TILE_DFREEZE || FrontTile == TILE_LFREEZE)
		{
			if(pHit)
				*pHit = Off;
			return true;
		}
	}
	return false;
}

static bool PredictFreeze(CCharacterCore Core, const CNetObj_PlayerInput &Input, CCollision *pCollision, bool CheckStart = true, vec2 *pHit = nullptr, int Steps = g_PredictFreezeTicks)
{
	if(CheckStart && CheckFreeze(Core.m_Pos, pCollision, pHit))
		return true;

	for(int i = 0; i < Steps; i++)
	{
		Core.m_Input = Input;
		Core.Tick(true);
		Core.Move();
		Core.Quantize();
		if(CheckFreeze(Core.m_Pos, pCollision, pHit))
			return true;
	}
	return false;
}

static bool TryHookRescue(const CCharacterCore &Core, CNetObj_PlayerInput &Input, const vec2 &HitDir, CCollision *pCollision,
        float HookLength, const vec2 &LastTarget, vec2 *pOutTarget, bool *pOutSafe)
{
        if(HookLength <= 0.0f)
                HookLength = 380.0f;

        vec2 ThreatDir = HitDir;
        if(length(ThreatDir) < 0.01f)
                ThreatDir = vec2(1.0f, 0.0f);
        else
                ThreatDir = normalize(ThreatDir);

        vec2 EscapeNorm = -ThreatDir;
        if(length(EscapeNorm) < 0.01f)
                EscapeNorm = vec2(-1.0f, 0.0f);
        else
                EscapeNorm = normalize(EscapeNorm);

        const vec2 Vel = Core.m_Vel;
        const float VelLen = length(Vel);
        vec2 VelNorm = vec2(0.0f, 0.0f);
        if(VelLen > 0.001f)
                VelNorm = Vel / VelLen;

        const auto Rotate = [](const vec2 &Dir, float Angle) {
                const float C = std::cos(Angle);
                const float S = std::sin(Angle);
                return vec2(Dir.x * C - Dir.y * S, Dir.x * S + Dir.y * C);
        };

        const CNetObj_PlayerInput BaseInput = Input;

        bool FoundSafe = false;
        bool FoundFallback = false;
        float BestSafeScore = -1e9f;
        float BestFallbackScore = -1e9f;
        vec2 BestSafeTarget = Core.m_Pos;
        vec2 BestFallbackTarget = Core.m_Pos;

        const auto EvaluateDirection = [&](const vec2 &RawDir) {
                vec2 Dir = RawDir;
                if(length(Dir) < 0.01f)
                        return;
                Dir = normalize(Dir);

                vec2 AimPos = Core.m_Pos + Dir * HookLength;
                vec2 HookPoint = AimPos;
                bool HasAttach = false;
                if(pCollision)
                {
                        vec2 Before;
                        const int HitTile = pCollision->IntersectLineTeleHook(Core.m_Pos, AimPos, &HookPoint, &Before);
                        if(HitTile == 0 || HitTile == TILE_NOHOOK)
                                return;
                        if(IsFreezeTileIndex(HitTile))
                                return;
                        if(IsHookPointOnFreeze(HookPoint, pCollision))
                                return;
                        if(distance(Core.m_Pos, HookPoint) > HookLength + 1.0f)
                                return;
                        HasAttach = true;
                }

                if(!HasAttach)
                        return;

                CNetObj_PlayerInput Test = BaseInput;
                Test.m_Hook = 1;
                Test.m_TargetX = (int)HookPoint.x;
                Test.m_TargetY = (int)HookPoint.y;

                const bool Safe = !PredictFreeze(Core, Test, pCollision, false, nullptr, g_HookRescuePredictTicks);

                float Score = dot(Dir, EscapeNorm) * 6.0f;
                Score -= maximum(dot(Dir, ThreatDir), 0.0f) * 5.0f;
                Score += clamp(-Dir.y, 0.0f, 1.0f) * 4.0f;
                Score -= maximum(Dir.y, 0.0f) * 6.0f;

                if(VelLen > 0.001f)
                        Score += -dot(Dir, VelNorm) * 2.0f;

                const float AttachDistance = distance(Core.m_Pos, HookPoint);
                const float AttachScore = clamp(1.0f - AttachDistance / HookLength, 0.0f, 1.0f);
                Score += AttachScore * 3.0f;

                if(length(LastTarget) > 0.01f)
                {
                        const float Similarity = clamp(1.0f - distance(HookPoint, LastTarget) / 64.0f, -1.0f, 1.0f);
                        Score += Similarity * 1.5f;
                }

                if(Safe)
                {
                        if(!FoundSafe || Score > BestSafeScore)
                        {
                                FoundSafe = true;
                                BestSafeScore = Score;
                                BestSafeTarget = HookPoint;
                        }
                }
                else
                {
                        Score -= 3.0f;
                        if(!FoundFallback || Score > BestFallbackScore)
                        {
                                FoundFallback = true;
                                BestFallbackScore = Score;
                                BestFallbackTarget = HookPoint;
                        }
                }
        };

        if(length(LastTarget) > 0.01f)
                EvaluateDirection(LastTarget - Core.m_Pos);

        const int MaxAngleSteps = 11;
        for(int Step = 0; Step <= MaxAngleSteps; ++Step)
        {
                const float Angle = DegToRad(Step * 15.0f);
                EvaluateDirection(Rotate(EscapeNorm, Angle));
                if(Step != 0)
                        EvaluateDirection(Rotate(EscapeNorm, -Angle));
        }

        vec2 Side = vec2(-EscapeNorm.y, EscapeNorm.x);
        for(int Step = -4; Step <= 4; ++Step)
        {
                const float Angle = DegToRad(Step * 15.0f);
                EvaluateDirection(Rotate(Side, Angle));
        }

        EvaluateDirection(vec2(0.0f, -1.0f));
        EvaluateDirection(vec2(-1.0f, -0.4f));
        EvaluateDirection(vec2(1.0f, -0.4f));

        if(VelLen > 0.001f)
        {
                EvaluateDirection(-VelNorm);
                EvaluateDirection(Rotate(-VelNorm, DegToRad(20.0f)));
                EvaluateDirection(Rotate(-VelNorm, -DegToRad(20.0f)));
        }

        if(FoundSafe)
        {
                Input.m_Hook = 1;
                Input.m_TargetX = (int)BestSafeTarget.x;
                Input.m_TargetY = (int)BestSafeTarget.y;
                if(pOutTarget)
                        *pOutTarget = BestSafeTarget;
                if(pOutSafe)
                        *pOutSafe = true;
                return true;
        }

        if(FoundFallback)
        {
                Input.m_Hook = 1;
                Input.m_TargetX = (int)BestFallbackTarget.x;
                Input.m_TargetY = (int)BestFallbackTarget.y;
                if(pOutTarget)
                        *pOutTarget = BestFallbackTarget;
                if(pOutSafe)
                        *pOutSafe = false;
                return true;
        }

        return false;
}

CControls::CControls()
{
        mem_zero(&m_aLastData, sizeof(m_aLastData));
        mem_zero(m_aMousePos, sizeof(m_aMousePos));
        mem_zero(m_aMousePosOnAction, sizeof(m_aMousePosOnAction));
        mem_zero(m_aTargetPos, sizeof(m_aTargetPos));

        for(int i = 0; i < NUM_DUMMIES; ++i)
                m_aRageState[i].Reset();
}

void CControls::OnReset()
{
        ResetInput(0);
        ResetInput(1);

        for(int &AmmoCount : m_aAmmoCount)
                AmmoCount = 0;

        m_LastSendTime = 0;

        for(int i = 0; i < NUM_DUMMIES; ++i)
                m_aRageState[i].Reset();
}

void CControls::ResetInput(int Dummy)
{
	m_aLastData[Dummy].m_Direction = 0;
	// simulate releasing the fire button
	if((m_aLastData[Dummy].m_Fire & 1) != 0)
		m_aLastData[Dummy].m_Fire++;
	m_aLastData[Dummy].m_Fire &= INPUT_STATE_MASK;
	m_aLastData[Dummy].m_Jump = 0;
	m_aInputData[Dummy] = m_aLastData[Dummy];

	m_aInputDirectionLeft[Dummy] = 0;
	m_aInputDirectionRight[Dummy] = 0;
}

void CControls::OnPlayerDeath()
{
        for(int &AmmoCount : m_aAmmoCount)
                AmmoCount = 0;

        for(int i = 0; i < NUM_DUMMIES; ++i)
                m_aRageState[i].Reset();
}

struct CInputState
{
	CControls *m_pControls;
	int *m_apVariables[NUM_DUMMIES];
};

static void ConKeyInputState(IConsole::IResult *pResult, void *pUserData)
{
	CInputState *pState = (CInputState *)pUserData;

	if(pState->m_pControls->GameClient()->m_GameInfo.m_BugDDRaceInput && pState->m_pControls->GameClient()->m_Snap.m_SpecInfo.m_Active)
		return;

	*pState->m_apVariables[g_Config.m_ClDummy] = pResult->GetInteger(0);
}

static void ConKeyInputCounter(IConsole::IResult *pResult, void *pUserData)
{
	CInputState *pState = (CInputState *)pUserData;

	if((pState->m_pControls->GameClient()->m_GameInfo.m_BugDDRaceInput && pState->m_pControls->GameClient()->m_Snap.m_SpecInfo.m_Active) || pState->m_pControls->GameClient()->m_Spectator.IsActive())
		return;

	int *pVariable = pState->m_apVariables[g_Config.m_ClDummy];
	if(((*pVariable) & 1) != pResult->GetInteger(0))
		(*pVariable)++;
	*pVariable &= INPUT_STATE_MASK;
}

struct CInputSet
{
	CControls *m_pControls;
	int *m_apVariables[NUM_DUMMIES];
	int m_Value;
};

static void ConKeyInputSet(IConsole::IResult *pResult, void *pUserData)
{
	CInputSet *pSet = (CInputSet *)pUserData;
	if(pResult->GetInteger(0))
	{
		*pSet->m_apVariables[g_Config.m_ClDummy] = pSet->m_Value;
	}
}

static void ConKeyInputNextPrevWeapon(IConsole::IResult *pResult, void *pUserData)
{
	CInputSet *pSet = (CInputSet *)pUserData;
	ConKeyInputCounter(pResult, pSet);
	pSet->m_pControls->m_aInputData[g_Config.m_ClDummy].m_WantedWeapon = 0;
}

void CControls::OnConsoleInit()
{
	// game commands
	{
		static CInputState s_State = {this, {&m_aInputDirectionLeft[0], &m_aInputDirectionLeft[1]}};
		Console()->Register("+left", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Move left");
	}
	{
		static CInputState s_State = {this, {&m_aInputDirectionRight[0], &m_aInputDirectionRight[1]}};
		Console()->Register("+right", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Move right");
	}
	{
		static CInputState s_State = {this, {&m_aInputData[0].m_Jump, &m_aInputData[1].m_Jump}};
		Console()->Register("+jump", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Jump");
	}
	{
		static CInputState s_State = {this, {&m_aInputData[0].m_Hook, &m_aInputData[1].m_Hook}};
		Console()->Register("+hook", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Hook");
	}
	{
		static CInputState s_State = {this, {&m_aInputData[0].m_Fire, &m_aInputData[1].m_Fire}};
		Console()->Register("+fire", "", CFGFLAG_CLIENT, ConKeyInputCounter, &s_State, "Fire");
	}
	{
		static CInputState s_State = {this, {&m_aShowHookColl[0], &m_aShowHookColl[1]}};
		Console()->Register("+showhookcoll", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Show Hook Collision");
	}

	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_WantedWeapon, &m_aInputData[1].m_WantedWeapon}, 1};
		Console()->Register("+weapon1", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to hammer");
	}
	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_WantedWeapon, &m_aInputData[1].m_WantedWeapon}, 2};
		Console()->Register("+weapon2", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to gun");
	}
	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_WantedWeapon, &m_aInputData[1].m_WantedWeapon}, 3};
		Console()->Register("+weapon3", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to shotgun");
	}
	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_WantedWeapon, &m_aInputData[1].m_WantedWeapon}, 4};
		Console()->Register("+weapon4", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to grenade");
	}
	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_WantedWeapon, &m_aInputData[1].m_WantedWeapon}, 5};
		Console()->Register("+weapon5", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to laser");
	}

	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_NextWeapon, &m_aInputData[1].m_NextWeapon}, 0};
		Console()->Register("+nextweapon", "", CFGFLAG_CLIENT, ConKeyInputNextPrevWeapon, &s_Set, "Switch to next weapon");
	}
	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_PrevWeapon, &m_aInputData[1].m_PrevWeapon}, 0};
		Console()->Register("+prevweapon", "", CFGFLAG_CLIENT, ConKeyInputNextPrevWeapon, &s_Set, "Switch to previous weapon");
	}
}

void CControls::OnMessage(int Msg, void *pRawMsg)
{
	if(Msg == NETMSGTYPE_SV_WEAPONPICKUP)
	{
		CNetMsg_Sv_WeaponPickup *pMsg = (CNetMsg_Sv_WeaponPickup *)pRawMsg;
		if(g_Config.m_ClAutoswitchWeapons)
			m_aInputData[g_Config.m_ClDummy].m_WantedWeapon = pMsg->m_Weapon + 1;
		// We don't really know ammo count, until we'll switch to that weapon, but any non-zero count will suffice here
		m_aAmmoCount[maximum(0, pMsg->m_Weapon % NUM_WEAPONS)] = 10;
	}
}

int CControls::SnapInput(int *pData)
{
	// update player state
	if(m_pClient->m_Chat.IsActive())
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags = PLAYERFLAG_CHATTING;
	else if(m_pClient->m_Menus.IsActive())
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags = PLAYERFLAG_IN_MENU;
	else
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags = PLAYERFLAG_PLAYING;

	if(m_pClient->m_Scoreboard.IsActive())
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_SCOREBOARD;

	if(Client()->ServerCapAnyPlayerFlag() && m_pClient->m_Controls.m_aShowHookColl[g_Config.m_ClDummy])
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_AIM;

	if(Client()->ServerCapAnyPlayerFlag() && m_pClient->m_Camera.CamType() == CCamera::CAMTYPE_SPEC)
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_SPEC_CAM;

	bool Send = m_aLastData[g_Config.m_ClDummy].m_PlayerFlags != m_aInputData[g_Config.m_ClDummy].m_PlayerFlags;

	m_aLastData[g_Config.m_ClDummy].m_PlayerFlags = m_aInputData[g_Config.m_ClDummy].m_PlayerFlags;

	// we freeze the input if chat or menu is activated
	if(!(m_aInputData[g_Config.m_ClDummy].m_PlayerFlags & PLAYERFLAG_PLAYING))
	{
		if(!GameClient()->m_GameInfo.m_BugDDRaceInput)
			ResetInput(g_Config.m_ClDummy);

		mem_copy(pData, &m_aInputData[g_Config.m_ClDummy], sizeof(m_aInputData[0]));

		// set the target anyway though so that we can keep seeing our surroundings,
		// even if chat or menu are activated
		m_aInputData[g_Config.m_ClDummy].m_TargetX = (int)m_aMousePos[g_Config.m_ClDummy].x;
		m_aInputData[g_Config.m_ClDummy].m_TargetY = (int)m_aMousePos[g_Config.m_ClDummy].y;

		// send once a second just to be sure
		Send = Send || time_get() > m_LastSendTime + time_freq();
	}
	else
	{
		m_aInputData[g_Config.m_ClDummy].m_TargetX = (int)m_aMousePos[g_Config.m_ClDummy].x;
		m_aInputData[g_Config.m_ClDummy].m_TargetY = (int)m_aMousePos[g_Config.m_ClDummy].y;

		if(g_Config.m_ClSubTickAiming && m_aMousePosOnAction[g_Config.m_ClDummy] != vec2(0.0f, 0.0f))
		{
			m_aInputData[g_Config.m_ClDummy].m_TargetX = (int)m_aMousePosOnAction[g_Config.m_ClDummy].x;
			m_aInputData[g_Config.m_ClDummy].m_TargetY = (int)m_aMousePosOnAction[g_Config.m_ClDummy].y;
			m_aMousePosOnAction[g_Config.m_ClDummy] = vec2(0.0f, 0.0f);
		}

		if(!m_aInputData[g_Config.m_ClDummy].m_TargetX && !m_aInputData[g_Config.m_ClDummy].m_TargetY)
		{
			m_aInputData[g_Config.m_ClDummy].m_TargetX = 1;
			m_aMousePos[g_Config.m_ClDummy].x = 1;
		}

		// set direction
		m_aInputData[g_Config.m_ClDummy].m_Direction = 0;
		if(m_aInputDirectionLeft[g_Config.m_ClDummy] && !m_aInputDirectionRight[g_Config.m_ClDummy])
			m_aInputData[g_Config.m_ClDummy].m_Direction = -1;
		if(!m_aInputDirectionLeft[g_Config.m_ClDummy] && m_aInputDirectionRight[g_Config.m_ClDummy])
			m_aInputData[g_Config.m_ClDummy].m_Direction = 1;

		// dummy copy moves
		if(g_Config.m_ClDummyCopyMoves)
		{
			CNetObj_PlayerInput *pDummyInput = &m_pClient->m_DummyInput;
			pDummyInput->m_Direction = m_aInputData[g_Config.m_ClDummy].m_Direction;
			pDummyInput->m_Hook = m_aInputData[g_Config.m_ClDummy].m_Hook;
			pDummyInput->m_Jump = m_aInputData[g_Config.m_ClDummy].m_Jump;
			pDummyInput->m_PlayerFlags = m_aInputData[g_Config.m_ClDummy].m_PlayerFlags;
			pDummyInput->m_TargetX = m_aInputData[g_Config.m_ClDummy].m_TargetX;
			pDummyInput->m_TargetY = m_aInputData[g_Config.m_ClDummy].m_TargetY;
			pDummyInput->m_WantedWeapon = m_aInputData[g_Config.m_ClDummy].m_WantedWeapon;

			if(!g_Config.m_ClDummyControl)
				pDummyInput->m_Fire += m_aInputData[g_Config.m_ClDummy].m_Fire - m_aLastData[g_Config.m_ClDummy].m_Fire;

			pDummyInput->m_NextWeapon += m_aInputData[g_Config.m_ClDummy].m_NextWeapon - m_aLastData[g_Config.m_ClDummy].m_NextWeapon;
			pDummyInput->m_PrevWeapon += m_aInputData[g_Config.m_ClDummy].m_PrevWeapon - m_aLastData[g_Config.m_ClDummy].m_PrevWeapon;

			m_aInputData[!g_Config.m_ClDummy] = *pDummyInput;
		}

		if(g_Config.m_ClDummyControl)
		{
			CNetObj_PlayerInput *pDummyInput = &m_pClient->m_DummyInput;
			pDummyInput->m_Jump = g_Config.m_ClDummyJump;

			if(g_Config.m_ClDummyFire)
				pDummyInput->m_Fire = g_Config.m_ClDummyFire;
			else if((pDummyInput->m_Fire & 1) != 0)
				pDummyInput->m_Fire++;

			pDummyInput->m_Hook = g_Config.m_ClDummyHook;
		}

		const int GoresMode = clamp(g_Config.m_ClGoresBot, 0, 2);
		const bool AvoidFreezeActive = g_Config.m_ClAvoidFreeze || GoresMode != 0;
		if(AvoidFreezeActive && m_pClient->m_Snap.m_LocalClientId >= 0)
		{
			int LocalId = m_pClient->m_Snap.m_LocalClientId;
			CCharacterCore Core = m_pClient->m_aClients[LocalId].m_Predicted;
			CNetObj_PlayerInput &Input = m_aInputData[g_Config.m_ClDummy];
			vec2 HitDirShort(0.0f, 0.0f);
			bool DangerShort = PredictFreeze(Core, Input, m_pClient->Collision(), true, &HitDirShort);
			vec2 HookThreatDir = HitDirShort;
			bool DangerLong = DangerShort;
			bool HookDirFromExtended = false;
			if(GoresMode == 2)
			{
				vec2 ExtendedHit(0.0f, 0.0f);
				if(PredictFreeze(Core, Input, m_pClient->Collision(), true, &ExtendedHit, g_HookRescuePredictTicks))
				{
					DangerLong = true;
					HookThreatDir = ExtendedHit;
					HookDirFromExtended = true;
				}
			}

			if(DangerShort)
			{
				if(absolute(HitDirShort.y) <= absolute(HitDirShort.x))
				{
					const bool CancelHook = g_Config.m_ClAvoidFreezeHook && g_Config.m_ClAvoidFreeze && GoresMode == 0;
					bool DangerPersists = true;
					if(CancelHook)
					{
						Input.m_Hook = 0;
						DangerPersists = PredictFreeze(Core, Input, m_pClient->Collision(), true, &HitDirShort);
					}

                                        if(DangerPersists)
                                        {
                                                CNetObj_PlayerInput Test = Input;
                                                Test.m_Direction = -1;
                                                const bool SafeLeft = !PredictFreeze(Core, Test, m_pClient->Collision(), false);
                                                Test.m_Direction = 1;
                                                const bool SafeRight = !PredictFreeze(Core, Test, m_pClient->Collision(), false);

                                                int Desired = HitDirShort.x > 0 ? -1 : 1;
                                                if(SafeLeft && !SafeRight)
                                                        Desired = -1;
                                                else if(SafeRight && !SafeLeft)
                                                        Desired = 1;
                                                else if(SafeLeft && SafeRight)
                                                {
                                                        if(Core.m_Vel.x > 0.1f)
                                                                Desired = -1;
                                                        else if(Core.m_Vel.x < -0.1f)
                                                                Desired = 1;
                                                        else
                                                                Desired = HitDirShort.x > 0 ? -1 : 1;
                                                }

                                                Input.m_Direction = Desired;
                                        }
                                }

                                if(DangerLong && !HookDirFromExtended)
                                        HookThreatDir = HitDirShort;
                        }

                        CRageState &RageState = m_aRageState[g_Config.m_ClDummy];
                        const float HookLength = m_pClient->m_aTuning[g_Config.m_ClDummy].m_HookLength;

                        if(GoresMode == 2)
                        {
                                if(DangerLong)
                                {
                                        RageState.m_LastThreatDir = HookThreatDir;
                                        bool ShouldHook = false;
                                        const bool HorizontalThreat = absolute(HookThreatDir.y) <= absolute(HookThreatDir.x);
                                        if(HorizontalThreat)
                                        {
                                                CNetObj_PlayerInput Future = Input;
                                                if(PredictFreeze(Core, Future, m_pClient->Collision(), false, nullptr, g_HookRescuePredictTicks))
                                                        ShouldHook = true;
                                        }
                                        else if(PredictFreeze(Core, Input, m_pClient->Collision(), false, nullptr, g_HookRescuePredictTicks))
                                                ShouldHook = true;

                                        if(ShouldHook)
                                        {
                                                vec2 NewTarget(0.0f, 0.0f);
                                                bool SafeTarget = false;
                                                if(TryHookRescue(Core, Input, HookThreatDir, m_pClient->Collision(), HookLength, RageState.m_LastTarget, &NewTarget, &SafeTarget))
                                                {
                                                        RageState.m_Active = true;
                                                        RageState.m_LastTarget = NewTarget;
                                                        RageState.m_LastThreatDir = HookThreatDir;
                                                        RageState.m_HoldTicks = maximum(RageState.m_HoldTicks, SafeTarget ? 8 : 4);
                                                }
                                        }
                                }

                                if(RageState.m_Active)
                                {
                                        bool KeepHook = DangerLong;

                                        if(!DangerLong)
                                        {
                                                if(RageState.m_HoldTicks > 0)
                                                {
                                                        RageState.m_HoldTicks--;
                                                        KeepHook = true;
                                                }
                                                else
                                                {
                                                        CNetObj_PlayerInput ReleaseTest = Input;
                                                        ReleaseTest.m_Hook = 0;
                                                        if(PredictFreeze(Core, ReleaseTest, m_pClient->Collision(), true, nullptr, g_PredictFreezeTicks))
                                                        {
                                                                KeepHook = true;
                                                                RageState.m_HoldTicks = 2;
                                                        }
                                                }

                                                if(KeepHook && length(RageState.m_LastThreatDir) > 0.01f)
                                                {
                                                        CNetObj_PlayerInput Candidate = Input;
                                                        vec2 NewTarget(0.0f, 0.0f);
                                                        bool SafeTarget = false;
                                                        if(TryHookRescue(Core, Candidate, RageState.m_LastThreatDir, m_pClient->Collision(), HookLength, RageState.m_LastTarget, &NewTarget, &SafeTarget))
                                                        {
                                                                Input = Candidate;
                                                                RageState.m_LastTarget = NewTarget;
                                                                if(SafeTarget)
                                                                        RageState.m_HoldTicks = maximum(RageState.m_HoldTicks, 4);
                                                        }
                                                }
                                        }
                                        else
                                        {
                                                RageState.m_HoldTicks = maximum(RageState.m_HoldTicks, 4);
                                        }

                                        if(KeepHook)
                                        {
                                                if(!ValidateHookTarget(Core, RageState.m_LastTarget, m_pClient->Collision(), HookLength))
                                                {
                                                        RageState.Reset();
                                                        Input.m_Hook = 0;
                                                }
                                                else
                                                {
                                                        Input.m_Hook = 1;
                                                        Input.m_TargetX = (int)RageState.m_LastTarget.x;
                                                        Input.m_TargetY = (int)RageState.m_LastTarget.y;

                                                        CNetObj_PlayerInput SafetyTest = Input;
                                                        if(PredictFreeze(Core, SafetyTest, m_pClient->Collision(), false, nullptr, g_PredictFreezeTicks))
                                                        {
                                                                RageState.Reset();
                                                                Input.m_Hook = 0;
                                                        }
                                                }
                                        }
                                        else
                                        {
                                                RageState.Reset();
                                                Input.m_Hook = 0;
                                        }
                                }
                        }
                        else
                        {
                                RageState.Reset();
                        }
                }
                // stress testing
#ifdef CONF_DEBUG
		if(g_Config.m_DbgStress)
		{
			float t = Client()->LocalTime();
			mem_zero(&m_aInputData[g_Config.m_ClDummy], sizeof(m_aInputData[0]));

			m_aInputData[g_Config.m_ClDummy].m_Direction = ((int)t / 2) & 1;
			m_aInputData[g_Config.m_ClDummy].m_Jump = ((int)t);
			m_aInputData[g_Config.m_ClDummy].m_Fire = ((int)(t * 10));
			m_aInputData[g_Config.m_ClDummy].m_Hook = ((int)(t * 2)) & 1;
			m_aInputData[g_Config.m_ClDummy].m_WantedWeapon = ((int)t) % NUM_WEAPONS;
			m_aInputData[g_Config.m_ClDummy].m_TargetX = (int)(std::sin(t * 3) * 100.0f);
			m_aInputData[g_Config.m_ClDummy].m_TargetY = (int)(std::cos(t * 3) * 100.0f);
		}
#endif
		// check if we need to send input
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_Direction != m_aLastData[g_Config.m_ClDummy].m_Direction;
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_Jump != m_aLastData[g_Config.m_ClDummy].m_Jump;
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_Fire != m_aLastData[g_Config.m_ClDummy].m_Fire;
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_Hook != m_aLastData[g_Config.m_ClDummy].m_Hook;
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_WantedWeapon != m_aLastData[g_Config.m_ClDummy].m_WantedWeapon;
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_NextWeapon != m_aLastData[g_Config.m_ClDummy].m_NextWeapon;
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_PrevWeapon != m_aLastData[g_Config.m_ClDummy].m_PrevWeapon;
		Send = Send || time_get() > m_LastSendTime + time_freq() / 25; // send at least 25 Hz
		Send = Send || (m_pClient->m_Snap.m_pLocalCharacter && m_pClient->m_Snap.m_pLocalCharacter->m_Weapon == WEAPON_NINJA && (m_aInputData[g_Config.m_ClDummy].m_Direction || m_aInputData[g_Config.m_ClDummy].m_Jump || m_aInputData[g_Config.m_ClDummy].m_Hook));
	}

	// copy and return size
	m_aLastData[g_Config.m_ClDummy] = m_aInputData[g_Config.m_ClDummy];

	if(!Send)
		return 0;

	m_LastSendTime = time_get();
	mem_copy(pData, &m_aInputData[g_Config.m_ClDummy], sizeof(m_aInputData[0]));
	return sizeof(m_aInputData[0]);
}

void CControls::OnRender()
{
	if(Client()->State() != IClient::STATE_ONLINE && Client()->State() != IClient::STATE_DEMOPLAYBACK)
		return;

	if(g_Config.m_ClAutoswitchWeaponsOutOfAmmo && !GameClient()->m_GameInfo.m_UnlimitedAmmo && m_pClient->m_Snap.m_pLocalCharacter)
	{
		// Keep track of ammo count, we know weapon ammo only when we switch to that weapon, this is tracked on server and protocol does not track that
		m_aAmmoCount[maximum(0, m_pClient->m_Snap.m_pLocalCharacter->m_Weapon % NUM_WEAPONS)] = m_pClient->m_Snap.m_pLocalCharacter->m_AmmoCount;
		// Autoswitch weapon if we're out of ammo
		if(m_aInputData[g_Config.m_ClDummy].m_Fire % 2 != 0 &&
			m_pClient->m_Snap.m_pLocalCharacter->m_AmmoCount == 0 &&
			m_pClient->m_Snap.m_pLocalCharacter->m_Weapon != WEAPON_HAMMER &&
			m_pClient->m_Snap.m_pLocalCharacter->m_Weapon != WEAPON_NINJA)
		{
			int Weapon;
			for(Weapon = WEAPON_LASER; Weapon > WEAPON_GUN; Weapon--)
			{
				if(Weapon == m_pClient->m_Snap.m_pLocalCharacter->m_Weapon)
					continue;
				if(m_aAmmoCount[Weapon] > 0)
					break;
			}
			if(Weapon != m_pClient->m_Snap.m_pLocalCharacter->m_Weapon)
				m_aInputData[g_Config.m_ClDummy].m_WantedWeapon = Weapon + 1;
		}
	}

	// update target pos
	if(m_pClient->m_Snap.m_pGameInfoObj && !m_pClient->m_Snap.m_SpecInfo.m_Active)
	{
		// make sure to compensate for smooth dyncam to ensure the cursor stays still in world space if zoomed
		vec2 DyncamOffsetDelta = m_pClient->m_Camera.m_DyncamTargetCameraOffset - m_pClient->m_Camera.m_aDyncamCurrentCameraOffset[g_Config.m_ClDummy];
		float Zoom = m_pClient->m_Camera.m_Zoom;
		m_aTargetPos[g_Config.m_ClDummy] = m_pClient->m_LocalCharacterPos + m_aMousePos[g_Config.m_ClDummy] - DyncamOffsetDelta + DyncamOffsetDelta / Zoom;
	}
	else if(m_pClient->m_Snap.m_SpecInfo.m_Active && m_pClient->m_Snap.m_SpecInfo.m_UsePosition)
	{
		m_aTargetPos[g_Config.m_ClDummy] = m_pClient->m_Snap.m_SpecInfo.m_Position + m_aMousePos[g_Config.m_ClDummy];
	}
	else
	{
		m_aTargetPos[g_Config.m_ClDummy] = m_aMousePos[g_Config.m_ClDummy];
	}
}

bool CControls::OnCursorMove(float x, float y, IInput::ECursorType CursorType)
{
	if(m_pClient->m_Snap.m_pGameInfoObj && (m_pClient->m_Snap.m_pGameInfoObj->m_GameStateFlags & GAMESTATEFLAG_PAUSED))
		return false;

	if(CursorType == IInput::CURSOR_JOYSTICK && g_Config.m_InpControllerAbsolute && m_pClient->m_Snap.m_pGameInfoObj && !m_pClient->m_Snap.m_SpecInfo.m_Active)
	{
		vec2 AbsoluteDirection;
		if(Input()->GetActiveJoystick()->Absolute(&AbsoluteDirection.x, &AbsoluteDirection.y))
			m_aMousePos[g_Config.m_ClDummy] = AbsoluteDirection * GetMaxMouseDistance();
		return true;
	}

	float Factor = 1.0f;
	if(g_Config.m_ClDyncam && g_Config.m_ClDyncamMousesens)
	{
		Factor = g_Config.m_ClDyncamMousesens / 100.0f;
	}
	else
	{
		switch(CursorType)
		{
		case IInput::CURSOR_MOUSE:
			Factor = g_Config.m_InpMousesens / 100.0f;
			break;
		case IInput::CURSOR_JOYSTICK:
			Factor = g_Config.m_InpControllerSens / 100.0f;
			break;
		default:
			dbg_msg("assert", "CControls::OnCursorMove CursorType %d", (int)CursorType);
			dbg_break();
			break;
		}
	}

	if(m_pClient->m_Snap.m_SpecInfo.m_Active && m_pClient->m_Snap.m_SpecInfo.m_SpectatorId < 0)
		Factor *= m_pClient->m_Camera.m_Zoom;

	m_aMousePos[g_Config.m_ClDummy] += vec2(x, y) * Factor;
	ClampMousePos();
	return true;
}

void CControls::ClampMousePos()
{
	if(m_pClient->m_Snap.m_SpecInfo.m_Active && m_pClient->m_Snap.m_SpecInfo.m_SpectatorId < 0)
	{
		m_aMousePos[g_Config.m_ClDummy].x = clamp(m_aMousePos[g_Config.m_ClDummy].x, -201.0f * 32, (Collision()->GetWidth() + 201.0f) * 32.0f);
		m_aMousePos[g_Config.m_ClDummy].y = clamp(m_aMousePos[g_Config.m_ClDummy].y, -201.0f * 32, (Collision()->GetHeight() + 201.0f) * 32.0f);
	}
	else
	{
		const float MouseMin = GetMinMouseDistance();
		const float MouseMax = GetMaxMouseDistance();

		float MouseDistance = length(m_aMousePos[g_Config.m_ClDummy]);
		if(MouseDistance < 0.001f)
		{
			m_aMousePos[g_Config.m_ClDummy].x = 0.001f;
			m_aMousePos[g_Config.m_ClDummy].y = 0;
			MouseDistance = 0.001f;
		}
		if(MouseDistance < MouseMin)
			m_aMousePos[g_Config.m_ClDummy] = normalize_pre_length(m_aMousePos[g_Config.m_ClDummy], MouseDistance) * MouseMin;
		MouseDistance = length(m_aMousePos[g_Config.m_ClDummy]);
		if(MouseDistance > MouseMax)
			m_aMousePos[g_Config.m_ClDummy] = normalize_pre_length(m_aMousePos[g_Config.m_ClDummy], MouseDistance) * MouseMax;
	}
}

float CControls::GetMinMouseDistance() const
{
	return g_Config.m_ClDyncam ? g_Config.m_ClDyncamMinDistance : g_Config.m_ClMouseMinDistance;
}

float CControls::GetMaxMouseDistance() const
{
	float CameraMaxDistance = 200.0f;
	float FollowFactor = (g_Config.m_ClDyncam ? g_Config.m_ClDyncamFollowFactor : g_Config.m_ClMouseFollowfactor) / 100.0f;
	float DeadZone = g_Config.m_ClDyncam ? g_Config.m_ClDyncamDeadzone : g_Config.m_ClMouseDeadzone;
	float MaxDistance = g_Config.m_ClDyncam ? g_Config.m_ClDyncamMaxDistance : g_Config.m_ClMouseMaxDistance;
	return minimum((FollowFactor != 0 ? CameraMaxDistance / FollowFactor + DeadZone : MaxDistance), MaxDistance);
}

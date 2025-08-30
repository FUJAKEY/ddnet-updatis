#include <engine/shared/config.h>
#include <game/localization.h>

#include "menus.h"

void CMenus::RenderSettingsFujix(CUIRect MainView)
{
        const float LineSize = 24.0f;
        MainView.Margin(10.0f, &MainView);
        DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClAvoidFreeze, Localize("Avoid freeze"), &g_Config.m_ClAvoidFreeze, &MainView, LineSize);
        DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClAvoidFreezeHook, Localize("Avoid freeze cancel hook"), &g_Config.m_ClAvoidFreezeHook, &MainView, LineSize);
}

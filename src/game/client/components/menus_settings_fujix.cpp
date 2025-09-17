#include <base/math.h>
#include <engine/shared/config.h>
#include <game/localization.h>

#include "menus.h"

void CMenus::RenderSettingsFujix(CUIRect MainView)
{
        const float LineSize = 24.0f;
        MainView.Margin(10.0f, &MainView);
        DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClAvoidFreeze, Localize("Avoid freeze"), &g_Config.m_ClAvoidFreeze, &MainView, LineSize);
        DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClAvoidFreezeHook, Localize("Avoid freeze cancel hook"), &g_Config.m_ClAvoidFreezeHook, &MainView, LineSize);

        CUIRect Row, Label, Button;
        MainView.HSplitTop(LineSize, &Row, &MainView);
        Row.VSplitRight(180.0f, &Label, &Button);
        Ui()->DoLabel(&Label, Localize("Gores bot"), 14.0f, TEXTALIGN_ML);

        Button.HMargin(2.0f, &Button);
        static CButtonContainer s_GoresBotButton;
        const int Mode = clamp(g_Config.m_ClGoresBot, 0, 2);
        const char *apModeNames[] = {
                Localize("Off"),
                Localize("Legit"),
                Localize("Rage"),
        };

        if(DoButton_Menu(&s_GoresBotButton, apModeNames[Mode], 0, &Button))
        {
                g_Config.m_ClGoresBot = (Mode + 1) % 3;
        }
}

SECTIONS
{
    gdx.widescreen.transition 0x0C4F0590 : {
        *widescreen_transition_matte.o(gdx.func)
    }
    gdx.widescreen.result 0x0C4F0640 : {
        *widescreen_result_black.o(gdx.func)
    }
    gdx.widescreen.hud 0x0C4F0700 : {
        *widescreen_hud.o(gdx.hud)
    }
}

ASSERT(SIZEOF(gdx.widescreen.transition) <= 0xB0,
       "widescreen transition overlaps result helper")
ASSERT(SIZEOF(gdx.widescreen.result) <= 0xC0,
       "widescreen result helper overlaps HUD helper")

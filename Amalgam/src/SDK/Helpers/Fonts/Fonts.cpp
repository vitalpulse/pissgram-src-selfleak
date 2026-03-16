#include "Fonts.h"

void CFonts::Reload(float flDPI)
{
	//m_mFonts[FONT_ESP] = { "Tahoma", int(12.f * flDPI), 0 }; // ESP Font
	//m_mFonts[FONT_FLAGS] = { "Small Fonts", int(11.f * flDPI), 0 }; // Flags Font
	//m_mFonts[FONT_TAGS] = { "Tahoma", int(12.f * flDPI), 0, FONTFLAG_ADDITIVE }; // Playerlist Tag Font
	//m_mFonts[FONT_INDICATORS] = { "Tahoma", int(13.f * flDPI), 0 }; // Indicators Font


	m_mFonts[FONT_ESP] = { Vars::Fonts::FONT_NAME::szName.Value.c_str(), int(Vars::Fonts::FONT_NAME::nTall.Value), Vars::Fonts::FONT_NAME::nFlags.Value, Vars::Fonts::FONT_NAME::nWeight.Value };
	m_mFonts[FONT_FLAGS] = { Vars::Fonts::FONT_FLAGS::szName.Value.c_str(), int(Vars::Fonts::FONT_FLAGS::nTall.Value), Vars::Fonts::FONT_FLAGS::nFlags.Value, Vars::Fonts::FONT_FLAGS::nWeight.Value };
	m_mFonts[FONT_TAGS] = { Vars::Fonts::FONT_TAGS::szName.Value.c_str(), int(Vars::Fonts::FONT_TAGS::nTall.Value), Vars::Fonts::FONT_TAGS::nFlags.Value, Vars::Fonts::FONT_TAGS::nWeight.Value }; // Playerlist Tag Font
	m_mFonts[FONT_INDICATORS] = { Vars::Fonts::FONT_INDICATORS::szName.Value.c_str(), int(Vars::Fonts::FONT_INDICATORS::nTall.Value), Vars::Fonts::FONT_INDICATORS::nFlags.Value, Vars::Fonts::FONT_INDICATORS::nWeight.Value };

	for (auto& [_, fFont] : m_mFonts)
	{
		if (fFont.m_dwFont = I::MatSystemSurface->CreateFont())
			I::MatSystemSurface->SetFontGlyphSet(fFont.m_dwFont, fFont.m_szName, fFont.m_nTall, fFont.m_nWeight, 0, 0, fFont.m_nFlags);
	}
}

const Font_t& CFonts::GetFont(EFonts eFont)
{
	return m_mFonts[eFont];
}
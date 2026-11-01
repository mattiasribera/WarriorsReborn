#include "theme.h"

namespace launcher {

extern const unsigned char inter_regular[], inter_semibold[];
extern const unsigned int inter_regular_size, inter_semibold_size;

Fonts fonts;

static ImVec4 rgb(int r, int g, int b, float a = 1) { return ImVec4(r / 255.f, g / 255.f, b / 255.f, a); }

namespace col {
/* The Warriors red #c8102e (the icon's), lighter for hover and for small text on the dark panels,
 * darker when pressed; white on it. Errors are orange so they never read as a selection. */
const ImVec4 accent = rgb(200, 16, 46), accent_hover = rgb(222, 38, 68), accent_active = rgb(160, 10, 34);
const ImVec4 accent_text = rgb(240, 84, 104);
const ImVec4 on_accent = rgb(255, 255, 255), text = rgb(236, 238, 242), dim = rgb(158, 164, 176), nav = rgb(204, 208, 216);
const ImVec4 good = rgb(96, 204, 120), bad = rgb(255, 140, 66), warn = rgb(236, 196, 84);
const ImVec4 card = rgb(18, 20, 26, 0.70f), sidebar = rgb(10, 11, 15, 0.78f), sidebar_solid = rgb(16, 17, 21);
const ImVec4 well = rgb(0, 0, 0, 0.28f), hover = rgb(255, 255, 255, 0.07f), hover_strong = rgb(255, 255, 255, 0.22f);
const ImVec4 off = rgb(255, 255, 255, 0.16f), line = rgb(255, 255, 255, 0.07f), shade = rgb(0, 0, 0, 0.45f);
}

static float font_density = 1;

static ImFont *add(const unsigned char *data, unsigned int size, float px) {
    static const ImWchar ranges[] = {
        0x0020, 0x00FF, /* Basic Latin, Latin-1 (Español, Français) */
        0x0100, 0x017F, /* Latin Extended-A */
        0x2010, 0x2027, /* dashes, quotes, bullet, ellipsis */
        0x2190, 0x2193, /* arrows */
        0,
    };
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;
    cfg.OversampleH = 2;
    cfg.OversampleV = 1;
    cfg.RasterizerDensity = font_density; /* sharp glyphs at the interface scale, same metrics */
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF((void *)data, (int)size, px, &cfg, ranges);
}

void theme_fonts(float density) {
    font_density = density;
    ImGui::GetIO().Fonts->Clear();
    fonts.body = add(inter_regular, inter_regular_size, 18);
    fonts.hint = add(inter_regular, inter_regular_size, 15.5f);
    fonts.nav = add(inter_semibold, inter_semibold_size, 19);
    fonts.heading = add(inter_semibold, inter_semibold_size, 25);
    fonts.title = add(inter_semibold, inter_semibold_size, 23);
    fonts.play = add(inter_semibold, inter_semibold_size, 26);
    fonts.label = add(inter_semibold, inter_semibold_size, 17);
    fonts.caps = add(inter_semibold, inter_semibold_size, 13);
    fonts.hero = add(inter_semibold, inter_semibold_size, 50);
    ImGui::GetIO().FontDefault = fonts.body;
}

void theme_init(float density) {
    theme_fonts(density);

    ImGuiStyle &s = ImGui::GetStyle();
    s = ImGuiStyle();
    s.WindowPadding = ImVec2(24, 20);
    s.FramePadding = ImVec2(12, 8);
    s.ItemSpacing = ImVec2(12, 10);
    s.ItemInnerSpacing = ImVec2(10, 6);
    s.ScrollbarSize = 12;
    s.GrabMinSize = 14;
    s.WindowBorderSize = 0;
    s.ChildBorderSize = 0;
    s.PopupBorderSize = 1;
    s.FrameBorderSize = 0;
    s.WindowRounding = 10;
    s.ChildRounding = 10;
    s.FrameRounding = 7;
    s.PopupRounding = 8;
    s.ScrollbarRounding = 8;
    s.GrabRounding = 7;
    s.TabRounding = 7;
    s.SelectableTextAlign = ImVec2(0, 0.5f);

    ImVec4 *c = s.Colors;
    const ImVec4 bg = rgb(14, 15, 18), frame = rgb(255, 255, 255, 0.08f), frame_hover = rgb(255, 255, 255, 0.13f),
                 frame_active = rgb(255, 255, 255, 0.18f);
    c[ImGuiCol_Text] = col::text;
    c[ImGuiCol_TextDisabled] = col::dim;
    c[ImGuiCol_WindowBg] = bg;
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = rgb(32, 35, 42);
    c[ImGuiCol_Border] = rgb(255, 255, 255, 0.10f);
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = frame;
    c[ImGuiCol_FrameBgHovered] = frame_hover;
    c[ImGuiCol_FrameBgActive] = frame_active;
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_TitleBgCollapsed] = bg;
    c[ImGuiCol_MenuBarBg] = bg;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = frame_hover;
    c[ImGuiCol_ScrollbarGrabHovered] = frame_active;
    c[ImGuiCol_ScrollbarGrabActive] = col::accent;
    c[ImGuiCol_CheckMark] = col::accent;
    c[ImGuiCol_SliderGrab] = col::accent;
    c[ImGuiCol_SliderGrabActive] = col::accent_hover;
    c[ImGuiCol_Button] = frame;
    c[ImGuiCol_ButtonHovered] = frame_hover;
    c[ImGuiCol_ButtonActive] = frame_active;
    c[ImGuiCol_Header] = rgb(200, 16, 46, 0.30f);
    c[ImGuiCol_HeaderHovered] = rgb(200, 16, 46, 0.40f);
    c[ImGuiCol_HeaderActive] = rgb(200, 16, 46, 0.55f);
    c[ImGuiCol_Separator] = col::line;
    c[ImGuiCol_SeparatorHovered] = c[ImGuiCol_SeparatorActive] = col::accent;
    c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered] = c[ImGuiCol_ResizeGripActive] = col::accent;
    c[ImGuiCol_Tab] = frame;
    c[ImGuiCol_TabHovered] = frame_hover;
    c[ImGuiCol_TabSelected] = col::accent;
    c[ImGuiCol_TextSelectedBg] = rgb(200, 16, 46, 0.45f);
    c[ImGuiCol_NavCursor] = col::accent;
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.6f);
}

} // namespace launcher

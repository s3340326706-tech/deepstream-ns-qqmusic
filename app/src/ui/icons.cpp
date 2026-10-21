#include "ui/icons.hpp"

#include <borealis/core/application.hpp>
#include <borealis/core/font.hpp>
#include <borealis/core/touch/tap_gesture.hpp>

namespace ui {

namespace {

// Material Icons 字体 id（borealis 启动时已注册）
int mdi_font()
{
    static int id     = -2;
    static bool tried = false;
    if (!tried)
    {
        tried = true;
        id    = brls::Application::getFont(brls::FONT_MATERIAL_ICONS);
    }
    return id;
}

// 图标字形统一往上抬一点点：Material Icons 的视觉重心比几何中心低
constexpr float kGlyphYOffset = -0.02f;

}  // namespace

// ─────────────────────────────────────────────────────────────
// Icon：只画字形
// ─────────────────────────────────────────────────────────────
Icon::Icon(const char* glyph, NVGcolor color, float size)
    : glyph_(glyph ? glyph : ""), color_(color), size_(size)
{
    setWidth(size * 1.25f);
    setHeight(size * 1.25f);
    setFocusable(false);
}

void Icon::setGlyph(const char* g) { glyph_ = g ? g : ""; }

void Icon::setIconColor(NVGcolor c) { color_ = c; }

void Icon::setIconSize(float s)
{
    size_ = s;
    setWidth(s * 1.25f);
    setHeight(s * 1.25f);
}

void Icon::draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style,
                brls::FrameContext*)
{
    const int f = mdi_font();
    if (f < 0 || glyph_.empty()) return;

    nvgFontFaceId(vg, f);
    nvgFontSize(vg, size_);
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, color_);
    nvgText(vg, x + width * 0.5f, y + height * 0.5f + size_ * kGlyphYOffset,
            glyph_.c_str(), nullptr);
}

// ─────────────────────────────────────────────────────────────
// IconButton：背景自己画，字形自己画，整块都能点
// ─────────────────────────────────────────────────────────────
IconButton::IconButton(const char* glyph, float icon_size, float box_size)
    : glyph_(glyph ? glyph : ""), icon_size_(icon_size)
{
    setWidth(box_size);
    setHeight(box_size);
    setCornerRadius(box_size * 0.30f);
    setFocusable(true);

    // ⚠️ 必须挂触摸手势。
    // brls::Button 在构造函数里自带 TapGestureRecognizer，而 Box 没有 ——
    // Box 只靠 registerClickAction 注册的是"手柄 A 键"的 action，
    // 触摸点上去 hitTest 能命中，但没有任何东西把落点翻译成一次点击，
    // 表现就是整个按钮摸不动（头像/播放键全哑）。这一行是触摸的全部前提。
    auto* tap = new brls::TapGestureRecognizer(this);
    tap->getTapGestureEvent().subscribe([this](brls::TapGestureStatus st, brls::Sound*) {
        // UNSURE = 手指按下但还没抬起；END/FAILED/INTERRUPTED = 已经离开
        const bool down = (st.state == brls::GestureState::UNSURE);
        if (down == pressed_) return;
        pressed_ = down;
        refresh();
    });
    addGestureRecognizer(tap);

    refresh();
}

void IconButton::refresh()
{
    // 三态亮度：常态 < 焦点 < 按下。触摸没有焦点态，靠按下这点变化给手感。
    int a;
    if (emphasized_)
        a = pressed_ ? 200 : (focused_ ? 255 : 232);
    else
        a = pressed_ ? 74 : (focused_ ? 46 : 18);
    if (emphasized_)
        // 主操作按钮用品牌绿（QQ 音乐车机版的播放键就是绿色实心圆）
        setBackgroundColor(nvgRGBA(49, 194, 124, a));
    else
        setBackgroundColor(nvgRGBA(255, 255, 255, a));
}

void IconButton::setGlyph(const char* g)
{
    // tick 每 250ms 会按播放状态刷一次图标，值没变就别动
    std::string s = g ? g : "";
    if (s == glyph_) return;
    glyph_ = std::move(s);
}

void IconButton::setEmphasized(bool on)
{
    if (emphasized_ == on) return;   // 切页时会连调几次，值没变就别重设背景
    emphasized_ = on;
    refresh();
}

void IconButton::setIconColor(NVGcolor c)
{
    color_        = c;
    custom_color_ = true;
}

void IconButton::onFocusGained()
{
    focused_ = true;
    refresh();
    brls::Box::onFocusGained();
}

void IconButton::onFocusLost()
{
    focused_ = false;
    refresh();
    brls::Box::onFocusLost();
}

void IconButton::draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style style,
                      brls::FrameContext* ctx)
{
    brls::Box::draw(vg, x, y, width, height, style, ctx);

    const int f = mdi_font();
    if (f < 0 || glyph_.empty()) return;

    NVGcolor col;
    if (emphasized_)
        col = nvgRGBA(255, 255, 255, 255);                  // 绿底白图标
    else if (custom_color_)
        col = color_;
    else
        col = focused_ ? nvgRGBA(255, 255, 255, 255) : color_;

    nvgFontFaceId(vg, f);
    nvgFontSize(vg, icon_size_);
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, col);
    nvgText(vg, x + width * 0.5f, y + height * 0.5f + icon_size_ * kGlyphYOffset,
            glyph_.c_str(), nullptr);
}

}  // namespace ui

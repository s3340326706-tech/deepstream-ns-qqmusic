// icons.hpp —— 全局图标层
//
// 所有界面元素（播放/暂停、上一首、下一首、搜索、音量、队列……）一律从这里取，
// 不要在各个界面文件里手写码点或自己画。
//
// 字族用 borealis 自带的 Material Icons（RomFS 里已有 material/MaterialIcons-Regular.ttf），
// 好处：单色矢量、不用打包 png、任意尺寸不糊、整机风格天然统一。
#pragma once

#include <borealis.hpp>

namespace ui {

// ── 图标码点（Material Icons，已实测字体 cmap 里全部存在）──
namespace glyph {
constexpr const char* Play      = "\uE037";  // play_arrow
constexpr const char* Pause     = "\uE034";  // pause
constexpr const char* Prev      = "\uE045";  // skip_previous
constexpr const char* Next      = "\uE044";  // skip_next
constexpr const char* Search    = "\uE8B6";  // search
constexpr const char* VolumeUp  = "\uE050";  // volume_up
constexpr const char* VolumeOff = "\uE04F";  // volume_off
constexpr const char* MusicNote = "\uE405";  // music_note
constexpr const char* Back      = "\uE5C4";  // arrow_back
constexpr const char* Close     = "\uE5CD";  // close
constexpr const char* Refresh   = "\uE5D5";  // refresh
constexpr const char* Library   = "\uE030";  // library_music
constexpr const char* Queue     = "\uE03D";  // queue_music
constexpr const char* Repeat    = "\uE040";  // repeat
constexpr const char* Shuffle   = "\uE043";  // shuffle
constexpr const char* Equalizer = "\uE1B8";  // graphic_eq
constexpr const char* Settings  = "\uE8B8";  // settings
constexpr const char* Loading   = "\uE028";  // autorenew
constexpr const char* More      = "\uE5D3";  // more_horiz
constexpr const char* Info      = "\uE88E";  // info_outline
constexpr const char* ChevronLeft  = "\uE5CB";  // chevron_left
constexpr const char* ChevronRight = "\uE5CC";  // chevron_right
constexpr const char* Home         = "\uE88A";  // home
constexpr const char* Album        = "\uE019";  // album
constexpr const char* Folder       = "\uE2C7";  // folder
}  // namespace glyph

// ── 尺寸档：新增界面只用这四档，别再临时拍数字 ──
constexpr float kIconSm = 20.0f;
constexpr float kIconMd = 28.0f;
constexpr float kIconLg = 36.0f;
constexpr float kIconXl = 46.0f;

// ── 按钮直径档 ──
constexpr float kBtnSm = 48.0f;
constexpr float kBtnMd = 60.0f;
constexpr float kBtnLg = 76.0f;

// ── 颜色 ──
inline const NVGcolor kIconOn     = {1.0f, 1.0f, 1.0f, 0.90f};   // 正常
inline const NVGcolor kIconMuted  = {1.0f, 1.0f, 1.0f, 0.45f};   // 次要 / 禁用
inline const NVGcolor kIconOnDark = {0.09f, 0.09f, 0.11f, 1.0f}; // 白底上的图标

// ── 品牌色板（对齐 QQ 音乐车机版：纯黑底 + 绿强调）──
inline const NVGcolor kAccent      = {0.192f, 0.761f, 0.486f, 1.0f};  // #31C27C
inline const NVGcolor kTextPrimary = {0.949f, 0.949f, 0.957f, 1.0f};  // #F2F2F4
inline const NVGcolor kTextMuted   = {0.478f, 0.478f, 0.502f, 1.0f};  // #7A7A80
// 纯黑底：车机版是真正的 #000，不是深灰。浅一点会把卡片的层次压没。
inline const NVGcolor kCanvas      = {0.0f, 0.0f, 0.0f, 1.0f};

// 只画一个字形，尺寸由 setWidth/setHeight 决定（居中绘制）。
// 不给子视图塞 Label：Label 会拦截触摸，导致外壳按钮点不到。
class Icon : public brls::View
{
  public:
    explicit Icon(const char* glyph, NVGcolor color = kIconOn, float size = kIconMd);

    void setGlyph(const char* glyph);
    void setIconColor(NVGcolor color);
    void setIconSize(float size);

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override;

  private:
    std::string glyph_;
    NVGcolor color_;
    float size_;
};

// 图标按钮：手柄可聚焦、触摸可点、焦点态自动高亮。
// 背景和字形都自己画，所以点击区域是整块，不会被内部子控件吞掉。
//   emphasized = true → 实心白底 + 深色图标（主操作：播放/暂停）
class IconButton : public brls::Box
{
  public:
    explicit IconButton(const char* glyph, float icon_size = kIconMd, float box_size = kBtnMd);

    void setGlyph(const char* glyph);
    void setEmphasized(bool on);
    void setIconColor(NVGcolor color);

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override;

    void onFocusGained() override;
    void onFocusLost() override;

  private:
    void refresh();

    std::string glyph_;
    float icon_size_;
    bool focused_      = false;
    bool pressed_      = false;   // 手指按在按钮上（触摸反馈）
    bool emphasized_   = false;
    bool custom_color_ = false;
    NVGcolor color_    = {1.0f, 1.0f, 1.0f, 0.90f};
};

}  // namespace ui

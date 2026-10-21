// card.hpp —— 车机版风格的卡片
//
// 为什么不用 brls::Button：
//   Button 的外观完全由主题决定（胶囊形灰底 + 主题描边），改不成"深色圆角卡片"；
//   而且 Button 内部的 Label 会参与焦点遍历，手柄一路按下来会停在文字上，
//   而不是停在"整块卡片"上。
//
// 所以这里用 Box 自绘：背景、焦点描边、强调色竖条全部自己画，
// 触摸手势自己也挂（Box 不像 Button 那样自带 TapGestureRecognizer —— 这是 v0.7.1
// 把整个界面摸不动的原因，务必保留构造里那三行）。
#pragma once

#include <borealis.hpp>

#include <string>

namespace ui {

// 一行卡片：可选左图标 + 主标题 + 副标题。
class CardRow : public brls::Box
{
  public:
    // glyph 传 nullptr 就不画左侧图标
    explicit CardRow(const std::string& title, const std::string& subtitle = "",
                     const char* glyph = nullptr);

    void setTitle(const std::string& t);
    void setSubtitle(const std::string& s);
    // 正在播放：标题变强调色 + 左侧多一条竖线
    void setAccent(bool on);
    // 首页那种居中的入口卡片
    void setCentered(bool on);
    void setFontSizes(float title, float subtitle);

    void draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style style,
              brls::FrameContext* ctx) override;
    void onFocusGained() override;
    void onFocusLost() override;

  private:
    void refresh();

    brls::Label* title_ = nullptr;
    brls::Label* sub_   = nullptr;
    brls::View* icon_   = nullptr;

    bool focused_     = false;
    bool pressed_     = false;
    bool accent_      = false;
    bool centered_    = false;
    float title_size_ = 22.0f;
    float sub_size_   = 16.0f;
};

}  // namespace ui

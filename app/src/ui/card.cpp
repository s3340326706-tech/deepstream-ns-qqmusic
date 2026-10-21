#include "ui/card.hpp"

#include <borealis/core/touch/tap_gesture.hpp>

#include "ui/icons.hpp"

namespace ui {

namespace {

// 卡片用的三档亮度。纯黑底上，卡片要比背景亮一档才浮得起来，
// 但又不能亮到抢内容 —— QQ 音乐车机版那套就是 #000 底 + #17181A 卡片。
constexpr float kRadius = 10.0f;

NVGcolor card_bg(bool accent, bool focused, bool pressed)
{
    if (accent)
    {
        if (pressed) return nvgRGB(34, 48, 42);
        if (focused) return nvgRGB(28, 44, 37);
        return nvgRGB(21, 32, 27);
    }
    if (pressed) return nvgRGB(38, 39, 42);
    if (focused) return nvgRGB(31, 32, 35);
    return nvgRGB(23, 24, 26);
}

}  // namespace

CardRow::CardRow(const std::string& title, const std::string& subtitle, const char* glyph)
{
    setAxis(brls::Axis::ROW);
    setAlignItems(brls::AlignItems::CENTER);
    setPadding(0, 22, 0, 22);
    setHeight(72);
    setMarginBottom(8);
    setFocusable(true);

    if (glyph && *glyph)
    {
        icon_ = new Icon(glyph, kIconMuted, kIconSm);
        icon_->setMarginRight(16);
        addView(icon_);
    }

    // 内层排版 Box：绝对不能被聚焦，否则手柄会停在文字框上而不是整块卡片
    auto* col = new brls::Box(brls::Axis::COLUMN);
    col->setGrow(1);
    col->setFocusable(false);
    col->setJustifyContent(brls::JustifyContent::CENTER);

    title_ = new brls::Label();
    title_->setText(title);
    title_->setFontSize(title_size_);
    title_->setTextColor(kTextPrimary);
    title_->setSingleLine(true);
    title_->setShrink(1);
    title_->setFocusable(false);
    col->addView(title_);

    sub_ = new brls::Label();
    sub_->setText(subtitle);
    sub_->setFontSize(sub_size_);
    sub_->setTextColor(kTextMuted);
    sub_->setSingleLine(true);
    sub_->setShrink(1);
    sub_->setFocusable(false);
    sub_->setMarginTop(4);
    if (subtitle.empty()) sub_->setVisibility(brls::Visibility::GONE);
    col->addView(sub_);

    addView(col);

    // Box 不像 brls::Button 那样自带手势识别器，这一挂是触摸的全部前提
    auto* tap = new brls::TapGestureRecognizer(this);
    tap->getTapGestureEvent().subscribe([this](brls::TapGestureStatus st, brls::Sound*) {
        const bool down = (st.state == brls::GestureState::UNSURE);
        if (down == pressed_) return;
        pressed_ = down;
    });
    addGestureRecognizer(tap);
}

void CardRow::setTitle(const std::string& t)
{
    if (!title_) return;
    if (title_->getFullText() == t) return;
    title_->setText(t);
}

void CardRow::setSubtitle(const std::string& s)
{
    if (!sub_) return;
    sub_->setText(s);
    sub_->setVisibility(s.empty() ? brls::Visibility::GONE : brls::Visibility::VISIBLE);
}

void CardRow::setAccent(bool on)
{
    accent_ = on;
    if (title_) title_->setTextColor(on ? kAccent : kTextPrimary);
}

void CardRow::setCentered(bool on)
{
    centered_ = on;
    if (!title_) return;
    title_->setHorizontalAlign(on ? brls::HorizontalAlign::CENTER : brls::HorizontalAlign::LEFT);
    if (sub_)
        sub_->setHorizontalAlign(on ? brls::HorizontalAlign::CENTER
                                    : brls::HorizontalAlign::LEFT);
}

void CardRow::setFontSizes(float title, float subtitle)
{
    title_size_ = title;
    sub_size_   = subtitle;
    if (title_) title_->setFontSize(title);
    if (sub_) sub_->setFontSize(subtitle);
}

void CardRow::onFocusGained()
{
    focused_ = true;
    if (icon_) static_cast<Icon*>(icon_)->setIconColor(kTextPrimary);
    brls::Box::onFocusGained();
}

void CardRow::onFocusLost()
{
    focused_ = false;
    if (icon_) static_cast<Icon*>(icon_)->setIconColor(kIconMuted);
    brls::Box::onFocusLost();
}

void CardRow::draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style style,
                   brls::FrameContext* ctx)
{
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, width, height, kRadius);
    nvgFillColor(vg, card_bg(accent_, focused_, pressed_));
    nvgFill(vg);

    // 正在播放：左侧一条绿色竖条，比整块变绿克制，也更像车机版的标记法
    if (accent_)
    {
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x + 2.0f, y + height * 0.30f, 4.0f, height * 0.40f, 2.0f);
        nvgFillColor(vg, kAccent);
        nvgFill(vg);
    }

    // 焦点描边。手柄移过来时必须看得出来停在哪 —— 掌机上没有鼠标指针
    if (focused_)
    {
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x + 1.5f, y + 1.5f, width - 3.0f, height - 3.0f, kRadius - 1.0f);
        nvgStrokeWidth(vg, 3.0f);
        nvgStrokeColor(vg, kAccent);
        nvgStroke(vg);
    }

    // ⚠️ 必须最后调：borealis 里唯一会画子视图的地方就是 Box::draw（内部遍历 children
    // 逐个 frame()）。这里覆盖了 draw 又不往下转，图标和文字就一个都不会出现 —
    // 整条链是 View::frame → draw()，没有别的兜底路径。
    brls::Box::draw(vg, x, y, width, height, style, ctx);
}

}  // namespace ui

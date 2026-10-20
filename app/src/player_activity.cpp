// player_activity.cpp —— 界面与交互
//
// 线程规则（borealis 硬要求）：
//   网络 / 解码 / 下载 一律在 brls::async 里跑，绝不能在主线程做。
//   任何 UI 改动都回到主线程，用 brls::sync 投递。
//   违反这条的表现是：界面卡死、转圈不响应、按 Home 退出时崩。

#include "player_activity.hpp"

#include <borealis/core/task.hpp>
#include <borealis/core/thread.hpp>

#include <atomic>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <dirent.h>
#include <fstream>
#include <memory>
#include <sys/stat.h>

#include "se/http.hpp"

namespace {

constexpr const char* kSourcePath  = "sdmc:/switch/se_sources/qqmusic.json";
// 内置副本：编进 nro 的 romfs。有它之后新装/升级都不用再往 SD 卡拷配置文件。
constexpr const char* kBuiltinSourcePath = "romfs:/sources/qqmusic.json";
constexpr const char* kAccountPath = "sdmc:/switch/se_sources/account.json";
// 外观偏好（目前只有背景图）和背景图目录
constexpr const char* kUiPrefPath = "sdmc:/switch/se_sources/ui.json";
constexpr const char* kBgDir      = "sdmc:/switch/se_sources/background";

// 同一个歌单页 60 秒内不重复同步：来回切页不该每次都打接口
constexpr int kSyncCooldownSec = 60;

// 一页渲染多少行。
// 这不是审美问题：歌单里 1404 首，如果整列表都建成 View（每行一个 Button，
// 内含 Label 和布局节点，队列行还有额外的 Box + 删除按钮），
// Switch 会直接爆内存 → 系统崩溃重启。所以所有列表一律按页渲染。
constexpr int kPageSize = 100;

// 外部配置缺的顶层段，从内置那份搬过来。
// 具体要解决的场景：v0.6 之前放好的 qqmusic.json 里没有 playlists / playlist_songs，
// 老文件不该让新功能直接消失。
void fill_missing(se::json& dst, const se::json& base) {
    if (!dst.is_object() || !base.is_object()) return;
    for (auto it = base.begin(); it != base.end(); ++it) {
        if (!dst.contains(it.key())) dst[it.key()] = it.value();
    }
}

// ─────────────────────────────────────────────────────────────
// 崩溃定位日志
// ─────────────────────────────────────────────────────────────
// Switch 上崩了只有一个"软件出错，重启"，既不给我堆栈也没有任何现场。
// 所以把关键步骤实时写到 SD 卡：重启后文件还在，最后一行就是崩之前
// 最后完成的那一步，能直接把范围缩到一个函数甚至一行。
// 定位完把 kDebugLog 改成 false 重编即可（编译期常量，会被整个优化掉）。
constexpr bool kDebugLog       = true;
constexpr const char* kLogPath = "sdmc:/switch/se_sources/deepstream.log";

long long log_ms() {
    static const auto t0 = std::chrono::steady_clock::now();
    return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
}

void dlog(const std::string& msg) {
    if (!kDebugLog) return;
    std::ofstream f(kLogPath, std::ios::app | std::ios::binary);
    if (!f) return;
    char head[32];
    std::snprintf(head, sizeof(head), "%7lldms ", log_ms());
    f << head << msg << '\n';
    f.flush();   // 崩溃来得突然，不 flush 等于没写
}

std::string clock_now() {
    std::time_t t = std::time(nullptr);
    std::tm lt{};
    localtime_r(&t, &lt);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", lt.tm_hour, lt.tm_min);
    return buf;
}

bool file_readable(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return (bool)f;
}

std::string to_lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// stb_image 能吃的那几种；不认识的直接跳过，免得列表里混进 txt
bool is_image_name(const std::string& name) {
    const auto dot = name.find_last_of('.');
    if (dot == std::string::npos) return false;
    const std::string ext = to_lower(name.substr(dot));
    return ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".bmp" || ext == ".tga";
}

// 背景图上的一层压暗渐变。
// 目的很具体：用户放什么图都得保证白字读得清，但也不能压成黑屏 ——
// 所以只做"上浅下深"的线性渐变，图本身还是要看得见的。
class Scrim : public brls::View
{
  public:
    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style, brls::FrameContext*) override
    {
        NVGpaint p = nvgLinearGradient(vg, x, y, x, y + height,
                                       nvgRGBA(6, 8, 12, 96),
                                       nvgRGBA(6, 8, 12, 190));
        nvgBeginPath(vg);
        nvgRect(vg, x, y, width, height);
        nvgFillPaint(vg, p);
        nvgFill(vg);
    }
};

// 正在播放的曲目用这个颜色标出来（和图标层的白/灰区分开，但仍是同一套冷色系）
const NVGcolor kNowPlayingColor = {0.49f, 0.84f, 0.86f, 1.0f};   // #7CD6DC
const NVGcolor kMutedColor      = {0.59f, 0.61f, 0.65f, 1.0f};

constexpr float kRowHeight = 64.0f;

std::string fmt_time(double sec) {
    if (sec < 0 || sec != sec) sec = 0;
    int total = (int)sec;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", total / 60, total % 60);
    return buf;
}

// 配置版本不匹配时，把话说清楚 —— 否则用户只会看到一句英文异常，以为是程序坏了
std::string friendly_error(const std::string& raw) {
    if (raw.find("does not implement playlists") != std::string::npos ||
        raw.find("does not implement playlist_songs") != std::string::npos)
        return "当前音源没有歌单接口（SD 卡上的 qqmusic.json 被覆盖成了旧版？删掉它即用内置版）";
    return raw;
}

std::string join_artist(const std::string& a) { return a.empty() ? "未知艺人" : a; }

// 音质档位的中文名，跟 QQ 音乐客户端里的称呼对齐，别让用户猜 128 是什么
std::string quality_label(const std::string& q) {
    if (q == "128") return "标准";
    if (q == "aac") return "标准(m4a)";
    if (q == "320") return "高品质";
    if (q == "flac") return "无损";
    return q.empty() ? "未知音质" : q;
}

std::string fmt_dur(long long ms) {
    if (ms <= 0) return "";
    return fmt_time((double)ms / 1000.0);
}

// 列表行的统一样式：所有页面都用同一个高/字号/间距，保证风格一致
brls::Button* make_row_button(const std::string& text, bool strong = false) {
    auto* row = new brls::Button();
    row->setText(text);
    row->setFontSize(strong ? 22.0f : 20.0f);
    row->setHeight(kRowHeight);
    row->setMarginBottom(6);
    if (!strong) row->setTextColor(nvgRGB(226, 229, 234));
    return row;
}

brls::Label* make_empty_label(const std::string& text) {
    auto* empty = new brls::Label();
    empty->setText(text);
    empty->setFontSize(20);
    empty->setTextColor(kMutedColor);
    return empty;
}

}  // namespace

// 250ms 刷新一次界面（进度、按钮图标、状态）
class PollTask : public brls::RepeatingTask
{
  public:
    explicit PollTask(PlayerActivity* a) : brls::RepeatingTask(250), act_(a) {}
    void run() override { act_->tick(); }

  private:
    PlayerActivity* act_;
};

PlayerActivity::PlayerActivity() {
    // 日志目录先确保在：没有 se_sources 时 dlog 会静默失败，那就等于没打日志
    mkdir("sdmc:/switch/se_sources", 0777);
    if (kDebugLog) std::remove(kLogPath);   // 每次启动一份新的，别让旧日志混淆现场
    dlog("ctor begin");

    // 音源描述：优先用 SD 卡上的外部文件（想自己改接口就改那份），
    // 没有就用打进 nro 的内置副本。两者都没有才报错。
    se::json src;
    bool external_ok = false;
    std::string ext_note;

    try {
        src         = se::Engine::load_file(kSourcePath);
        external_ok = true;
    } catch (const std::exception& e) {
        // 文件不存在是正常情况（大多数人不会放），不值得报错；
        // 语法坏了才需要提示 —— 否则用户改了文件却没生效，会一头雾水。
        const std::string m = e.what();
        if (m.find("file not found") == std::string::npos)
            ext_note = std::string("外部音源配置无效，已改用内置：") + m;
    }

    try {
        se::json builtin = se::Engine::load_file(kBuiltinSourcePath);
        if (external_ok) {
            fill_missing(src, builtin);   // 外部缺的段由内置补齐
            source_external_ = true;
        } else {
            src = std::move(builtin);
        }
    } catch (const std::exception& e) {
        if (!external_ok) last_error_ = std::string("内置音源损坏: ") + e.what();
    }

    if (!src.is_object() || src.empty()) {
        if (last_error_.empty()) last_error_ = "音源不可用：内置副本和外部配置都没读到";
        engine_ok_ = false;
    } else {
        try {
            se::Engine::validate(src);
            engine_       = std::make_unique<se::Engine>(src);
            engine_->opts.cainfo = "romfs:/cacert.pem";
            auto ck     = se::Engine::load_account(kAccountPath);
            account_ok_ = !ck.empty();
            engine_->set_cookies(ck);
            engine_ok_ = true;
        } catch (const std::exception& e) {
            last_error_ = std::string("音源加载失败: ") + e.what();
            engine_ok_  = false;
        }
    }

    if (!ext_note.empty()) status_text_ = ext_note;

    // 外观偏好 + 背景候选。目录不存在就顺手建一个，省得用户还要自己 mkdir。
    load_ui_pref();
    scan_backgrounds();

    alive_ = std::make_shared<std::atomic<bool>>(true);
    dlog(std::string("ctor done engine_ok=") + (engine_ok_ ? "1" : "0") +
         " account_ok=" + (account_ok_ ? "1" : "0") + " bg=" + bg_name_);
}

PlayerActivity::~PlayerActivity() {
    if (alive_) alive_->store(false);
    if (poll_) {
        poll_->stop();
        delete poll_;
        poll_ = nullptr;
    }
    hd::AudioEngine::get().unload();   // 退出时把缓冲/线程/解码器一起收干净
}

void PlayerActivity::set_status(const std::string& text) {
    status_text_ = text;
    refresh_status();
}

// ─────────────────────────────────────────────────────────────
// 界面骨架
// ─────────────────────────────────────────────────────────────
brls::View* PlayerActivity::createContentView() {
    auto* root = new brls::Box(brls::Axis::COLUMN);
    root->setPadding(24, 36, 24, 36);
    root->setGrow(1);

    // ——— 背景层 ———
    // 绝对定位 + 四边 0 → 铺满整个 Activity，且不参与流式布局，
    // 所以下面的 header / 列表 / 播放条排版完全不受影响。
    bg_image_ = new brls::Image();
    bg_image_->setPositionType(brls::PositionType::ABSOLUTE);
    bg_image_->setPositionTop(0);
    bg_image_->setPositionLeft(0);
    bg_image_->setPositionBottom(0);
    bg_image_->setPositionRight(0);
    bg_image_->setScalingType(brls::ImageScalingType::FILL);   // 保持比例填满（裁边，不变形）
    bg_image_->setAlpha(0.78f);
    bg_image_->setVisibility(brls::Visibility::GONE);
    root->addView(bg_image_);

    bg_scrim_ = new Scrim();
    bg_scrim_->setPositionType(brls::PositionType::ABSOLUTE);
    bg_scrim_->setPositionTop(0);
    bg_scrim_->setPositionLeft(0);
    bg_scrim_->setPositionBottom(0);
    bg_scrim_->setPositionRight(0);
    bg_scrim_->setVisibility(brls::Visibility::GONE);
    root->addView(bg_scrim_);

    // ——— 顶栏 ———
    auto* header = new brls::Box(brls::Axis::ROW);
    header->setAlignItems(brls::AlignItems::CENTER);
    header->setMarginBottom(16);
    this->header_ = header;   // 手柄左右键判断"焦点是否在这排按钮里"用

    auto* title = new brls::Label();
    title->setText("Deepstream");
    title->setFontSize(32);
    // 顶栏按钮变多以后标题被压窄过：当时表现为最后那个 "m" 单独掉到第二行。
    // 单行 + 不收缩，宁可挤按钮也不许压标题。
    title->setSingleLine(true);
    title->setShrink(0);
    header->addView(title);

    // 标题右边撑开，把按钮组推到最右；状态文字挪到下一行（见下面 root->addView(status_)）
    auto* spacer = new brls::Box(brls::Axis::ROW);
    spacer->setGrow(1);
    header->addView(spacer);

    // 导航：搜索 / 歌单 / 队列。当前所在页用实心白底标出来。
    nav_search_ = new ui::IconButton(ui::glyph::Search, ui::kIconMd, ui::kBtnMd);
    nav_search_->registerClickAction([this](brls::View*) {
        // 点放大镜 = 进搜索页 + 立刻弹输入法。
        // v0.6 加三页导航时这里只剩 switch_page，输入法那一步丢了 ——
        // 表现就是"按钮有按下反馈、但什么也没发生"。
        // IME 是模态 Activity，不能在触摸回调返回前压栈（同 rebuild 的规矩），延后一帧。
        this->post([this]() {
            if (this->page_ != Page::Search) this->switch_page(Page::Search);
            this->open_search_dialog();
        });
        return true;
    });
    header->addView(nav_search_);

    nav_library_ = new ui::IconButton(ui::glyph::Library, ui::kIconMd, ui::kBtnMd);
    nav_library_->setMarginLeft(12);
    nav_library_->registerClickAction([this](brls::View*) {
        const bool from_songs = (this->page_ == Page::Songs);
        this->switch_page(Page::Playlists);
        // 歌单由软件自己同步：每次进这个页面都拉一次账号里的最新歌单，
        // 只是 60 秒内重复进不会重复请求（见 load_playlists 的节流）。
        if (!from_songs) this->load_playlists();
        return true;
    });
    header->addView(nav_library_);

    nav_queue_ = new ui::IconButton(ui::glyph::Queue, ui::kIconMd, ui::kBtnMd);
    nav_queue_->setMarginLeft(12);
    nav_queue_->registerClickAction([this](brls::View*) {
        this->switch_page(Page::Queue);
        return true;
    });
    header->addView(nav_queue_);

    // 设置：外观（自定义背景）以后的音质、主题之类都往这里放
    nav_settings_ = new ui::IconButton(ui::glyph::Settings, ui::kIconMd, ui::kBtnMd);
    nav_settings_->setMarginLeft(12);
    nav_settings_->registerClickAction([this](brls::View*) {
        // 进设置页就重扫一次：用户刚往目录里拷完图，不用再额外点刷新
        this->scan_backgrounds();
        this->switch_page(Page::Settings);
        return true;
    });
    header->addView(nav_settings_);

    nav_back_ = new ui::IconButton(ui::glyph::Back, ui::kIconMd, ui::kBtnMd);
    nav_back_->setMarginLeft(12);
    nav_back_->setVisibility(brls::Visibility::GONE);   // 只在歌单歌曲页出现
    nav_back_->registerClickAction([this](brls::View*) {
        this->switch_page(Page::Playlists);
        return true;
    });
    header->addView(nav_back_);

    // 手动重新同步：歌单页重拉歌单列表，歌曲页重拉当前歌单内容
    nav_refresh_ = new ui::IconButton(ui::glyph::Refresh, ui::kIconMd, ui::kBtnMd);
    nav_refresh_->setMarginLeft(12);
    nav_refresh_->setVisibility(brls::Visibility::GONE);
    nav_refresh_->registerClickAction([this](brls::View*) {
        this->refresh_current_page();
        return true;
    });
    header->addView(nav_refresh_);

    // 状态行单独一行。以前它在顶栏里和标题抢位置，按钮一多就两个都变形：
    // 标题折行、状态文字被压成一团。挪下来以后两件事都解决了。
    status_ = new brls::Label();
    status_->setText("就绪");
    status_->setFontSize(16);
    status_->setTextColor(nvgRGB(150, 156, 166));
    status_->setSingleLine(true);
    status_->setMarginBottom(10);
    if (!engine_ok_ && !last_error_.empty()) status_->setText(last_error_);

    root->addView(header);
    root->addView(status_);

    // ——— 内容区（所有页面共用，切页时整块重建）———
    scroller_ = new brls::ScrollingFrame();
    scroller_->setGrow(1);

    content_box_ = new brls::Box(brls::Axis::COLUMN);
    scroller_->setContentView(content_box_);
    root->addView(scroller_);

    // ——— 底部播放条：左封面 + 右信息 ———
    auto* footer = new brls::Box(brls::Axis::ROW);
    footer->setAlignItems(brls::AlignItems::CENTER);
    footer->setMarginTop(16);
    footer->setPadding(16, 20, 16, 20);
    footer->setBackgroundColor(nvgRGBA(255, 255, 255, 14));
    footer->setCornerRadius(12);

    // 封面：固定 112×112，圆角靠外层 Box 兜（Image 自身不裁圆角）
    auto* cover_box = new brls::Box(brls::Axis::COLUMN);
    cover_box->setWidth(112);
    cover_box->setHeight(112);
    cover_box->setBackgroundColor(nvgRGBA(255, 255, 255, 20));
    cover_box->setCornerRadius(10);
    cover_box->setJustifyContent(brls::JustifyContent::CENTER);
    cover_box->setAlignItems(brls::AlignItems::CENTER);

    // 没封面时垫一个音符占位；封面到了再盖上去（Image 无图时什么都不画）
    cover_placeholder_ = new ui::Icon(ui::glyph::MusicNote, ui::kIconMuted, ui::kIconXl);
    cover_box->addView(cover_placeholder_);

    cover_ = new brls::Image();
    cover_->setPositionType(brls::PositionType::ABSOLUTE);
    cover_->setPositionTop(0);
    cover_->setPositionLeft(0);
    cover_->setWidth(112);
    cover_->setHeight(112);
    cover_->setScalingType(brls::ImageScalingType::FILL);
    cover_->setImageAlign(brls::ImageAlignment::CENTER);
    cover_box->addView(cover_);
    footer->addView(cover_box);

    auto* info = new brls::Box(brls::Axis::COLUMN);
    info->setGrow(1);
    info->setMarginLeft(20);
    footer->addView(info);

    now_title_ = new brls::Label();
    now_title_->setText("未在播放");
    now_title_->setFontSize(22);
    now_title_->setMarginBottom(10);
    info->addView(now_title_);

    auto* row_time = new brls::Box(brls::Axis::ROW);
    row_time->setAlignItems(brls::AlignItems::CENTER);

    progress_ = new brls::Slider();
    progress_->setGrow(1);
    progress_->setMarginRight(18);
    progress_->setProgress(0.0f);
    // 关键：Slider::setProgress() 内部同样会 fire 这个事件，
    // 所以必须区分"用户拖动"和"程序刷新"，否则 tick 每刷一次进度条就会 seek 一次。
    // 另外拖动时每帧都 seek 会把缓冲反复清空（每次都要重新定位解码），
    // 听起来就是连续卡顿 —— 这里只记目标，等手停 200ms 后由 tick 执行一次。
    progress_->getProgressEvent()->subscribe([this](float v) {
        if (this->suppress_seek_) return;   // 程序刷新触发的，忽略
        auto& ae         = hd::AudioEngine::get();
        const double dur = ae.duration();
        if (dur <= 0.5) return;
        this->seek_target_sec_ = (double)v * dur;
        this->seek_pending_    = true;
        this->seek_touch_at_   = std::chrono::steady_clock::now();
    });
    row_time->addView(progress_);

    time_label_ = new brls::Label();
    time_label_->setText("00:00 / 00:00");
    time_label_->setFontSize(18);
    time_label_->setWidth(180);
    row_time->addView(time_label_);
    info->addView(row_time);

    auto* row_btn = new brls::Box(brls::Axis::ROW);
    row_btn->setMarginTop(14);
    row_btn->setAlignItems(brls::AlignItems::CENTER);
    this->footer_row_ = row_btn;   // 同上：这一排里左右是移动焦点

    auto* btn_prev = new ui::IconButton(ui::glyph::Prev, ui::kIconMd, ui::kBtnMd);
    btn_prev->registerClickAction([this](brls::View*) {
        this->play_next(-1);
        return true;
    });
    row_btn->addView(btn_prev);

    // 主操作：实心白底 + 深色图标，比两侧大一号
    btn_play_ = new ui::IconButton(ui::glyph::Play, ui::kIconXl, ui::kBtnLg);
    btn_play_->setEmphasized(true);
    btn_play_->setMarginLeft(26);
    btn_play_->setMarginRight(26);
    btn_play_->registerClickAction([this](brls::View*) {
        hd::AudioEngine::get().toggle();
        return true;
    });
    row_btn->addView(btn_play_);

    auto* btn_next = new ui::IconButton(ui::glyph::Next, ui::kIconMd, ui::kBtnMd);
    btn_next->registerClickAction([this](brls::View*) {
        this->play_next(1);
        return true;
    });
    row_btn->addView(btn_next);

    info->addView(row_btn);
    root->addView(footer);

    // 手柄动作绑在 root 上（contentView）。必须在 return 之前、root 建好之后做。
    setup_gamepad(root);

    dlog("createContentView done");
    return root;
}

void PlayerActivity::onContentAvailable() {
    dlog("onContentAvailable");
    poll_ = new PollTask(this);
    poll_->start();

    apply_background();   // 放到首帧之后做：解码大图不会卡住黑屏阶段
    refresh_nav();
    refresh_status();

    // 首屏给一批内容，省得用户面对空列表
    if (engine_ok_ && search_results_.empty()) start_search("晴天");
}

// ─────────────────────────────────────────────────────────────
// 页面切换
// ─────────────────────────────────────────────────────────────
// ─────────────────────────────────────────────────────────────
// 手柄
// ─────────────────────────────────────────────────────────────
// 三条注意事项，都是踩过才知道的：
//
// 1) action 必须绑在 contentView(root) 上，不能绑在 Activity 上。
//    Activity::registerAction 内部是 `if (this->contentView) contentView->registerAction(...)`，
//    而 createContentView 执行期间 contentView 还没被赋值 —— 那时注册会被静默丢弃。
//
// 2) handleAction 是从 currentFocus 沿父链往上找的：列表行、按钮这些子视图先拿到
//    按键，它们没兴趣才轮到 root。所以这里注册 A 不会抢掉"列表行 A 播放"
//    和"播放键 A 暂停"。
//
// 3) 方向键默认会被 Application::navigate() 吃干净 —— onControllerButtonPressed
//    里 `case BUTTON_NAV_LEFT: navigate(...); return;` 根本不看导航成不成功、
//    有没有找到目标。所以左右切页只能自己注册 action，不能指望"导航失败后冒泡上来"。
void PlayerActivity::setup_gamepad(brls::View* root) {
    if (!root) return;
    // 一律 hidden：底部不出现按键提示条，界面保持干净（提示文字与功能无关）
    constexpr bool kHidden = true;

    // —— LB / RB：上一首 / 下一首 ——
    root->registerAction("上一首", brls::BUTTON_LB, [this](brls::View*) {
        this->play_next(-1);
        return true;
    }, kHidden);
    root->registerAction("下一首", brls::BUTTON_RB, [this](brls::View*) {
        this->play_next(1);
        return true;
    }, kHidden);

    // —— 十字左右：主页之间循环 ——
    // 焦点在顶栏 / 播放按钮那一排时交还给导航：那两排是横排，
    // 左右本来就有"选相邻按钮"的含义，抢走会让人没法用十字键选按钮。
    root->registerAction("上一页", brls::BUTTON_LEFT, [this](brls::View*) {
        if (this->focus_in(this->header_) || this->focus_in(this->footer_row_)) {
            this->focus_sibling_in(this->focus_in(this->header_) ? this->header_ : this->footer_row_, -1);
            return true;
        }
        this->nav_page(-1);
        return true;
    }, kHidden);
    root->registerAction("下一页", brls::BUTTON_RIGHT, [this](brls::View*) {
        if (this->focus_in(this->header_) || this->focus_in(this->footer_row_)) {
            this->focus_sibling_in(this->focus_in(this->header_) ? this->header_ : this->footer_row_, 1);
            return true;
        }
        this->nav_page(1);
        return true;
    }, kHidden);

    // —— 十字上/下：吃掉重复上报，导航仍由 NAV_UP/NAV_DOWN 完成 ——
    // FULL 映射下按一次十字上，libnx 会同时置位 BUTTON_UP 和 NAV_UP。
    // NAV_UP 负责导航，BUTTON_UP 没人认领 → borealis 会播一声"错误"音。
    // 这里挂个空动作把它吃掉，不影响导航。
    root->registerAction("", brls::BUTTON_UP, [](brls::View*) { return true; }, kHidden);
    root->registerAction("", brls::BUTTON_DOWN, [](brls::View*) { return true; }, kHidden);

    // —— ZL / ZR：快退 / 快进 ——
    // allowRepeating = true：按住 250ms 后 borealis 每 100ms 回调一次。
    // 回调里只累加目标位置，真正的 seek 由 tick 的 200ms 防抖统一落地。
    // 如果每次回调都 seek，解码器会一直"重新定位 + 重新下载"，音乐直接卡死。
    root->registerAction("快退", brls::BUTTON_LT, [this](brls::View*) {
        this->trigger_step(-1);
        return true;
    }, kHidden, true);
    root->registerAction("快进", brls::BUTTON_RT, [this](brls::View*) {
        this->trigger_step(1);
        return true;
    }, kHidden, true);

    // —— B：返回 ——
    // 歌单里的歌曲页回歌单列表；其他页面吞掉。
    // 吞掉而不是放行：borealis 对没人认领的按键会播"错误"音，
    // 而且这里也不注册退出动作，避免误按 B 直接把应用关掉。
    root->registerAction("返回", brls::BUTTON_B, [this](brls::View*) {
        if (this->page_ == Page::Songs) {
            this->switch_page(Page::Playlists);
        } else if (this->page_ != Page::Search) {
            // 其他非首页也回搜索页：不做的话按 B 完全没有反馈，像是卡住了
            this->switch_page(Page::Search);
        }
        return true;
    }, kHidden);

    // —— A：兜底播放/暂停 ——
    // 只在焦点落在没有 A 动作的地方（空白区、状态行）时生效。
    root->registerAction("播放 / 暂停", brls::BUTTON_A, [this](brls::View*) {
        hd::AudioEngine::get().toggle();
        return true;
    }, kHidden);

    // —— X / Y / 左右摇杆按下：占位 ——
    // 先吞掉，避免按下时报错误音；同时写日志，方便以后分配时确认键位。
    struct Reserved
    {
        brls::ControllerButton key;
        const char* name;
    };
    static const Reserved kReserved[] = {
        {brls::BUTTON_X, "X"},
        {brls::BUTTON_Y, "Y"},
        {brls::BUTTON_LSB, "LSB(左摇杆按下)"},
        {brls::BUTTON_RSB, "RSB(右摇杆按下)"},
    };
    for (const auto& r : kReserved) {
        const std::string n = r.name;
        root->registerAction(n + "（未分配）", r.key, [n](brls::View*) {
            dlog("reserved key: " + n);
            return true;
        }, kHidden);
    }

    dlog("gamepad ready");
}

bool PlayerActivity::focus_in(brls::View* box) const {
    if (!box) return false;
    for (brls::View* v = brls::Application::getCurrentFocus(); v; v = v->getParent())
        if (v == box) return true;
    return false;
}

bool PlayerActivity::focus_sibling_in(brls::Box* box, int dir) {
    if (!box || dir == 0) return false;

    brls::View* cur = brls::Application::getCurrentFocus();
    auto& kids      = box->getChildren();

    int idx = -1;
    for (size_t i = 0; i < kids.size(); ++i) {
        if (kids[i] == cur) {
            idx = (int)i;
            break;
        }
    }
    if (idx < 0) return false;

    // 跳过 GONE 的按钮：顶栏的返回 / 刷新平时是隐藏的，
    // 焦点落在看不见的控件上会让人以为按键失灵了。
    for (int i = idx + dir; i >= 0 && i < (int)kids.size(); i += dir) {
        brls::View* c = kids[i];
        if (!c || c->getVisibility() != brls::Visibility::VISIBLE) continue;
        if (!c->isFocusable()) continue;
        brls::Application::giveFocus(c);
        return true;
    }
    return false;   // 到头了：什么都不做，不要顺势切页
}

void PlayerActivity::nav_page(int delta) {
    // 歌单里的歌曲页是子页：左右键先退回歌单列表，和 B 键语义一致。
    // 直接跳到别的页面会让人失去"我在哪一层"的感觉。
    if (this->page_ == Page::Songs) {
        this->switch_page(Page::Playlists);
        return;
    }

    // 主循环不含 Songs（它是 Playlists 的子页）
    static const Page kRing[] = {Page::Search, Page::Playlists, Page::Queue, Page::Settings};
    constexpr int kN       = (int)(sizeof(kRing) / sizeof(kRing[0]));

    int idx = 0;
    for (int i = 0; i < kN; ++i) {
        if (kRing[i] == this->page_) {
            idx = i;
            break;
        }
    }
    idx               = (idx + delta + kN) % kN;
    const Page target = kRing[idx];

    // 进这两个页面时的附带动作和顶栏按钮保持一致：
    // 歌单页要同步（内部有 60 秒节流），设置页要重扫背景目录。
    if (target == Page::Settings) this->scan_backgrounds();
    this->switch_page(target);
    if (target == Page::Playlists) this->load_playlists();
}

void PlayerActivity::trigger_step(int dir) {
    auto& ae         = hd::AudioEngine::get();
    const double dur = ae.duration();
    if (dur <= 0.5) return;          // 时长都没解析出来，跳了也没有意义
    if (!ae.can_seek()) return;      // ADTS 裸流没有索引表，跳不了

    const auto now = std::chrono::steady_clock::now();

    // 第一次按 / 换方向：重新取基准。
    // 基准优先用"还没落地的目标位置" —— 否则连点两下会以旧位置为基准，
    // 表现为"点了两下只前进 5 秒"。
    if (this->trigger_dir_ != dir) {
        this->trigger_dir_      = dir;
        this->trigger_base_sec_ = this->seek_pending_ ? this->seek_target_sec_ : ae.position();
        this->trigger_hold_at_  = now;
    }

    const double held = std::chrono::duration<double>(now - this->trigger_hold_at_).count();
    // 按下立刻给 5 秒，之后按住每秒再累加 5 秒 —— 与"按住后每秒前进 5 秒"等价。
    double target = this->trigger_base_sec_ + (double)dir * (5.0 + held * 5.0);

    if (target < 0.0) target = 0.0;
    // 留 1 秒余量，不能只留 "一点点"：
    // tick 里判定"播完"的条件是 pos >= dur - 0.5，这里如果把目标夹到 dur-0.3，
    // 快进到结尾时会在落地那一帧被误判成播放结束，直接跳到下一首。
    if (target > dur - 1.0) target = dur - 1.0;
    if (target < 0.0) target = 0.0;

    this->seek_target_sec_ = target;
    this->seek_pending_    = true;
    this->seek_touch_at_   = now;   // 每次回调都往后推，按住期间不落地
}

void PlayerActivity::switch_page(Page p) {
    dlog("switch_page -> " + std::to_string((int)p));
    if (p != page_) list_offset_ = 0;   // 换页从头看，同页内保留翻页位置
    page_ = p;
    status_text_.clear();   // 切页时清掉上一页的临时提示
    refresh_nav();
    dlog("  nav refreshed");
    refresh_status();

    // 重建必须推到下一帧，不能在这里同步做。
    // 触摸点击时框架还在 gestureRecognizerRequest 的递归里（它先从命中的
    // view 一路往 parent 调），此时把整棵内容树 clearViews(true) 掉，
    // 框架返回途中一旦碰到已释放的视图就是直接重启 —— 所以一律延后。
    post([this]() { this->rebuild_content(); });
}

void PlayerActivity::refresh_nav() {
    if (nav_search_) nav_search_->setEmphasized(page_ == Page::Search);
    if (nav_library_)
        nav_library_->setEmphasized(page_ == Page::Playlists || page_ == Page::Songs);
    if (nav_queue_) nav_queue_->setEmphasized(page_ == Page::Queue);
    if (nav_settings_) nav_settings_->setEmphasized(page_ == Page::Settings);
    if (nav_back_)
        nav_back_->setVisibility(page_ == Page::Songs ? brls::Visibility::VISIBLE
                                                      : brls::Visibility::GONE);
    // 刷新只在"账号内容页 / 背景目录"有意义：搜索结果是即时查的，队列是本地内存
    if (nav_refresh_)
        nav_refresh_->setVisibility((page_ == Page::Playlists || page_ == Page::Songs ||
                                     page_ == Page::Settings)
                                        ? brls::Visibility::VISIBLE
                                        : brls::Visibility::GONE);
}

void PlayerActivity::refresh_status() {
    if (!status_) return;
    if (!engine_ok_ && !last_error_.empty()) {
        status_->setText(last_error_);
        return;
    }
    if (!status_text_.empty()) {
        status_->setText(status_text_);
        return;
    }
    switch (page_) {
        case Page::Search:
            status_->setText(search_results_.empty()
                                 ? "按搜索图标找歌"
                                 : "搜索结果 · " + std::to_string(search_results_.size()) + " 首");
            break;
        case Page::Playlists: {
            std::string t = playlists_.empty()
                                ? std::string("我的歌单")
                                : "我的歌单 · " + std::to_string(playlists_.size()) + " 个";
            if (!sync_clock_.empty()) t += " · " + sync_clock_ + " 同步";
            status_->setText(t);
            break;
        }
        case Page::Songs:
            status_->setText(playlist_name_ + " · " + std::to_string(playlist_songs_.size()) + " 首");
            break;
        case Page::Queue:
            status_->setText(queue_.empty()
                                 ? "播放队列（空）"
                                 : "播放队列 · " + std::to_string(queue_.size()) + " 首");
            break;
        case Page::Settings:
            status_->setText(bg_name_.empty()
                                 ? "设置 · 背景：纯色"
                                 : "设置 · 背景：" + bg_name_);
            break;
    }
}

void PlayerActivity::rebuild_content() {
    if (!content_box_) return;
    dlog("rebuild begin page=" + std::to_string((int)page_) +
         " queue=" + std::to_string(queue_.size()) +
         " offset=" + std::to_string(list_offset_));

    // 必须先转移焦点再清空：borealis 的 clearViews(true) 会直接 freeView 子视图，
    // 完全不检查 Application::currentFocus。只要焦点正停在某一行上，
    // 清完那个指针就是野的，下一次 giveFocus / 按方向键就会踩它（崩溃或乱跳）。
    // 播放键在 footer 里、不属于列表，用它当临时落脚点最稳。
    if (btn_play_) brls::Application::giveFocus(btn_play_);
    dlog("  focus parked");

    content_box_->clearViews(true);
    dlog("  cleared children");
    pending_focus_ = nullptr;

    switch (page_) {
        case Page::Search:
            list_source_ = &search_results_;
            build_track_rows(search_results_, "还没有搜索结果。点右上角放大镜搜一首。", false);
            break;
        case Page::Playlists:
            build_playlist_rows();
            break;
        case Page::Songs:
            list_source_ = &playlist_songs_;
            build_track_rows(playlist_songs_, "这个歌单是空的。", true);
            break;
        case Page::Queue:
            build_queue_rows();
            break;
        case Page::Settings:
            build_settings_rows();
            break;
    }
    dlog("  rows built");

    // 清空重建后焦点会掉，手动交给第一行，否则手柄要按好几次方向键才回得来
    if (pending_focus_) brls::Application::giveFocus(pending_focus_);
    pending_focus_ = nullptr;
    dlog("  focus restored");

    refresh_status();
    dlog("rebuild done");
}

void PlayerActivity::build_track_rows(const std::vector<se::Track>& items,
                                      const std::string& empty_hint, bool show_play_all) {
    // 「播放全部」只在歌单页出现，放在列表最上面
    if (show_play_all && !items.empty()) {
        auto* all = make_row_button("播放全部 · " + std::to_string(items.size()) + " 首", true);
        all->setTextColor(kNowPlayingColor);
        all->registerClickAction([this](brls::View*) {
            this->play_from(*this->list_source_, 0);
            return true;
        });
        content_box_->addView(all);
        pending_focus_ = all;
    }

    if (items.empty()) {
        content_box_->addView(make_empty_label(empty_hint));
        return;
    }

    // 当前播放的是哪一首，靠 track id 比出来（队列可能来自另一个列表）
    const se::Track* cur =
        (queue_pos_ >= 0 && queue_pos_ < (int)queue_.size()) ? &queue_[queue_pos_] : nullptr;

    // 越界保护：列表变短（比如删了队列项）以后 offset 可能落到末尾外面
    if (list_offset_ >= (int)items.size()) list_offset_ = 0;
    const int begin = list_offset_;
    const int end   = std::min((int)items.size(), begin + kPageSize);

    add_pager((int)items.size(), begin, end);

    for (int i = begin; i < end; ++i) {
        const auto& t      = items[i];
        const bool playing = cur && !cur->id.empty() && cur->id == t.id;

        std::string text = t.title + "   ·   " + join_artist(t.artist);
        std::string dur  = fmt_dur(t.duration_ms);
        if (!dur.empty()) text += "   ·   " + dur;

        auto* row = make_row_button(text, false);
        if (playing) row->setTextColor(kNowPlayingColor);
        if (playing) pending_focus_ = row;
        else if (!pending_focus_) pending_focus_ = row;

        const int index      = i;   // 绝对下标：播放全部 / 队列都从整个列表算
        const bool is_cur    = playing;
        row->registerClickAction([this, index, is_cur](brls::View*) {
            // 选中的就是正在播的那首：A 键切成播放/暂停。
            // 否则"选中当前歌曲按 A"会重新取流加载一遍，表现为卡一下再从头开始。
            if (is_cur) {
                hd::AudioEngine::get().toggle();
                return true;
            }
            // 点哪首，整个当前列表就成为播放队列 —— 这样"下一首"才有意义
            this->play_from(*this->list_source_, index);
            return true;
        });
        content_box_->addView(row);
    }
}

// 分页条：只在超过一页时出现，放在列表上方。
// 左箭头 / 页码 / 右箭头，两端留等宽占位，保证页码始终居中。
void PlayerActivity::add_pager(int total, int begin, int end) {
    if (total <= kPageSize) return;

    auto* pager = new brls::Box(brls::Axis::ROW);
    pager->setAlignItems(brls::AlignItems::CENTER);
    pager->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    pager->setMarginBottom(10);

    if (begin > 0) {
        auto* prev = new ui::IconButton(ui::glyph::ChevronLeft, ui::kIconSm, ui::kBtnSm);
        prev->registerClickAction([this](brls::View*) {
            this->shift_page(-1);
            return true;
        });
        pager->addView(prev);
    } else {
        auto* pad = new brls::Box();
        pad->setWidth(ui::kBtnSm);
        pad->setHeight(ui::kBtnSm);
        pager->addView(pad);
    }

    auto* info = new brls::Label();
    info->setText(std::to_string(begin + 1) + " - " + std::to_string(end) + " / " +
                  std::to_string(total));
    info->setFontSize(16);
    info->setTextColor(kMutedColor);
    pager->addView(info);

    if (end < total) {
        auto* next = new ui::IconButton(ui::glyph::ChevronRight, ui::kIconSm, ui::kBtnSm);
        next->registerClickAction([this](brls::View*) {
            this->shift_page(1);
            return true;
        });
        pager->addView(next);
    } else {
        auto* pad = new brls::Box();
        pad->setWidth(ui::kBtnSm);
        pad->setHeight(ui::kBtnSm);
        pager->addView(pad);
    }

    content_box_->addView(pager);
}

void PlayerActivity::post(std::function<void()> fn) {
    auto token = alive_;
    // delay 的回调在主线程的下一帧执行（不是另开线程），
    // 正好卡在本次触摸事件处理完、框架不再碰被点 view 之后。
    brls::delay(1, [token, fn]() {
        if (token->load()) fn();
    });
}

void PlayerActivity::shift_page(int delta) {
    const std::vector<se::Track>* src = nullptr;
    switch (page_) {
        case Page::Search: src = &search_results_; break;
        case Page::Songs: src = &playlist_songs_; break;
        case Page::Queue: src = &queue_; break;
        default: return;
    }

    const int total = (int)src->size();
    int off         = list_offset_ + delta * kPageSize;
    if (off < 0) off = 0;
    if (off >= total || off == list_offset_) return;

    list_offset_ = off;
    post([this]() { this->rebuild_content(); });   // 翻页按钮本身就在被重建的列表里
}

void PlayerActivity::build_playlist_rows() {
    if (playlists_.empty()) {
        std::string hint;
        if (busy_)
            hint = "正在同步歌单…";
        else if (!account_ok_)
            hint = "没有读到登录信息：把 account.json 放到 sdmc:/switch/se_sources/ 后再刷新";   // 没有 Cookie 时接口只认游客
        else
            hint = "这个账号下没有歌单。点右上角刷新重新同步。";
        content_box_->addView(make_empty_label(hint));
        return;
    }

    for (size_t i = 0; i < playlists_.size(); ++i) {
        const auto& p = playlists_[i];
        std::string text = p.name;
        if (p.song_count > 0) text += "   ·   " + std::to_string(p.song_count) + " 首";

        auto* row  = make_row_button(text, true);
        int index  = (int)i;
        row->registerClickAction([this, index](brls::View*) {
            // 点歌单会切页并重建列表，把这一行自己删掉 → 延后一帧
            this->post([this, index]() { this->open_playlist(index); });
            return true;
        });
        content_box_->addView(row);
        if (!pending_focus_) pending_focus_ = row;
    }
}

void PlayerActivity::build_queue_rows() {
    dlog("  build_queue_rows n=" + std::to_string(queue_.size()) +
         " pos=" + std::to_string(queue_pos_));
    if (queue_.empty()) {
        content_box_->addView(make_empty_label("队列是空的。去搜索或打开一个歌单点歌。"));
        return;
    }

    if (list_offset_ >= (int)queue_.size()) list_offset_ = 0;
    const int begin = list_offset_;
    const int end   = std::min((int)queue_.size(), begin + kPageSize);

    add_pager((int)queue_.size(), begin, end);

    for (int i = begin; i < end; ++i) {
        const auto& t      = queue_[i];
        const bool playing = (i == queue_pos_);

        // 一行 = 左边可点的歌名 + 右边独立的删除按钮。
        // 不做成"整行 + 内部图标"，因为 Label 会拦截触摸，点到文字上就没反应了。
        auto* line = new brls::Box(brls::Axis::ROW);
        line->setAlignItems(brls::AlignItems::CENTER);
        line->setMarginBottom(6);

        std::string text = std::to_string(i + 1) + ".  " + t.title + "   ·   " +
                           join_artist(t.artist);
        if (playing) text += "   ·   正在播放";

        auto* row = make_row_button(text, false);
        row->setGrow(1);
        row->setMarginBottom(0);
        if (playing) row->setTextColor(kNowPlayingColor);

        const int index   = i;
        const bool is_cur = playing;
        row->registerClickAction([this, index, is_cur](brls::View*) {
            // 队列里点的就是正在播的那首 → A 键播放/暂停，不要重新加载
            if (is_cur) {
                hd::AudioEngine::get().toggle();
                return true;
            }
            this->play_queue_index(index);
            return true;
        });
        line->addView(row);

        auto* del = new ui::IconButton(ui::glyph::Close, ui::kIconSm, ui::kBtnSm);
        del->setMarginLeft(10);
        del->registerClickAction([this, index](brls::View*) {
            // 这个叉本身就在队列里，queue_remove 会重建列表顺手把它删掉。
            // 在它自己的点击回调里同步删 = 删自己 → 必须延后一帧。
            this->post([this, index]() { this->queue_remove(index); });
            return true;
        });
        line->addView(del);

        content_box_->addView(line);
        // 切歌后队列会重建，焦点优先落回正在播的那一行，
        // 否则每换一首焦点就跳回列表顶部，手柄操作会很难受。
        if (playing) pending_focus_ = row;
        else if (!pending_focus_) pending_focus_ = row;
    }
    dlog("  queue rows ok");
}

// ─────────────────────────────────────────────────────────────
// 设置 / 外观
// ─────────────────────────────────────────────────────────────
void PlayerActivity::load_ui_pref() {
    std::ifstream f(kUiPrefPath, std::ios::binary);
    if (!f) return;
    try {
        se::json j;
        f >> j;
        if (j.is_object()) bg_name_ = j.value("background", std::string(""));
    } catch (...) {
        // 配置坏了就当没设置过：一个显示偏好不值得让播放器起不来
    }
}

void PlayerActivity::save_ui_pref() {
    se::json j     = se::json::object();
    j["background"] = bg_name_;
    std::ofstream f(kUiPrefPath, std::ios::binary | std::ios::trunc);
    if (!f) return;
    f << j.dump(2);
}

void PlayerActivity::scan_backgrounds() {
    bg_files_.clear();
    mkdir(kBgDir, 0777);   // 已经有了就返回 -1，不用管

    DIR* d = opendir(kBgDir);
    if (!d) return;
    while (struct dirent* e = readdir(d)) {
        const std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        if (!is_image_name(name)) continue;
        bg_files_.push_back(name);
    }
    closedir(d);
    std::sort(bg_files_.begin(), bg_files_.end());
}

void PlayerActivity::apply_background() {
    if (!bg_image_ || !bg_scrim_) return;

    if (bg_name_.empty()) {
        bg_image_->setVisibility(brls::Visibility::GONE);
        bg_scrim_->setVisibility(brls::Visibility::GONE);
        return;
    }

    const std::string path = std::string(kBgDir) + "/" + bg_name_;
    if (!file_readable(path)) {
        // 图被删了 / 改名了：退回纯色，别留一块解码失败的空白
        bg_name_.clear();
        save_ui_pref();
        bg_image_->setVisibility(brls::Visibility::GONE);
        bg_scrim_->setVisibility(brls::Visibility::GONE);
        return;
    }

    bg_image_->setImageFromFile(path);
    bg_image_->setVisibility(brls::Visibility::VISIBLE);
    bg_scrim_->setVisibility(brls::Visibility::VISIBLE);
}

void PlayerActivity::set_background(const std::string& name) {
    bg_name_ = name;
    save_ui_pref();
    apply_background();
    set_status(name.empty() ? "背景已关闭" : "背景已切换：" + name);
    rebuild_content();   // 重画列表，让"当前"标记跟过去
}

void PlayerActivity::build_settings_rows() {
    auto* sec = new brls::Label();
    sec->setText("背景图片");
    sec->setFontSize(18);
    sec->setTextColor(kMutedColor);
    sec->setMarginTop(2);
    sec->setMarginBottom(10);
    content_box_->addView(sec);

    auto* plain = make_row_button(bg_name_.empty() ? "纯色背景   ·   当前" : "纯色背景", false);
    if (bg_name_.empty()) plain->setTextColor(kNowPlayingColor);
    plain->registerClickAction([this](brls::View*) {
        // 选背景会重建设置页，把这一行删掉 → 延后一帧
        this->post([this]() { this->set_background(""); });
        return true;
    });
    content_box_->addView(plain);
    pending_focus_ = plain;

    if (bg_files_.empty()) {
        content_box_->addView(make_empty_label(
            "没有找到图片。把 .jpg / .png 放进 sdmc:/switch/se_sources/background/ 后点右上角刷新。"));
        return;
    }

    for (const auto& name : bg_files_) {
        const bool cur = (name == bg_name_);
        auto* row      = make_row_button(cur ? name + "   ·   当前" : name, false);
        if (cur) row->setTextColor(kNowPlayingColor);
        row->registerClickAction([this, name](brls::View*) {
            this->post([this, name]() { this->set_background(name); });
            return true;
        });
        content_box_->addView(row);
        if (cur) pending_focus_ = row;
    }
}

// ─────────────────────────────────────────────────────────────
// 搜索
// ─────────────────────────────────────────────────────────────
void PlayerActivity::open_search_dialog() {
    if (!engine_ok_) {
        set_status("音源未就绪，无法搜索");
        return;
    }
    auto token = alive_;
    brls::Application::getImeManager()->openForText(
        [this, token](std::string text) {
            if (!token->load()) return;
            if (text.empty()) return;
            this->start_search(text);
        },
        "搜索歌曲", "输入歌名或歌手，按 + 确认", 64, keyword_);
}

void PlayerActivity::start_search(const std::string& keyword) {
    keyword_ = keyword;
    busy_    = true;
    if (page_ != Page::Search) switch_page(Page::Search);
    set_status("搜索中: " + keyword);

    auto token = alive_;
    const int seq = ++req_seq_;
    brls::async([this, token, keyword, seq]() {
        std::vector<se::Track> found;
        std::string err;
        try {
            std::lock_guard<std::mutex> lk(op_mu_);
            found = engine_->search(keyword, 1);
        } catch (const std::exception& e) {
            err = e.what();
        }

        brls::sync([this, token, found, err, seq]() {
            if (!token->load()) return;
            if (seq != req_seq_) return;   // 期间又发起了新请求，这次结果作废
            busy_ = false;
            if (!err.empty()) {
                set_status(std::string("搜索失败: ") + err);
                return;
            }
            search_results_ = found;
            list_offset_    = 0;   // 新的一批结果，回到第一页
            status_text_.clear();
            rebuild_content();
        });
    });
}

// ─────────────────────────────────────────────────────────────
// 歌单
// ─────────────────────────────────────────────────────────────
void PlayerActivity::load_playlists(bool force) {
    if (!engine_ok_) {
        set_status("音源未就绪");
        return;
    }
    if (busy_) return;

    // 节流：列表已经有内容、而且刚同步过，就不重复打接口。
    // 手动点刷新（force）不受这个限制。
    if (!force && !playlists_.empty()) {
        const auto age = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::steady_clock::now() - last_sync_)
                             .count();
        if (age < kSyncCooldownSec) return;
    }

    busy_ = true;
    set_status(playlists_.empty() ? "正在同步歌单…" : "正在刷新歌单…");
    // 空列表时显示"正在同步"；已有内容则保留旧列表，不闪空。
    // 这条链路是从顶栏图标的点击回调里进来的，重建只能延后一帧 —— 见 switch_page 的注释。
    post([this]() { this->rebuild_content(); });

    auto token = alive_;
    const int seq = ++req_seq_;
    brls::async([this, token, seq]() {
        std::vector<se::Playlist> got;
        std::string err;
        try {
            std::lock_guard<std::mutex> lk(op_mu_);
            got = engine_->playlists();
        } catch (const std::exception& e) {
            err = e.what();
        }

        brls::sync([this, token, got, err, seq]() {
            if (!token->load()) return;
            if (seq != req_seq_) return;
            busy_ = false;
            if (!err.empty()) {
                set_status("歌单同步失败: " + friendly_error(err));
                rebuild_content();
                return;
            }
            playlists_  = got;
            last_sync_  = std::chrono::steady_clock::now();
            sync_clock_ = clock_now();
            status_text_.clear();
            rebuild_content();
        });
    });
}

// 刷新按钮：按当前页决定刷新什么。不去动搜索页/队列页（那里没有可同步的远端数据）。
void PlayerActivity::refresh_current_page() {
    if (!engine_ok_) {
        set_status("音源未就绪");
        return;
    }
    if (busy_) {
        set_status("正在同步，请稍候…");
        return;
    }

    if (page_ == Page::Settings) {
        // 用户刚往目录里拷了图：重新扫一遍，顺便校验选中的那张还在不在
        scan_backgrounds();
        apply_background();
        set_status("已重新扫描：" + std::to_string(bg_files_.size()) + " 张图片");
        post([this]() { this->rebuild_content(); });   // 同样在点击回调里，延后一帧
        return;
    }

    if (page_ == Page::Songs) {
        // 用歌单 id 找回下标，走和点击进入完全相同的那条路径
        for (size_t i = 0; i < playlists_.size(); ++i) {
            if (playlists_[i].id == playlist_id_) {
                open_playlist((int)i);
                return;
            }
        }
        // 列表被同步换掉、找不到这个歌单了：退回列表重新同步
        switch_page(Page::Playlists);
        load_playlists(true);
        return;
    }

    load_playlists(true);
}

void PlayerActivity::open_playlist(int index) {
    if (index < 0 || index >= (int)playlists_.size()) return;

    // 这里不拦 busy_：用户连点两个歌单时，应该以后点的那个为准，
    // 旧请求靠 req_seq_ 作废，而不是让第二次点击毫无反应。
    const se::Playlist p = playlists_[index];   // 拷一份，异步期间列表可能被换掉
    busy_                = true;
    page_                = Page::Songs;
    playlist_name_       = p.name;
    playlist_id_         = p.id;
    playlist_songs_.clear();
    status_text_ = "打开歌单: " + p.name;
    refresh_nav();
    rebuild_content();

    auto token = alive_;
    const int seq = ++req_seq_;
    brls::async([this, token, p, seq]() {
        std::vector<se::Track> songs;
        std::string err;
        try {
            std::lock_guard<std::mutex> lk(op_mu_);
            songs = engine_->playlist_songs(p.id);
        } catch (const std::exception& e) {
            err = e.what();
        }

        brls::sync([this, token, songs, err, p, seq]() {
            if (!token->load()) return;
            if (seq != req_seq_) return;
            busy_ = false;
            if (!err.empty()) {
                set_status("歌单打开失败: " + friendly_error(err));
                rebuild_content();
                return;
            }
            playlist_songs_ = songs;
            list_offset_    = 0;
            playlist_name_  = p.name;
            status_text_.clear();
            rebuild_content();
        });
    });
}

// ─────────────────────────────────────────────────────────────
// 播放
// ─────────────────────────────────────────────────────────────
// 下载封面并在主线程贴上去。
// 封面走单独的请求，不占音频连接，也不阻塞出声。
void PlayerActivity::load_cover(const se::Track& t, std::shared_ptr<std::atomic<bool>> token) {
    if (!cover_) return;

    // 换歌先清掉上一张，避免网络慢的时候还挂着旧封面
    if (t.cover == cover_url_) return;   // 同一张就别重复下载
    cover_url_ = t.cover;
    cover_bytes_.clear();
    cover_->clear();
    if (cover_placeholder_) cover_placeholder_->setVisibility(brls::Visibility::VISIBLE);

    if (t.cover.empty()) return;

    const std::string url = t.cover;
    brls::async([this, token, url]() {
        std::string data;
        try {
            se::HttpRequest req;
            req.url         = url;
            req.timeout_sec = 10;
            req.max_body    = 512 * 1024;   // 300×300 的 JPEG 一般 20~60KB
            req.cainfo      = "romfs:/cacert.pem";
            auto r          = se::http_perform(req);
            if (r.ok() && r.status == 200 && !r.body.empty()) data = std::move(r.body);
        } catch (const std::exception& e) {
            // 封面失败不影响播放，静默跳过
        }
        if (data.empty()) return;

        brls::sync([this, token, url, data]() {
            if (!token->load()) return;
            if (!cover_) return;
            if (url != cover_url_) return;   // 期间又换歌了，这张作废
            cover_bytes_.assign(data.begin(), data.end());
            cover_->setImageFromMem(cover_bytes_.data(), (int)cover_bytes_.size());
            if (cover_placeholder_) cover_placeholder_->setVisibility(brls::Visibility::GONE);
        });
    });
}

// 把 src 整体作为播放队列，从 index 开始播
void PlayerActivity::play_from(const std::vector<se::Track>& src, int index) {
    if (index < 0 || index >= (int)src.size()) return;
    queue_     = src;
    queue_pos_ = index;
    play_current();
}

void PlayerActivity::play_queue_index(int index) {
    if (index < 0 || index >= (int)queue_.size()) return;
    queue_pos_ = index;
    play_current();
}

void PlayerActivity::play_current() {
    if (queue_pos_ < 0 || queue_pos_ >= (int)queue_.size()) return;
    if (!engine_ok_) return;

    ended_handled_ = false;
    const se::Track t = queue_[queue_pos_];   // 拷一份，避免异步期间队列被改
    // 从"点歌"到"出声"中间有取流 + 预缓冲两段等待。m4a 还要等整首下完
    // （moov 在文件尾），所以这里统一显示"缓冲中"，别让用户以为卡死。
    set_status("缓冲中: " + t.title);

    auto token = alive_;
    load_cover(t, token);
    brls::async([this, token, t]() {
        std::string err;
        std::string used_q;   // 实际下下来的档位（resolve 内部可能回退，以它返回的 quality 为准）

        // 音质顺序：标准 MP3 → 标准 AAC → 高品质 → 无损。
        // ① 默认标准档：320k 对大量曲目没有权限，服务端会返回一个语法完全正确、
        //    文件却不存在的地址，下载就是 404 —— 之前默认 320 的报错绝大部分来自这里。
        // ② AAC 排在 320 前面：QQ 有一部分曲子只给 m4a，不放这一档就永远 404。
        // ③ 每一档都必须真下载成功才算数，不能"取到地址就当成功"。
        static const char* kQualities[] = {"128", "aac", "320", "flac"};
        std::vector<std::string> tried_urls;

        for (const char* q : kQualities) {
            std::string url, got_q;
            try {
                std::lock_guard<std::mutex> lk(op_mu_);
                auto r = engine_->resolve(t, q);
                if (!r.ok) {
                    err = r.error.empty() ? "无法取流" : r.error;
                    continue;
                }
                url   = r.url;
                got_q = r.quality.empty() ? std::string(q) : r.quality;
            } catch (const std::exception& e) {
                err = e.what();
                continue;
            }

            // 同一首在不同档位可能给出同一个地址，别拿同一个坏地址反复下
            bool dup = false;
            for (const auto& u : tried_urls)
                if (u == url) dup = true;
            if (dup) continue;
            tried_urls.push_back(url);

            // 下载 + 打开解码器（也在这里做，别在主线程）
            std::string load_err;
            if (hd::AudioEngine::get().load(url, &load_err)) {
                err.clear();
                used_q = got_q;
                // 记下实际解出来的格式：上机看日志就能确认 AAC 这条路有没有真的走通
                auto inf = hd::AudioEngine::get().info();
                dlog("play ok @" + got_q + " " + inf.codec + " " + std::to_string(inf.sample_rate) +
                     "Hz/" + std::to_string(inf.channels) + "ch " +
                     std::to_string((int)inf.duration_sec) + "s " + t.title);
                break;
            }
            err = load_err;
            dlog("play fail @" + got_q + ": " + err);
        }

        if (!err.empty() && err.find("HTTP 404") != std::string::npos)
            err = "标准音质也取不到文件（HTTP 404）：这首歌在 QQ 音乐上没有版权或需要单独购买";

        brls::sync([this, token, err, t, used_q]() {
            if (!token->load()) return;
            if (!err.empty()) {
                set_status("播放失败: " + err);
                return;
            }
            if (now_title_) now_title_->setText(t.title + "   ·   " + join_artist(t.artist));
            hd::AudioEngine::get().play();
            auto info = hd::AudioEngine::get().info();
            set_status(quality_label(used_q) + " · " + info.codec + "  " +
                       std::to_string(info.sample_rate) + "Hz  " +
                       std::to_string(info.bytes / 1024 / 1024) + "MB");
            // 正在播放的标记跟着走：所有带歌曲行的页面都要刷新一次
            //（歌单页只有歌单名，没有"正在播放"，不用重建）
            if (page_ != Page::Playlists) rebuild_content();
        });
    });
}

void PlayerActivity::play_next(int delta) {
    if (queue_.empty()) return;
    const int n = (int)queue_.size();
    int next    = (queue_pos_ < 0) ? 0 : ((queue_pos_ + delta) % n + n) % n;
    queue_pos_  = next;
    play_current();
}

void PlayerActivity::queue_remove(int index) {
    if (index < 0 || index >= (int)queue_.size()) return;

    const bool was_current = (index == queue_pos_);
    queue_.erase(queue_.begin() + index);

    if (queue_.empty()) {
        queue_pos_ = -1;
        auto& ae   = hd::AudioEngine::get();
        ae.stop();
        ae.unload();
        if (now_title_) now_title_->setText("未在播放");
        if (cover_) cover_->clear();
        if (cover_placeholder_) cover_placeholder_->setVisibility(brls::Visibility::VISIBLE);
        cover_url_.clear();
        rebuild_content();
        return;
    }

    if (was_current) {
        // 删掉的是正在播的那首：直接播队列里接上来的那一首（删到最后就回到开头）
        if (queue_pos_ >= (int)queue_.size()) queue_pos_ = 0;
        rebuild_content();
        play_current();
        return;
    }
    if (index < queue_pos_) queue_pos_--;
    rebuild_content();
}

// ─────────────────────────────────────────────────────────────
// 定时刷新
// ─────────────────────────────────────────────────────────────
void PlayerActivity::tick() {
    // 心跳：每 10 秒一条。崩溃后看最后一条是 tick 还是 rebuild，
    // 就能分清"死在绘制/布局里"还是"死在切页链路里"。
    static int tick_n = 0;
    if (++tick_n % 40 == 0) {
        auto& aec = hd::AudioEngine::get();
        dlog("tick page=" + std::to_string((int)page_) + " q=" + std::to_string(queue_.size()) +
             " pos=" + std::to_string(queue_pos_) + " state=" + std::to_string((int)aec.state()));
    }

    auto& ae = hd::AudioEngine::get();

    switch (ae.state()) {
        case hd::PlayState::Loading:
            if (btn_play_) btn_play_->setGlyph(ui::glyph::Loading);
            break;
        case hd::PlayState::Playing:
            if (btn_play_) btn_play_->setGlyph(ui::glyph::Pause);
            break;
        case hd::PlayState::Paused:
            if (btn_play_) btn_play_->setGlyph(ui::glyph::Play);
            break;
        default:
            if (btn_play_) btn_play_->setGlyph(ui::glyph::Play);
            break;
    }

    double dur = ae.duration();
    double pos = ae.position();

    // 扳机"松开"没有事件（action 只在按下和重复时回调），用超时反推：
    // 超过 400ms 没有新的重复回调就当已经松手，下次按同方向重新取基准。
    if (trigger_dir_ != 0 &&
        std::chrono::steady_clock::now() - seek_touch_at_ > std::chrono::milliseconds(400))
        trigger_dir_ = 0;

    // 手停 200ms 才真正跳。拖动 / 按住扳机期间都不跳 ——
    // 每次跳转都要清空缓冲重新定位，连着跳就会变成"卡成一片"。
    if (seek_pending_ && std::chrono::steady_clock::now() - seek_touch_at_ >
                             std::chrono::milliseconds(200)) {
        seek_pending_ = false;
        ae.seek(seek_target_sec_);
        // 不要读回 ae.position()：seek 是异步落地的，读回来还是旧位置，
        // 进度条会先往回弹一下再走过去。直接用目标值，下一帧自然被真值覆盖。
        pos = seek_target_sec_;
    }

    // 待落地期间显示目标位置：拖进度条 / 按住扳机时，时间和滑块要跟着手走，
    // 否则用户完全不知道自己跳到哪了。
    const double shown = seek_pending_ ? seek_target_sec_ : pos;
    if (time_label_) time_label_->setText(fmt_time(shown) + " / " + fmt_time(dur));
    if (progress_ && dur > 0.5) {
        suppress_seek_ = true;
        progress_->setProgress((float)(shown / dur));
        suppress_seek_ = false;
    }

    // 放完了自动下一首；ended_handled_ 防止每 250ms 重复触发
    if (!ended_handled_ && ae.state() == hd::PlayState::Playing && dur > 1.0 &&
        pos >= dur - 0.5 && !queue_.empty()) {
        ended_handled_ = true;
        // 这首放完立刻把下载缓冲还给系统（数据本来就不落盘，没有任何文件要删）。
        // 下一首 load() 会重新向服务器请求，绝不复用上一首的数据。
        ae.unload();
        play_next(1);
    }
}

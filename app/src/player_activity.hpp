// player_activity.hpp —— 播放器主界面
#pragma once

#include <borealis.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "player/audio_engine.hpp"
#include "se/source_engine.hpp"
#include "ui/icons.hpp"

class PollTask;

class PlayerActivity : public brls::Activity
{
  public:
    PlayerActivity();
    ~PlayerActivity() override;

    brls::View* createContentView() override;
    void onContentAvailable() override;

    // 由 PollTask 每 250ms 调一次（主线程）
    void tick();

  private:
    // ── 页面 ──
    // Home     : 首页（正在播放 + 四个入口）
    // Search   : 搜索结果
    // Playlists: 账号歌单列表
    // Songs    : 某个歌单里的歌曲
    // Queue    : 当前播放队列
    enum class Page
    {
        Home,
        Search,
        Playlists,
        Songs,
        Queue,
        Settings
    };

    void switch_page(Page p);
    void rebuild_content();
    void build_home_rows();
    void build_track_rows(const std::vector<se::Track>& items, const std::string& empty_hint,
                          bool show_play_all);
    void build_playlist_rows();
    void build_queue_rows();
    void build_settings_rows();
    void refresh_nav();
    void refresh_status();

    // ── 沉浸播放页 ──
    // 它不是 Page 枚举里的一项，而是一层**覆盖在整个界面之上的绝对定位浮层**。
    // 这么做的原因是崩溃：如果做成 Page，进出都要 clearViews(true) 重建内容树，
    // 而入口是"点底部封面"——触摸回调里拆树正是 v0.8 那次重启的原因。
    // 浮层只在启动时建一次，进出只切 visibility，全程不删任何视图。
    void build_now_overlay(brls::Box* root);
    void open_now();
    void close_now();
    void refresh_now_ui();        // 标题 / 歌手 / 音质（换歌时同步）
    void apply_cover_to_now();    // 把已下载的封面字节喂给大图与整屏底图
    // 列表分页：一次只建一页的视图（见 cpp 里 kPageSize 的说明）
    void add_pager(int total, int begin, int end);
    void shift_page(int delta);

    // ⚠️ 触摸回调里不能同步重建列表。
    // borealis 处理一次触摸时，是"先找到 firstResponder(被点的那个 view) →
    // 调它的手势识别器 → 识别器执行我们注册的 action"。
    // action 里如果 clearViews(true) 把那个 view 删了，事件处理返回后
    // 框架还要继续访问它（interruptGestures / 手势循环），于是 use-after-free，
    // 表现就是按一下直接崩溃重启。所以凡是"点了之后要重建列表"的，统一 post 到下一帧。
    void post(std::function<void()> fn);

    // ── 外观 / 自定义背景 ──
    void load_ui_pref();
    void save_ui_pref();
    void scan_backgrounds();                 // 扫 sdmc:/switch/se_sources/background/
    void apply_background();                 // 把 bg_name_ 落到视图上
    void set_background(const std::string& name);   // 选中 + 存盘 + 立即生效

    // ── 操作 ──
    void open_search_dialog();
    // jump = false 时只预取结果、不切到搜索页（首页要留在首页）
    void start_search(const std::string& keyword, bool jump = true);
    // force = true 时忽略冷却时间（用户手动点刷新）；默认进入歌单页只做节流同步
    void load_playlists(bool force = false);
    void open_playlist(int index);
    void refresh_current_page();   // 刷新按钮：歌单页重新拉列表，歌曲页重拉当前歌单

    void play_from(const std::vector<se::Track>& src, int index);
    void play_current();
    void play_queue_index(int index);
    void play_next(int delta);
    void queue_remove(int index);

    // ── 手柄 ──
    // 绑在 contentView 上（不是 Activity 上）：Activity::registerAction 在
    // createContentView 期间 contentView 还是空的，那时注册会静默失败。
    void setup_gamepad(brls::View* root);
    // 扳机快进/快退：每次回调只累加目标位置，真正的 seek 交给 tick 的
    // 200ms 防抖落地。按住时 borealis 每 100ms 回调一次，如果每次都 seek，
    // 解码器会一直处于"重新定位 + 重新下载"状态，音乐直接卡成一片。
    void trigger_step(int dir);
    void nav_page(int delta);        // 十字左右：主页之间循环
    bool focus_in(brls::View* box) const;
    // 在一排横向按钮里把焦点往 dir(-1/+1) 挪一格。
    // 不用 Application::navigate：它是 private，而且它会把 GONE 的按钮也算进去。
    bool focus_sibling_in(brls::Box* box, int dir);

    void set_status(const std::string& text);
    void load_cover(const se::Track& t, std::shared_ptr<std::atomic<bool>> token);

    // ── 音源 ──
    std::unique_ptr<se::Engine> engine_;
    bool engine_ok_ = false;
    // 音源描述从哪来：优先 SD 卡上的外部文件，没有就用打进 nro 的内置副本。
    // 外部文件缺的段会由内置补齐，所以老的 qqmusic.json 不会把新功能顶掉。
    bool source_external_ = false;
    bool account_ok_      = false;   // 有没有读到登录 Cookie（没读到只影响歌单/会员音质）
    // 歌单同步节流：同一个页面来回切时不至于每次都发请求
    std::chrono::steady_clock::time_point last_sync_{};
    std::string sync_clock_;   // 上次同步成功的时刻，如 "13:54"

    // 两个页面的数据源必须分开：
    // 搜索和歌单都会写"歌曲列表"，共用一个 vector 的话，慢请求回来会把另一页的内容顶掉。
    std::vector<se::Track> search_results_;
    std::vector<se::Track> playlist_songs_;
    std::vector<se::Track>* list_source_ = &search_results_;   // 当前页实际显示的那一份
    std::vector<se::Playlist> playlists_;                      // 账号歌单
    std::vector<se::Track> queue_;                             // 播放队列
    int queue_pos_ = -1;                       // 正在播放的曲目在 queue_ 里的下标

    Page page_         = Page::Home;
    std::string keyword_;
    std::string playlist_name_;    // 当前打开的歌单名
    std::string playlist_id_;
    std::string status_text_;      // 状态栏当前显示的文字
    // 当前这首实际取到的音质档位（"128" / "aac" / "320" / "flac"），
    // resolve 内部可能回退，所以以实际成功那档为准。沉浸页要显示它。
    std::string quality_;
    // 自定义背景：目录里扫到的候选 + 当前选中的那个（空字符串 = 纯色）
    std::vector<std::string> bg_files_;
    std::string bg_name_;
    bool busy_ = false;            // 有异步请求在跑，避免重复触发
    // 请求序号：只有最后发起的那次请求允许落地。
    // 没有这个，先发的慢请求会在后发的请求之后返回，把界面刷成旧内容。
    int req_seq_ = 0;
    brls::View* pending_focus_ = nullptr;   // 重建列表后要把焦点交给谁
    // 当前列表从第几条开始渲染。切页 / 换列表时归零，翻页时按 kPageSize 步进。
    int list_offset_ = 0;

    // 引擎本身不是线程安全的：搜索和取流都串行化
    std::mutex op_mu_;

    // ── UI ──
    brls::Label* status_   = nullptr;
    // 背景层：绝对定位铺满，画在最底下；内容是普通流式布局，不受影响
    brls::Image* bg_image_ = nullptr;
    brls::View* bg_scrim_  = nullptr;   // 渐变压暗层，保证白字在任何图上都读得清
    brls::Box* content_box_ = nullptr;
    brls::ScrollingFrame* scroller_ = nullptr;
    brls::Label* now_title_ = nullptr;
    brls::Image* cover_     = nullptr;
    ui::Icon* cover_placeholder_ = nullptr;   // 没封面时显示的音符
    ui::IconButton* btn_play_ = nullptr;
    ui::IconButton* nav_home_ = nullptr;
    ui::IconButton* nav_search_ = nullptr;
    ui::IconButton* nav_library_ = nullptr;
    ui::IconButton* nav_queue_ = nullptr;
    ui::IconButton* nav_back_ = nullptr;
    ui::IconButton* nav_refresh_ = nullptr;   // 歌单/歌曲页的"重新同步"
    ui::IconButton* nav_settings_ = nullptr;
    brls::Label* time_label_ = nullptr;
    brls::Slider* progress_  = nullptr;
    // 手柄"左右交还导航"的判断锚点：焦点在这两排横排按钮里时，
    // 十字左右应该移动焦点，而不是被我们拿去切页。
    brls::Box* header_     = nullptr;
    brls::Box* footer_row_ = nullptr;

    // 沉浸播放页（浮层）。imm_open_ = 当前是否展开，手柄的方向键拦截要靠它判断。
    bool imm_open_ = false;
    brls::Box* imm_overlay_    = nullptr;   // 整层，绝对定位铺满屏幕
    brls::Image* imm_bg_       = nullptr;   // 整屏封面打底（低透明度）
    brls::Image* imm_cover_    = nullptr;   // 左边的大封面
    ui::Icon* imm_ph_          = nullptr;   // 没有封面时的音符占位
    brls::Label* imm_title_    = nullptr;
    brls::Label* imm_artist_   = nullptr;
    brls::Slider* imm_progress_ = nullptr;
    brls::Label* imm_time_     = nullptr;
    ui::IconButton* imm_play_  = nullptr;
    std::string imm_cover_url_;            // 已经喂给 imm_cover_ 的那张封面地址
    // 扳机快进/快退的按住状态：-1 后退 / +1 前进 / 0 未按
    int trigger_dir_            = 0;
    double trigger_base_sec_    = 0.0;
    std::chrono::steady_clock::time_point trigger_hold_at_{};

    // borealis 的 Slider::setProgress() 内部会 fire progressEvent。
    // 于是 tick 里刷一次进度条，就会被当成"用户拖动"再触发一次 seek。
    // 这个标志用来在程序自己刷新期间挡住那个回调。
    bool suppress_seek_ = false;
    bool seek_pending_  = false;   // 用户拖过进度条，等着落地
    double seek_target_sec_ = 0.0;
    std::chrono::steady_clock::time_point seek_touch_at_{};
    PollTask* poll_ = nullptr;

    // 封面 JPEG 原始字节。nanovg 解码后会把纹理传到 GPU，
    // 但保留这一份可以避免某些后端在 resize 时重新读已释放的内存。
    std::vector<uint8_t> cover_bytes_;
    std::string cover_url_;   // 正在显示的封面地址，用来避免重复下载同一张

    bool ended_handled_ = false;
    std::string last_error_;

    // 异步任务的生命周期令牌：activity 销毁后回调立刻失效，避免野指针
    std::shared_ptr<std::atomic<bool>> alive_;
};

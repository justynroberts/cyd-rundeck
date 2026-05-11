#include "ui.h"
#include "config.h"
#include "storage.h"

#include <Arduino.h>
#include <lvgl.h>
#include <vector>
#include <algorithm>
#include "fonts/fonts.h"
#include "assets_gen.h"

// ===========================================================================
// Rundeck-themed UI on a 320x240 CYD.
//
// Layout (landscape):
//   ┌───────────────────────────────────────────────────┐
//   │ [gear] RUNDECK · <project>            HH:MM  WiFi │  18px top bar
//   ├───────────────────────────────────────────────────┤
//   │  ┌─────────┐ ┌─────────┐ ┌─────────┐              │
//   │  │ RUNNING │ │   OK    │ │  FAIL   │   3 KPI tiles│
//   │  │   3     │ │   42    │ │   2     │              │
//   │  └─────────┘ └─────────┘ └─────────┘              │
//   │                                                   │
//   │  ─── RECENT EXECUTIONS ──────────────────         │
//   │  ● job_name              project   ·  5m  >       │  scroll list
//   │  ● ...                                            │
//   └───────────────────────────────────────────────────┘
//
// Tapping a row -> detail screen with ABORT button if RUNNING.
// Caches refs to value labels to avoid LVGL heap fragmentation (rebuilding
// the list every poll fragments the pool — only update text in place).
// ===========================================================================

// ---- palette --------------------------------------------------------------

// High-contrast palette: pure black, pure white text, red the only accent.
#define COL_BG       lv_color_hex(0x000000)
#define COL_PANEL    lv_color_hex(0x0a0a0a)
#define COL_BORDER   lv_color_hex(0x2a0a10)   // very dim red, subtle
#define COL_TEXT     lv_color_hex(0xffffff)
#define COL_MUTED    lv_color_hex(0x7a7a7a)
#define COL_BLUE     lv_color_hex(0xff2942)   // "blue" symbolically — now red accent
#define COL_GREEN    lv_color_hex(0x22dd66)   // success = vivid green
#define COL_RED      lv_color_hex(0xff2942)   // failures = vivid red
#define COL_AMBER    lv_color_hex(0xff8090)   // warning = pink-red
#define COL_GREY     lv_color_hex(0x444444)

// ---- state ----------------------------------------------------------------

static rundeck::Snapshot s_snap;
static bool              s_haveSnap = false;
static String            s_lastError;
static String            s_baseUrl;
static String            s_version;
static String            s_project;

// Cached widget refs.
static lv_obj_t* scr_dashboard = nullptr;
static lv_obj_t* lbl_topbar_proj = nullptr;
static lv_obj_t* lbl_topbar_wifi = nullptr;

// Panes
static lv_obj_t* pane_exec  = nullptr;
static lv_obj_t* pane_roi   = nullptr;
static lv_obj_t* pane_users = nullptr;
static int       s_activePane = 0;   // 0=exec, 1=roi, 2=users

// Nav bar
static lv_obj_t* nav_btn[3] = { nullptr, nullptr, nullptr };
static lv_obj_t* nav_lbl[3] = { nullptr, nullptr, nullptr };

// Exec pane — two rotating subviews
static lv_obj_t* exec_view_kpi  = nullptr;  // huge tiles
static lv_obj_t* exec_view_list = nullptr;  // recent executions
static int       s_execSubview  = 0;        // 0 = kpi, 1 = list
static uint32_t  s_execRotateAt = 0;        // millis when next swap is due

// Big KPI labels
static lv_obj_t* lbl_big_running = nullptr;
static lv_obj_t* lbl_big_ok = nullptr;
static lv_obj_t* lbl_big_fail = nullptr;

// (legacy small KPIs kept for ROI/other use)
static lv_obj_t* lbl_kpi_running = nullptr;
static lv_obj_t* lbl_kpi_ok = nullptr;
static lv_obj_t* lbl_kpi_fail = nullptr;

static lv_obj_t* obj_list_holder = nullptr;
static lv_obj_t* lbl_footer = nullptr;

// ROI pane
static lv_obj_t* lbl_roi_runs = nullptr;
static lv_obj_t* lbl_roi_success = nullptr;
static lv_obj_t* lbl_roi_time = nullptr;
static lv_obj_t* lbl_roi_dollars = nullptr;
static lv_obj_t* bar_roi_success = nullptr;
static lv_obj_t* lbl_roi_avg = nullptr;

// Users pane (top 5 rows)
struct UserRow {
    lv_obj_t* row;
    lv_obj_t* name;
    lv_obj_t* bar;
    lv_obj_t* count;
};
static UserRow s_userRows[5] = {};

// ROI assumptions (could become NVS knobs later)
static constexpr int   ROI_MINS_PER_RUN = 5;   // labor saved per execution
static constexpr float ROI_RATE_USD_HR  = 50.0f;

static ui::RefreshCb s_refreshCb = nullptr;

// list row cache (so we don't realloc every refresh)
struct RowRefs {
    lv_obj_t* row;
    lv_obj_t* dot;
    lv_obj_t* title;
    lv_obj_t* sub;
    long      execId;
};
static std::vector<RowRefs> s_rows;

// detail screen
static lv_obj_t* scr_detail = nullptr;
static long      s_detail_exec_id = -1;
static lv_obj_t* lbl_detail_title = nullptr;
static lv_obj_t* lbl_detail_status = nullptr;
static lv_obj_t* lbl_detail_meta = nullptr;
static lv_obj_t* btn_abort = nullptr;
static lv_obj_t* lbl_detail_result = nullptr;

// ---- helpers --------------------------------------------------------------

static void setBgDark(lv_obj_t* o) {
    lv_obj_set_style_bg_color(o, COL_BG, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
}

static lv_obj_t* makePanel(lv_obj_t* parent) {
    lv_obj_t* p = lv_obj_create(parent);
    lv_obj_set_style_bg_color(p, COL_PANEL, 0);
    lv_obj_set_style_border_color(p, COL_BORDER, 0);
    lv_obj_set_style_border_width(p, 1, 0);
    lv_obj_set_style_radius(p, 6, 0);
    lv_obj_set_style_pad_all(p, 4, 0);
    return p;
}

static lv_color_t colorFromHex(uint32_t h) { return lv_color_hex(h); }

// Strip non-printable / non-ASCII (incl. emoji) so unsupported glyphs don't
// render as squares. Collapses any run of stripped chars into a single space.
static String asciiSafe(const String& in) {
    String out;
    out.reserve(in.length());
    bool lastSpace = false;
    for (size_t i = 0; i < in.length(); ++i) {
        unsigned char c = (unsigned char)in[i];
        if (c >= 0x20 && c < 0x7F) {
            out += (char)c;
            lastSpace = false;
        } else if (!lastSpace && out.length() > 0) {
            out += ' ';
            lastSpace = true;
        }
    }
    // Trim trailing space
    while (out.length() > 0 && out[out.length()-1] == ' ')
        out.remove(out.length()-1);
    return out.length() ? out : String("?");
}

static String shortAge(uint32_t epoch) {
    if (epoch == 0) return String("-");
    time_t now = time(nullptr);
    if (now < epoch || now == 0) return String("now");
    uint32_t d = (uint32_t)(now - epoch);
    if (d < 60)        return String(d) + "s";
    if (d < 3600)      return String(d / 60) + "m";
    if (d < 86400)     return String(d / 3600) + "h";
    return String(d / 86400) + "d";
}

// ---- top bar / common chrome ----------------------------------------------

static void buildTopBar(lv_obj_t* parent) {
    lv_obj_t* bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 320, 22);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(bar, COL_BORDER, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    // Brand: Rundeck logo (18px tall)
    lv_obj_t* brand = lv_img_create(bar);
    lv_img_set_src(brand, &rundeck_logo_18);
    lv_obj_align(brand, LV_ALIGN_LEFT_MID, 4, 0);

    // Project (or "all projects")
    lbl_topbar_proj = lv_label_create(bar);
    lv_label_set_text(lbl_topbar_proj, "");
    lv_obj_set_style_text_color(lbl_topbar_proj, COL_MUTED, 0);
    lv_obj_set_style_text_font(lbl_topbar_proj, &plex_medium_12, 0);
    lv_obj_align(lbl_topbar_proj, LV_ALIGN_LEFT_MID, 92, 0);
    lv_label_set_long_mode(lbl_topbar_proj, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl_topbar_proj, 180);

    // Refresh button (tap = force immediate poll)
    lv_obj_t* refreshBtn = lv_obj_create(bar);
    lv_obj_remove_style_all(refreshBtn);
    lv_obj_set_size(refreshBtn, 28, 20);
    lv_obj_align(refreshBtn, LV_ALIGN_RIGHT_MID, -30, 0);
    lv_obj_add_flag(refreshBtn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(refreshBtn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(refreshBtn, [](lv_event_t*) {
        if (s_refreshCb) s_refreshCb();
        if (lbl_footer) lv_label_set_text(lbl_footer, "refreshing...");
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* rl = lv_label_create(refreshBtn);
    lv_label_set_text(rl, "R");
    lv_obj_set_style_text_color(rl, COL_MUTED, 0);
    lv_obj_set_style_text_font(rl, &plex_medium_14, 0);
    lv_obj_center(rl);

    // WiFi indicator
    lbl_topbar_wifi = lv_label_create(bar);
    lv_label_set_text(lbl_topbar_wifi, "W");
    lv_obj_set_style_text_color(lbl_topbar_wifi, COL_GREEN, 0);
    lv_obj_set_style_text_font(lbl_topbar_wifi, &plex_medium_14, 0);
    lv_obj_align(lbl_topbar_wifi, LV_ALIGN_RIGHT_MID, -6, 0);
}

// ---- splash ---------------------------------------------------------------

static lv_obj_t* scr_splash = nullptr;
static lv_obj_t* lbl_splash_status = nullptr;

void ui::showSplash() {
    if (!scr_splash) {
        scr_splash = lv_obj_create(NULL);
        setBgDark(scr_splash);

        lv_obj_t* logo = lv_img_create(scr_splash);
        lv_img_set_src(logo, &rundeck_logo_80);
        lv_obj_align(logo, LV_ALIGN_CENTER, 0, -20);

        lv_obj_t* sub = lv_label_create(scr_splash);
        lv_label_set_text(sub, "CYD companion");
        lv_obj_set_style_text_color(sub, COL_MUTED, 0);
        lv_obj_set_style_text_font(sub, &plex_medium_14, 0);
        lv_obj_align(sub, LV_ALIGN_CENTER, 0, 34);

        lv_obj_t* sp = lv_spinner_create(scr_splash, 1000, 60);
        lv_obj_set_size(sp, 28, 28);
        lv_obj_align(sp, LV_ALIGN_CENTER, 0, 50);
        lv_obj_set_style_arc_color(sp, COL_BORDER, 0);
        lv_obj_set_style_arc_color(sp, COL_BLUE, LV_PART_INDICATOR);

        lbl_splash_status = lv_label_create(scr_splash);
        lv_label_set_text(lbl_splash_status, "Starting...");
        lv_obj_set_style_text_color(lbl_splash_status, COL_MUTED, 0);
        lv_obj_set_style_text_font(lbl_splash_status, &plex_medium_12, 0);
        lv_obj_align(lbl_splash_status, LV_ALIGN_BOTTOM_MID, 0, -8);
    }
    lv_scr_load(scr_splash);
}

void ui::showStatus(const String& msg) {
    if (lbl_splash_status && lv_scr_act() == scr_splash) {
        lv_label_set_text(lbl_splash_status, msg.c_str());
    } else if (lbl_footer) {
        lv_label_set_text(lbl_footer, msg.c_str());
    }
}

// ---- captive portal screens ----------------------------------------------

static lv_obj_t* scr_wifi = nullptr;
void ui::showWifiPortal(const String& ssid, const String& pass) {
    if (scr_wifi) { lv_obj_del(scr_wifi); scr_wifi = nullptr; }
    scr_wifi = lv_obj_create(NULL);
    setBgDark(scr_wifi);

    lv_obj_t* hlogo = lv_img_create(scr_wifi);
    lv_img_set_src(hlogo, &rundeck_logo_32);
    lv_obj_align(hlogo, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_t* title = lv_label_create(scr_wifi);
    lv_label_set_text(title, "WIFI SETUP");
    lv_obj_set_style_text_color(title, COL_BLUE, 0);
    lv_obj_set_style_text_font(title, &plex_medium_14, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 46);

    lv_obj_t* p = makePanel(scr_wifi);
    lv_obj_set_size(p, 300, 150);
    lv_obj_align(p, LV_ALIGN_CENTER, 0, 8);

    lv_obj_t* sub = lv_label_create(p);
    lv_label_set_text(sub, "Join this WiFi to set up:");
    lv_obj_set_style_text_color(sub, COL_MUTED, 0);
    lv_obj_set_style_text_font(sub, &plex_medium_12, 0);
    lv_obj_align(sub, LV_ALIGN_TOP_LEFT, 4, 4);

    lv_obj_t* ssidLabel = lv_label_create(p);
    lv_label_set_text_fmt(ssidLabel, "SSID:  %s", ssid.c_str());
    lv_obj_set_style_text_color(ssidLabel, COL_TEXT, 0);
    lv_obj_set_style_text_font(ssidLabel, &plex_medium_16, 0);
    lv_obj_align(ssidLabel, LV_ALIGN_TOP_LEFT, 4, 26);

    lv_obj_t* passLabel = lv_label_create(p);
    lv_label_set_text_fmt(passLabel, "Pass:  %s", pass.c_str());
    lv_obj_set_style_text_color(passLabel, COL_TEXT, 0);
    lv_obj_set_style_text_font(passLabel, &plex_medium_16, 0);
    lv_obj_align(passLabel, LV_ALIGN_TOP_LEFT, 4, 50);

    lv_obj_t* hint = lv_label_create(p);
    lv_label_set_text(hint, "Then open: http://192.168.4.1");
    lv_obj_set_style_text_color(hint, COL_BLUE, 0);
    lv_obj_set_style_text_font(hint, &plex_medium_14, 0);
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 4, 80);

    lv_obj_t* hint2 = lv_label_create(p);
    lv_label_set_text(hint2, "(captive portal usually opens itself)");
    lv_obj_set_style_text_color(hint2, COL_MUTED, 0);
    lv_obj_set_style_text_font(hint2, &plex_medium_12, 0);
    lv_obj_align(hint2, LV_ALIGN_TOP_LEFT, 4, 104);

    lv_scr_load_anim(scr_wifi, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
}

static lv_obj_t* scr_setup = nullptr;
void ui::showSetupPortal(const String& url) {
    if (scr_setup) { lv_obj_del(scr_setup); scr_setup = nullptr; }
    scr_setup = lv_obj_create(NULL);
    setBgDark(scr_setup);

    lv_obj_t* slogo = lv_img_create(scr_setup);
    lv_img_set_src(slogo, &rundeck_logo_32);
    lv_obj_align(slogo, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_t* title = lv_label_create(scr_setup);
    lv_label_set_text(title, "SETUP");
    lv_obj_set_style_text_color(title, COL_BLUE, 0);
    lv_obj_set_style_text_font(title, &plex_medium_14, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 46);

    lv_obj_t* p = makePanel(scr_setup);
    lv_obj_set_size(p, 300, 160);
    lv_obj_align(p, LV_ALIGN_CENTER, 0, 8);

    lv_obj_t* l1 = lv_label_create(p);
    lv_label_set_text(l1, "Open in a browser:");
    lv_obj_set_style_text_color(l1, COL_MUTED, 0);
    lv_obj_set_style_text_font(l1, &plex_medium_14, 0);
    lv_obj_align(l1, LV_ALIGN_TOP_LEFT, 6, 6);

    lv_obj_t* l2 = lv_label_create(p);
    lv_label_set_text(l2, url.c_str());
    lv_obj_set_style_text_color(l2, COL_BLUE, 0);
    lv_obj_set_style_text_font(l2, &plex_medium_16, 0);
    lv_obj_align(l2, LV_ALIGN_TOP_LEFT, 6, 30);
    lv_label_set_long_mode(l2, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l2, 280);

    lv_obj_t* l3 = lv_label_create(p);
    lv_label_set_text(l3,
        "Enter your Rundeck base URL and\n"
        "API token (User Profile -> Tokens)");
    lv_obj_set_style_text_color(l3, COL_TEXT, 0);
    lv_obj_set_style_text_font(l3, &plex_medium_12, 0);
    lv_obj_align(l3, LV_ALIGN_TOP_LEFT, 6, 86);

    lv_scr_load_anim(scr_setup, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
}

// ---- forward refs ---------------------------------------------------------
static void rebuildExecutionList();
static void renderDetail();

// ---- dashboard ------------------------------------------------------------

static void onRowClick(lv_event_t* e) {
    lv_obj_t* row = lv_event_get_target(e);
    long id = (long)(intptr_t)lv_obj_get_user_data(row);
    if (id != 0) ui::showDetail(id);
}

static void onBackClick(lv_event_t* /*e*/) {
    ui::showDashboard();
}

static void onAbortClick(lv_event_t* /*e*/) {
    if (s_detail_exec_id < 0) return;
    String err;
    bool ok = rundeck::abortExecution(s_detail_exec_id, err);
    if (lbl_detail_result) {
        if (ok) {
            lv_label_set_text(lbl_detail_result, "ABORT requested");
            lv_obj_set_style_text_color(lbl_detail_result, COL_GREEN, 0);
        } else {
            String m = String("! ") + err;
            lv_label_set_text(lbl_detail_result, m.c_str());
            lv_obj_set_style_text_color(lbl_detail_result, COL_RED, 0);
        }
    }
}

static lv_obj_t* makeKpiTile(lv_obj_t* parent, int x, const char* label,
                             lv_color_t accent, lv_obj_t** outValueLabel) {
    lv_obj_t* t = lv_obj_create(parent);
    lv_obj_remove_style_all(t);
    lv_obj_set_size(t, 98, 56);
    lv_obj_set_pos(t, x, 26);
    lv_obj_set_style_bg_color(t, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(t, COL_BORDER, 0);
    lv_obj_set_style_border_width(t, 1, 0);
    lv_obj_set_style_radius(t, 6, 0);
    lv_obj_set_style_pad_all(t, 4, 0);
    lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);

    // accent stripe (left edge)
    lv_obj_t* stripe = lv_obj_create(t);
    lv_obj_remove_style_all(stripe);
    lv_obj_set_size(stripe, 3, 48);
    lv_obj_align(stripe, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(stripe, accent, 0);
    lv_obj_set_style_bg_opa(stripe, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(stripe, 2, 0);

    lv_obj_t* lblLabel = lv_label_create(t);
    lv_label_set_text(lblLabel, label);
    lv_obj_set_style_text_color(lblLabel, COL_MUTED, 0);
    lv_obj_set_style_text_font(lblLabel, &plex_medium_12, 0);
    lv_obj_align(lblLabel, LV_ALIGN_TOP_LEFT, 8, 2);

    lv_obj_t* lblVal = lv_label_create(t);
    lv_label_set_text(lblVal, "-");
    lv_obj_set_style_text_color(lblVal, COL_TEXT, 0);
    lv_obj_set_style_text_font(lblVal, &plex_bold_22, 0);
    lv_obj_align(lblVal, LV_ALIGN_BOTTOM_LEFT, 8, -2);

    *outValueLabel = lblVal;
    return t;
}

// ---- pane: exec ----------------------------------------------------------

static lv_obj_t* makePane(lv_obj_t* parent) {
    lv_obj_t* p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_set_pos(p, 0, 22);
    lv_obj_set_size(p, 320, 194);      // 240 - 22 topbar - 24 nav
    lv_obj_set_style_bg_color(p, COL_BG, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(p, 0, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

static void buildExecPane(lv_obj_t* parent) {
    pane_exec = makePane(parent);

    // -------- Subview A: BIG KPI tiles (full pane area minus footer) --------
    exec_view_kpi = lv_obj_create(pane_exec);
    lv_obj_remove_style_all(exec_view_kpi);
    lv_obj_set_size(exec_view_kpi, 320, 178);
    lv_obj_set_pos(exec_view_kpi, 0, 0);
    lv_obj_set_style_bg_color(exec_view_kpi, COL_BG, 0);
    lv_obj_set_style_bg_opa(exec_view_kpi, LV_OPA_COVER, 0);
    lv_obj_clear_flag(exec_view_kpi, LV_OBJ_FLAG_SCROLLABLE);

    // Big Rundeck logo at top of the giant-KPI subview
    lv_obj_t* bigLogo = lv_img_create(exec_view_kpi);
    lv_img_set_src(bigLogo, &rundeck_logo_32);
    lv_obj_align(bigLogo, LV_ALIGN_TOP_MID, 0, 4);

    auto bigTile = [&](int x, const char* label, lv_color_t accent, lv_obj_t** outVal) {
        lv_obj_t* t = lv_obj_create(exec_view_kpi);
        lv_obj_remove_style_all(t);
        lv_obj_set_size(t, 100, 128);
        lv_obj_set_pos(t, x, 44);
        lv_obj_set_style_bg_color(t, COL_PANEL, 0);
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(t, accent, 0);
        lv_obj_set_style_border_width(t, 2, 0);
        lv_obj_set_style_radius(t, 4, 0);
        lv_obj_set_style_pad_all(t, 4, 0);
        lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* lblLabel = lv_label_create(t);
        lv_label_set_text(lblLabel, label);
        lv_obj_set_style_text_color(lblLabel, accent, 0);
        lv_obj_set_style_text_font(lblLabel, &plex_medium_16, 0);
        lv_obj_align(lblLabel, LV_ALIGN_TOP_MID, 0, 6);

        lv_obj_t* lblVal = lv_label_create(t);
        lv_label_set_text(lblVal, "-");
        lv_obj_set_style_text_color(lblVal, COL_TEXT, 0);
        lv_obj_set_style_text_font(lblVal, &plex_bold_48, 0);
        lv_obj_align(lblVal, LV_ALIGN_CENTER, 0, 18);
        *outVal = lblVal;
    };
    bigTile(  6, "RUN",  COL_BLUE,  &lbl_big_running);
    bigTile(110, "OK",   COL_GREEN, &lbl_big_ok);
    bigTile(214, "FAIL", COL_RED,   &lbl_big_fail);

    // -------- Subview B: list --------
    exec_view_list = lv_obj_create(pane_exec);
    lv_obj_remove_style_all(exec_view_list);
    lv_obj_set_size(exec_view_list, 320, 178);
    lv_obj_set_pos(exec_view_list, 0, 0);
    lv_obj_set_style_bg_color(exec_view_list, COL_BG, 0);
    lv_obj_set_style_bg_opa(exec_view_list, LV_OPA_COVER, 0);
    lv_obj_clear_flag(exec_view_list, LV_OBJ_FLAG_SCROLLABLE);

    // Compact KPI strip (smaller, top)
    auto kpi = [&](int x, const char* label, lv_color_t accent, lv_obj_t** out) {
        lv_obj_t* t = lv_obj_create(exec_view_list);
        lv_obj_remove_style_all(t);
        lv_obj_set_size(t, 100, 36);
        lv_obj_set_pos(t, x, 2);
        lv_obj_set_style_bg_color(t, COL_PANEL, 0);
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(t, accent, 0);
        lv_obj_set_style_border_width(t, 1, 0);
        lv_obj_set_style_radius(t, 4, 0);
        lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* lblLabel = lv_label_create(t);
        lv_label_set_text(lblLabel, label);
        lv_obj_set_style_text_color(lblLabel, accent, 0);
        lv_obj_set_style_text_font(lblLabel, &plex_medium_12, 0);
        lv_obj_align(lblLabel, LV_ALIGN_TOP_LEFT, 6, 1);

        lv_obj_t* lblVal = lv_label_create(t);
        lv_label_set_text(lblVal, "-");
        lv_obj_set_style_text_color(lblVal, COL_TEXT, 0);
        lv_obj_set_style_text_font(lblVal, &plex_bold_22, 0);
        lv_obj_align(lblVal, LV_ALIGN_BOTTOM_RIGHT, -6, -1);
        *out = lblVal;
    };
    kpi(  6, "RUN",  COL_BLUE,  &lbl_kpi_running);
    kpi(110, "OK",   COL_GREEN, &lbl_kpi_ok);
    kpi(214, "FAIL", COL_RED,   &lbl_kpi_fail);

    // Scroll list — taller rows
    obj_list_holder = lv_obj_create(exec_view_list);
    lv_obj_remove_style_all(obj_list_holder);
    lv_obj_set_pos(obj_list_holder, 0, 44);
    lv_obj_set_size(obj_list_holder, 320, 134);
    lv_obj_set_style_bg_color(obj_list_holder, COL_BG, 0);
    lv_obj_set_style_bg_opa(obj_list_holder, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(obj_list_holder, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(obj_list_holder, 3, 0);
    lv_obj_set_style_pad_all(obj_list_holder, 4, 0);
    lv_obj_set_scrollbar_mode(obj_list_holder, LV_SCROLLBAR_MODE_AUTO);

    // Footer (shared, lives on pane_exec under both views)
    lbl_footer = lv_label_create(pane_exec);
    lv_label_set_text(lbl_footer, "");
    lv_obj_set_style_text_color(lbl_footer, COL_MUTED, 0);
    lv_obj_set_style_text_font(lbl_footer, &plex_medium_12, 0);
    lv_obj_align(lbl_footer, LV_ALIGN_BOTTOM_LEFT, 8, -2);

    // Start on big KPI view; will toggle from main loop's renderCurrentPane tick
    lv_obj_clear_flag(exec_view_kpi, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(exec_view_list, LV_OBJ_FLAG_HIDDEN);
    s_execSubview  = 0;
    s_execRotateAt = millis() + 20000;
}

static void setExecSubview(int idx) {
    s_execSubview = idx;
    if (idx == 0) {
        lv_obj_clear_flag(exec_view_kpi, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(exec_view_list, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(exec_view_kpi, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(exec_view_list, LV_OBJ_FLAG_HIDDEN);
    }
    s_execRotateAt = millis() + 20000;
}

// ---- pane: ROI -----------------------------------------------------------

static lv_obj_t* makeRoiTile(lv_obj_t* parent, int x, int y, int w, int h,
                             const char* label, lv_color_t accent,
                             const char* font, lv_obj_t** outVal) {
    lv_obj_t* t = lv_obj_create(parent);
    lv_obj_remove_style_all(t);
    lv_obj_set_size(t, w, h);
    lv_obj_set_pos(t, x, y);
    lv_obj_set_style_bg_color(t, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(t, COL_BORDER, 0);
    lv_obj_set_style_border_width(t, 1, 0);
    lv_obj_set_style_radius(t, 8, 0);
    lv_obj_set_style_pad_all(t, 6, 0);
    lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* lblLabel = lv_label_create(t);
    lv_label_set_text(lblLabel, label);
    lv_obj_set_style_text_color(lblLabel, COL_MUTED, 0);
    lv_obj_set_style_text_font(lblLabel, &plex_medium_12, 0);
    lv_obj_set_style_pad_bottom(lblLabel, 5, 0);   // breathing room beneath label
    lv_obj_align(lblLabel, LV_ALIGN_TOP_LEFT, 2, 2);

    lv_obj_t* lblVal = lv_label_create(t);
    lv_label_set_text(lblVal, "-");
    lv_obj_set_style_text_color(lblVal, accent, 0);
    if (strcmp(font, "big") == 0)
        lv_obj_set_style_text_font(lblVal, &plex_bold_28, 0);
    else
        lv_obj_set_style_text_font(lblVal, &plex_bold_22, 0);
    lv_obj_align(lblVal, LV_ALIGN_BOTTOM_LEFT, 2, -2);
    *outVal = lblVal;
    return t;
}

static void buildRoiPane(lv_obj_t* parent) {
    pane_roi = makePane(parent);

    // Hero tile: hours saved (full width)
    makeRoiTile(pane_roi, 6, 4, 308, 56, "TIME SAVED (RECENT)",
                COL_BLUE, "big", &lbl_roi_time);

    // 2x2 grid below: Runs, Success rate, $ saved, Avg (taller — was 44)
    makeRoiTile(pane_roi,   6,  66, 152, 50, "RUNS",        COL_TEXT,  "med", &lbl_roi_runs);
    makeRoiTile(pane_roi, 162,  66, 152, 50, "SUCCESS",     COL_GREEN, "med", &lbl_roi_success);
    makeRoiTile(pane_roi,   6, 120, 152, 50, "USD SAVED",   COL_AMBER, "med", &lbl_roi_dollars);
    makeRoiTile(pane_roi, 162, 120, 152, 50, "AVG DURATION",COL_TEXT,  "med", &lbl_roi_avg);

    // Success-rate bar at the bottom
    lv_obj_t* barBg = lv_obj_create(pane_roi);
    lv_obj_remove_style_all(barBg);
    lv_obj_set_size(barBg, 308, 8);
    lv_obj_set_pos(barBg, 6, 178);
    lv_obj_set_style_bg_color(barBg, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(barBg, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(barBg, 4, 0);
    lv_obj_set_style_border_color(barBg, COL_BORDER, 0);
    lv_obj_set_style_border_width(barBg, 1, 0);

    bar_roi_success = lv_obj_create(barBg);
    lv_obj_remove_style_all(bar_roi_success);
    lv_obj_set_size(bar_roi_success, 0, 6);
    lv_obj_align(bar_roi_success, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(bar_roi_success, COL_GREEN, 0);
    lv_obj_set_style_bg_opa(bar_roi_success, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bar_roi_success, 3, 0);

    lv_obj_add_flag(pane_roi, LV_OBJ_FLAG_HIDDEN);
}

// ---- pane: USERS ---------------------------------------------------------

static void buildUsersPane(lv_obj_t* parent) {
    pane_users = makePane(parent);

    lv_obj_t* sec = lv_label_create(pane_users);
    lv_label_set_text(sec, "TOP USERS (RECENT)");
    lv_obj_set_style_text_color(sec, COL_MUTED, 0);
    lv_obj_set_style_text_font(sec, &plex_medium_12, 0);
    lv_obj_align(sec, LV_ALIGN_TOP_LEFT, 8, 4);

    for (int i = 0; i < 5; i++) {
        UserRow& u = s_userRows[i];
        u.row = lv_obj_create(pane_users);
        lv_obj_remove_style_all(u.row);
        lv_obj_set_size(u.row, 308, 28);
        lv_obj_set_pos(u.row, 6, 24 + i * 32);
        lv_obj_set_style_bg_color(u.row, COL_PANEL, 0);
        lv_obj_set_style_bg_opa(u.row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(u.row, COL_BORDER, 0);
        lv_obj_set_style_border_width(u.row, 1, 0);
        lv_obj_set_style_radius(u.row, 6, 0);
        lv_obj_clear_flag(u.row, LV_OBJ_FLAG_SCROLLABLE);

        u.name = lv_label_create(u.row);
        lv_obj_set_style_text_color(u.name, COL_TEXT, 0);
        lv_obj_set_style_text_font(u.name, &plex_medium_14, 0);
        lv_obj_align(u.name, LV_ALIGN_LEFT_MID, 8, 0);
        lv_label_set_long_mode(u.name, LV_LABEL_LONG_DOT);
        lv_obj_set_width(u.name, 90);

        // Bar background
        lv_obj_t* bg = lv_obj_create(u.row);
        lv_obj_remove_style_all(bg);
        lv_obj_set_size(bg, 150, 6);
        lv_obj_align(bg, LV_ALIGN_LEFT_MID, 110, 0);
        lv_obj_set_style_bg_color(bg, COL_BG, 0);
        lv_obj_set_style_bg_opa(bg, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(bg, 3, 0);

        u.bar = lv_obj_create(bg);
        lv_obj_remove_style_all(u.bar);
        lv_obj_set_size(u.bar, 0, 6);
        lv_obj_align(u.bar, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_set_style_bg_color(u.bar, COL_BLUE, 0);
        lv_obj_set_style_bg_opa(u.bar, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(u.bar, 3, 0);

        u.count = lv_label_create(u.row);
        lv_obj_set_style_text_color(u.count, COL_MUTED, 0);
        lv_obj_set_style_text_font(u.count, &plex_medium_14, 0);
        lv_obj_align(u.count, LV_ALIGN_RIGHT_MID, -8, 0);

        lv_obj_add_flag(u.row, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_add_flag(pane_users, LV_OBJ_FLAG_HIDDEN);
}

// ---- bottom nav strip ----------------------------------------------------

static void onNavClick(lv_event_t* e);

static void buildNavBar(lv_obj_t* parent) {
    lv_obj_t* bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 320, 24);
    lv_obj_set_pos(bar, 0, 216);
    lv_obj_set_style_bg_color(bar, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(bar, COL_BORDER, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    const char* labels[3] = { "EXEC", "ROI", "USERS" };
    int widths[3] = { 106, 107, 107 };
    int xs[3]     = { 0, 106, 213 };
    for (int i = 0; i < 3; i++) {
        lv_obj_t* b = lv_obj_create(bar);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, widths[i], 24);
        lv_obj_set_pos(b, xs[i], 0);
        lv_obj_set_style_bg_color(b, COL_PANEL, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_user_data(b, (void*)(intptr_t)i);
        lv_obj_add_event_cb(b, onNavClick, LV_EVENT_CLICKED, nullptr);

        lv_obj_t* l = lv_label_create(b);
        lv_label_set_text(l, labels[i]);
        lv_obj_set_style_text_color(l, COL_MUTED, 0);
        lv_obj_set_style_text_font(l, &plex_medium_14, 0);
        lv_obj_center(l);

        // Active indicator (top stripe)
        lv_obj_t* stripe = lv_obj_create(b);
        lv_obj_remove_style_all(stripe);
        lv_obj_set_size(stripe, widths[i] - 8, 2);
        lv_obj_align(stripe, LV_ALIGN_TOP_MID, 0, 1);
        lv_obj_set_style_bg_color(stripe, COL_BLUE, 0);
        lv_obj_set_style_bg_opa(stripe, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(stripe, 1, 0);
        lv_obj_add_flag(stripe, LV_OBJ_FLAG_HIDDEN);

        nav_btn[i] = stripe;
        nav_lbl[i] = l;
    }
}

static void renderExecPane();
static void renderRoiPane();
static void renderUsersPane();
static void renderCurrentPane();

static void setActivePane(int idx) {
    if (idx < 0 || idx > 2) return;
    s_activePane = idx;
    if (pane_exec)  (idx == 0) ? lv_obj_clear_flag(pane_exec,  LV_OBJ_FLAG_HIDDEN) : lv_obj_add_flag(pane_exec,  LV_OBJ_FLAG_HIDDEN);
    if (pane_roi)   (idx == 1) ? lv_obj_clear_flag(pane_roi,   LV_OBJ_FLAG_HIDDEN) : lv_obj_add_flag(pane_roi,   LV_OBJ_FLAG_HIDDEN);
    if (pane_users) (idx == 2) ? lv_obj_clear_flag(pane_users, LV_OBJ_FLAG_HIDDEN) : lv_obj_add_flag(pane_users, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < 3; i++) {
        if (nav_btn[i]) (i == idx) ? lv_obj_clear_flag(nav_btn[i], LV_OBJ_FLAG_HIDDEN) : lv_obj_add_flag(nav_btn[i], LV_OBJ_FLAG_HIDDEN);
        if (nav_lbl[i]) lv_obj_set_style_text_color(nav_lbl[i], (i == idx) ? COL_TEXT : COL_MUTED, 0);
    }
    renderCurrentPane();
}

static void onNavClick(lv_event_t* e) {
    int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    setActivePane(idx);
}

static void buildDashboard() {
    if (scr_dashboard) return;
    scr_dashboard = lv_obj_create(NULL);
    setBgDark(scr_dashboard);
    lv_obj_set_style_pad_all(scr_dashboard, 0, 0);
    lv_obj_clear_flag(scr_dashboard, LV_OBJ_FLAG_SCROLLABLE);

    buildTopBar(scr_dashboard);
    buildExecPane(scr_dashboard);
    buildRoiPane(scr_dashboard);
    buildUsersPane(scr_dashboard);
    buildNavBar(scr_dashboard);

    setActivePane(0);
}

void ui::showDashboard() {
    buildDashboard();

    // Top-bar project label
    if (lbl_topbar_proj) {
        if (s_project.length() > 0) {
            lv_label_set_text(lbl_topbar_proj, s_project.c_str());
        } else {
            lv_label_set_text(lbl_topbar_proj, "all projects");
        }
    }

    if (!s_haveSnap && lbl_footer) {
        lv_label_set_text(lbl_footer, "loading executions...");
    }

    renderCurrentPane();

    if (lv_scr_act() != scr_dashboard) {
        lv_scr_load_anim(scr_dashboard, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
    }
}

void ui::showExecutions() { ui::showDashboard(); }

// ---- list rebuild ---------------------------------------------------------

static const rundeck::Execution* findExecById(long id) {
    for (auto& e : s_snap.recent) if (e.id == id) return &e;
    return nullptr;
}

static void renderExecPane() {
    if (!obj_list_holder) return;

    // Big tiles + small strip both reflect the same numbers.
    String r = String(s_snap.running);
    String o = String(s_snap.succeededRecent);
    String f = String(s_snap.failedRecent);
    if (lbl_big_running) lv_label_set_text(lbl_big_running, r.c_str());
    if (lbl_big_ok)      lv_label_set_text(lbl_big_ok,      o.c_str());
    if (lbl_big_fail)    lv_label_set_text(lbl_big_fail,    f.c_str());
    if (lbl_kpi_running) lv_label_set_text(lbl_kpi_running, r.c_str());
    if (lbl_kpi_ok)      lv_label_set_text(lbl_kpi_ok,      o.c_str());
    if (lbl_kpi_fail)    lv_label_set_text(lbl_kpi_fail,    f.c_str());

    // Grow row cache as needed; reuse existing rows.
    size_t want = s_snap.recent.size();
    while (s_rows.size() < want) {
        RowRefs r{};
        r.row = lv_obj_create(obj_list_holder);
        lv_obj_remove_style_all(r.row);
        lv_obj_set_size(r.row, 308, 42);
        lv_obj_set_style_bg_color(r.row, COL_PANEL, 0);
        lv_obj_set_style_bg_opa(r.row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(r.row, COL_BORDER, 0);
        lv_obj_set_style_border_width(r.row, 1, 0);
        lv_obj_set_style_radius(r.row, 4, 0);
        lv_obj_set_style_pad_all(r.row, 0, 0);
        lv_obj_clear_flag(r.row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(r.row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r.row, onRowClick, LV_EVENT_CLICKED, nullptr);

        // status stripe (left edge, full height)
        r.dot = lv_obj_create(r.row);
        lv_obj_remove_style_all(r.dot);
        lv_obj_set_size(r.dot, 4, 42);
        lv_obj_align(r.dot, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_set_style_radius(r.dot, 0, 0);
        lv_obj_set_style_bg_opa(r.dot, LV_OPA_COVER, 0);

        r.title = lv_label_create(r.row);
        lv_obj_set_style_text_color(r.title, COL_TEXT, 0);
        lv_obj_set_style_text_font(r.title, &plex_medium_16, 0);
        lv_obj_align(r.title, LV_ALIGN_TOP_LEFT, 12, 4);
        lv_label_set_long_mode(r.title, LV_LABEL_LONG_DOT);
        lv_obj_set_width(r.title, 290);

        r.sub = lv_label_create(r.row);
        lv_obj_set_style_text_color(r.sub, COL_MUTED, 0);
        lv_obj_set_style_text_font(r.sub, &plex_medium_12, 0);
        lv_obj_align(r.sub, LV_ALIGN_BOTTOM_LEFT, 12, -2);
        lv_label_set_long_mode(r.sub, LV_LABEL_LONG_DOT);
        lv_obj_set_width(r.sub, 290);
        lv_obj_set_style_pad_top(r.title, 0, 0);
        lv_obj_set_style_pad_bottom(r.title, 2, 0);

        s_rows.push_back(r);
    }

    // Fill rows; hide unused.
    for (size_t i = 0; i < s_rows.size(); ++i) {
        if (i < want) {
            const auto& ex = s_snap.recent[i];
            lv_obj_clear_flag(s_rows[i].row, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(s_rows[i].dot, colorFromHex(rundeck::statusColor(ex.status)), 0);
            String title = asciiSafe(ex.jobName.length() ? ex.jobName : String("adhoc"));
            lv_label_set_text(s_rows[i].title, title.c_str());
            String sub;
            if (ex.status == rundeck::Status::Running) {
                sub = "RUN  " + shortAge(ex.startedEpoch);
            } else {
                sub = String(rundeck::statusText(ex.status)) + "  " + shortAge(ex.startedEpoch);
            }
            lv_label_set_text(s_rows[i].sub, sub.c_str());
            // Colour the status portion by execution state.
            lv_color_t subColor = COL_MUTED;
            switch (ex.status) {
                case rundeck::Status::Succeeded: subColor = COL_GREEN; break;
                case rundeck::Status::Running:   subColor = COL_BLUE;  break;
                case rundeck::Status::Failed:
                case rundeck::Status::TimedOut:
                case rundeck::Status::FailedWithRetry: subColor = COL_RED; break;
                default: break;
            }
            lv_obj_set_style_text_color(s_rows[i].sub, subColor, 0);
            s_rows[i].execId = ex.id;
            lv_obj_set_user_data(s_rows[i].row, (void*)(intptr_t)ex.id);
        } else {
            lv_obj_add_flag(s_rows[i].row, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (lbl_footer) {
        String m = s_lastError.length()
                   ? (String("! ") + s_lastError)
                   : (String("Rundeck ") + (s_version.length() ? s_version : String("?")));
        lv_label_set_text(lbl_footer, m.c_str());
    }
}

// ---- ROI pane render -----------------------------------------------------

static void renderRoiPane() {
    if (!lbl_roi_runs) return;

    int total = (int)s_snap.recent.size();
    int ok    = (int)s_snap.succeededRecent;
    int fail  = (int)s_snap.failedRecent;
    int finished = ok + fail;   // exclude still-running from rate denom

    // Time saved = total runs * ROI_MINS_PER_RUN
    long mins = (long)total * ROI_MINS_PER_RUN;
    String timeStr;
    if (mins >= 60) timeStr = String(mins / 60) + "h " + String(mins % 60) + "m";
    else            timeStr = String(mins) + "m";
    lv_label_set_text(lbl_roi_time, timeStr.c_str());

    lv_label_set_text(lbl_roi_runs, String(total).c_str());

    int rate = finished > 0 ? (ok * 100) / finished : 0;
    String successStr = String(rate) + "%";
    lv_label_set_text(lbl_roi_success, successStr.c_str());
    lv_obj_set_style_text_color(lbl_roi_success,
        rate >= 90 ? COL_GREEN : (rate >= 70 ? COL_AMBER : COL_RED), 0);

    // USD saved: mins * (rate/hour) / 60
    int dollars = (int)((float)mins * ROI_RATE_USD_HR / 60.0f);
    lv_label_set_text(lbl_roi_dollars, (String("$") + dollars).c_str());

    // Average duration (over finished executions with known duration)
    long totalDur = 0; int n = 0;
    for (auto& ex : s_snap.recent) {
        if (ex.durationSec > 0) { totalDur += ex.durationSec; n++; }
    }
    if (n == 0) {
        lv_label_set_text(lbl_roi_avg, "-");
    } else {
        long avg = totalDur / n;
        String avgStr;
        if (avg >= 3600)    avgStr = String(avg / 3600) + "h " + String((avg % 3600) / 60) + "m";
        else if (avg >= 60) avgStr = String(avg / 60) + "m " + String(avg % 60) + "s";
        else                avgStr = String(avg) + "s";
        lv_label_set_text(lbl_roi_avg, avgStr.c_str());
    }

    // Success bar width — 308 - 2*border = ~304 available inside
    if (bar_roi_success) {
        int barW = (rate * 306) / 100;
        if (barW < 0) barW = 0; if (barW > 306) barW = 306;
        lv_obj_set_width(bar_roi_success, barW);
        lv_obj_set_style_bg_color(bar_roi_success,
            rate >= 90 ? COL_GREEN : (rate >= 70 ? COL_AMBER : COL_RED), 0);
    }
}

// ---- Users pane render ---------------------------------------------------

static void renderUsersPane() {
    if (!s_userRows[0].row) return;

    // Tally counts.
    struct Entry { String name; int count; };
    std::vector<Entry> users;
    users.reserve(8);
    for (auto& ex : s_snap.recent) {
        String u = asciiSafe(ex.user.length() ? ex.user : String("(unknown)"));
        bool found = false;
        for (auto& e : users) { if (e.name == u) { e.count++; found = true; break; } }
        if (!found) users.push_back({u, 1});
    }
    // Sort by count desc
    std::sort(users.begin(), users.end(),
              [](const Entry& a, const Entry& b) { return a.count > b.count; });

    int maxCount = users.empty() ? 1 : users[0].count;
    if (maxCount < 1) maxCount = 1;

    for (int i = 0; i < 5; i++) {
        UserRow& u = s_userRows[i];
        if (i < (int)users.size()) {
            lv_obj_clear_flag(u.row, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(u.name, users[i].name.c_str());
            lv_label_set_text(u.count, String(users[i].count).c_str());
            int barW = (users[i].count * 150) / maxCount;
            if (barW < 2) barW = 2;
            lv_obj_set_width(u.bar, barW);
            // Top runner gets blue; lower runners get progressively dimmer.
            lv_color_t c = (i == 0) ? COL_BLUE
                          : (i == 1) ? lv_color_hex(0x3a72b3)
                          : (i == 2) ? lv_color_hex(0x2d5688)
                          : lv_color_hex(0x244668);
            lv_obj_set_style_bg_color(u.bar, c, 0);
        } else {
            lv_obj_add_flag(u.row, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void renderCurrentPane() {
    renderExecPane();
    renderRoiPane();
    renderUsersPane();
}

// ---- detail ---------------------------------------------------------------

static void buildDetail() {
    if (scr_detail) return;
    scr_detail = lv_obj_create(NULL);
    setBgDark(scr_detail);
    lv_obj_set_style_pad_all(scr_detail, 0, 0);
    lv_obj_clear_flag(scr_detail, LV_OBJ_FLAG_SCROLLABLE);

    // Topbar (back arrow + title)
    lv_obj_t* bar = lv_obj_create(scr_detail);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 320, 22);
    lv_obj_set_style_bg_color(bar, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(bar, COL_BORDER, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* back = lv_btn_create(bar);
    lv_obj_set_size(back, 56, 18);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_set_style_bg_color(back, COL_BORDER, 0);
    lv_obj_set_style_radius(back, 4, 0);
    lv_obj_add_event_cb(back, onBackClick, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* bl = lv_label_create(back);
    lv_label_set_text(bl, "< BACK");
    lv_obj_set_style_text_color(bl, COL_TEXT, 0);
    lv_obj_set_style_text_font(bl, &plex_medium_12, 0);
    lv_obj_center(bl);

    lv_obj_t* head = lv_label_create(bar);
    lv_label_set_text(head, "EXECUTION");
    lv_obj_set_style_text_color(head, COL_BLUE, 0);
    lv_obj_set_style_text_font(head, &plex_medium_14, 0);
    lv_obj_align(head, LV_ALIGN_RIGHT_MID, -8, 0);

    // Body panel
    lv_obj_t* p = lv_obj_create(scr_detail);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, 320, 218);
    lv_obj_set_pos(p, 0, 22);
    lv_obj_set_style_bg_color(p, COL_BG, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(p, 10, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);

    lbl_detail_title = lv_label_create(p);
    lv_obj_set_style_text_color(lbl_detail_title, COL_TEXT, 0);
    lv_obj_set_style_text_font(lbl_detail_title, &plex_medium_18, 0);
    lv_label_set_long_mode(lbl_detail_title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl_detail_title, 300);
    lv_obj_align(lbl_detail_title, LV_ALIGN_TOP_LEFT, 0, 0);

    lbl_detail_status = lv_label_create(p);
    lv_obj_set_style_text_font(lbl_detail_status, &plex_medium_16, 0);
    lv_obj_align(lbl_detail_status, LV_ALIGN_TOP_LEFT, 0, 28);

    lbl_detail_meta = lv_label_create(p);
    lv_obj_set_style_text_color(lbl_detail_meta, COL_MUTED, 0);
    lv_obj_set_style_text_font(lbl_detail_meta, &plex_medium_12, 0);
    lv_label_set_long_mode(lbl_detail_meta, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl_detail_meta, 300);
    lv_obj_align(lbl_detail_meta, LV_ALIGN_TOP_LEFT, 0, 56);

    btn_abort = lv_btn_create(p);
    lv_obj_set_size(btn_abort, 130, 36);
    lv_obj_align(btn_abort, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(btn_abort, COL_RED, 0);
    lv_obj_set_style_radius(btn_abort, 6, 0);
    lv_obj_add_event_cb(btn_abort, onAbortClick, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* abl = lv_label_create(btn_abort);
    lv_label_set_text(abl, "ABORT");
    lv_obj_set_style_text_color(abl, lv_color_white(), 0);
    lv_obj_set_style_text_font(abl, &plex_medium_14, 0);
    lv_obj_center(abl);

    lbl_detail_result = lv_label_create(p);
    lv_label_set_text(lbl_detail_result, "");
    lv_obj_set_style_text_font(lbl_detail_result, &plex_medium_12, 0);
    lv_obj_align(lbl_detail_result, LV_ALIGN_BOTTOM_RIGHT, 0, -6);
}

static void renderDetail() {
    const rundeck::Execution* ex = findExecById(s_detail_exec_id);
    if (!ex) {
        lv_label_set_text(lbl_detail_title, "(execution gone)");
        lv_label_set_text(lbl_detail_status, "");
        lv_label_set_text(lbl_detail_meta, "");
        lv_obj_add_flag(btn_abort, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    String title = asciiSafe(ex->jobName.length() ? ex->jobName : String("adhoc"));
    lv_label_set_text_fmt(lbl_detail_title, "#%ld  %s", ex->id, title.c_str());

    String stxt = rundeck::statusText(ex->status);
    lv_label_set_text(lbl_detail_status, stxt.c_str());
    lv_obj_set_style_text_color(lbl_detail_status,
        lv_color_hex(rundeck::statusColor(ex->status)), 0);

    String meta;
    meta += "project: " + asciiSafe(ex->project.length() ? ex->project : String("?")) + "\n";
    meta += "user: "    + asciiSafe(ex->user.length()    ? ex->user    : String("?")) + "\n";
    meta += "started: " + shortAge(ex->startedEpoch) + " ago";
    if (ex->durationSec >= 0) {
        meta += String("    duration: ");
        if (ex->durationSec >= 3600) meta += String(ex->durationSec / 3600) + "h " +
                                            String((ex->durationSec % 3600) / 60) + "m";
        else if (ex->durationSec >= 60) meta += String(ex->durationSec / 60) + "m " +
                                                String(ex->durationSec % 60) + "s";
        else meta += String(ex->durationSec) + "s";
    }
    lv_label_set_text(lbl_detail_meta, meta.c_str());

    if (ex->status == rundeck::Status::Running) {
        lv_obj_clear_flag(btn_abort, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(btn_abort, LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text(lbl_detail_result, "");
}

void ui::showDetail(long execId) {
    buildDetail();
    s_detail_exec_id = execId;
    renderDetail();
    lv_scr_load_anim(scr_detail, LV_SCR_LOAD_ANIM_OVER_LEFT, 180, 0, false);
}

// ---- public data feed -----------------------------------------------------

void ui::begin() {}

void ui::setSnapshot(const rundeck::Snapshot& s) {
    s_snap = s;
    s_haveSnap = true;
    s_lastError = s.ok ? String("") : s.error;
    if (scr_dashboard && lv_scr_act() == scr_dashboard) {
        renderCurrentPane();
    } else if (scr_detail && lv_scr_act() == scr_detail) {
        renderDetail();
    }
}

void ui::setError(const String& msg) {
    s_lastError = msg;
    if (lbl_footer) {
        String m = String("! ") + msg;
        lv_label_set_text(lbl_footer, m.c_str());
    }
}

void ui::setServerInfo(const String& baseUrl, const String& version, const String& project) {
    s_baseUrl = baseUrl;
    s_version = version;
    s_project = project;
    if (lbl_topbar_proj) {
        lv_label_set_text(lbl_topbar_proj, project.length() ? project.c_str() : "all projects");
    }
}

void ui::invalidate() {
    if (scr_dashboard) lv_obj_invalidate(scr_dashboard);
    if (scr_detail)    lv_obj_invalidate(scr_detail);
}

void ui::setRefreshCallback(RefreshCb cb) { s_refreshCb = cb; }

void ui::tick() {
    // Auto-rotate the EXEC subview every 20s, only when the EXEC tab is the
    // active pane on the dashboard.
    if (!scr_dashboard || lv_scr_act() != scr_dashboard) return;
    if (s_activePane != 0) return;
    if (!exec_view_kpi || !exec_view_list) return;
    uint32_t now = millis();
    if ((int32_t)(now - s_execRotateAt) >= 0) {
        setExecSubview(s_execSubview == 0 ? 1 : 0);
    }
}

void ui::releaseTransientScreens() {
    // Only safe to delete a screen that is not currently active.
    lv_obj_t* act = lv_scr_act();
    auto kill = [&](lv_obj_t*& s) {
        if (s && s != act) { lv_obj_del(s); s = nullptr; }
    };
    kill(scr_splash);
    kill(scr_wifi);
    kill(scr_setup);
    if (scr_splash == nullptr) lbl_splash_status = nullptr;
}

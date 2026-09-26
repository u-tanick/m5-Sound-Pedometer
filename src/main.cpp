// SPDX-FileCopyrightText: 2026 M5Stack Core2 Step Sound Counter
// SPDX-License-Identifier: MIT

#include <M5Unified.h>
#include <SPIFFS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <cmath>
#include <vector>

// ==========================================
// テーマ定義 (JSONから動的ロード)
// ==========================================

struct CharacterTheme {
    String id_name;
    String display_name;
    String image_path;
    String wav_path;
    uint16_t theme_color{0xFFFF};
};

static std::vector<CharacterTheme> s_themes;

enum ScreenMode {
    SCREEN_MAIN,
    SCREEN_LOG
};

// ==========================================
// 音声キャッシュ（ゼロ遅延再生用）
// ==========================================

struct CachedAudio {
    uint8_t* data{nullptr};
    size_t size{0};
};

static std::vector<CachedAudio> s_audio_cache;

// ==========================================
// グローバル状態
// ==========================================

static Preferences s_prefs;
static ScreenMode s_screen_mode = SCREEN_MAIN;

static int s_current_theme = 0;   // デフォルトは 0（無音・タイトル表示）
static bool s_is_running = false; // デフォルトはカウント停止（BtnAで開始）
static uint8_t s_volume = 200;    // デフォルト音量 (0〜255)

static uint32_t s_total_steps = 0;
static std::vector<uint32_t> s_theme_steps;

static uint32_t s_last_save_steps = 0;
static uint32_t s_last_save_time_ms = 0;

// 歩数検出用変数 (IMU MPU6886)
static float s_acc_filtered = 1.0f;
static float s_acc_prev = 1.0f;
static bool s_acc_rising = false;
static uint32_t s_last_step_time_ms = 0;

// タッチスワイプ判定用
static int32_t s_touch_start_x = -1;
static int32_t s_touch_start_y = -1;
static bool s_touch_swiped = false;

// 画面再描画フラグ
static bool s_need_redraw = true;

// ==========================================
// カラーコード変換補助
// ==========================================

uint16_t parse_color_string(const String& str) {
    if (str.isEmpty()) return 0x07FF;
    if (str.startsWith("#")) {
        // "#RRGGBB" 形式
        uint32_t rgb = strtoul(str.substring(1).c_str(), nullptr, 16);
        uint8_t r = (rgb >> 16) & 0xFF;
        uint8_t g = (rgb >> 8) & 0xFF;
        uint8_t b = rgb & 0xFF;
        return M5.Lcd.color565(r, g, b);
    }
    // "0x07FF" 形式など
    return static_cast<uint16_t>(strtoul(str.c_str(), nullptr, 0));
}

// ==========================================
// JSON からテーマ一覧をロード
// ==========================================

void load_themes_json() {
    s_themes.clear();

    if (SPIFFS.exists("/themes.json")) {
        File f = SPIFFS.open("/themes.json", "r");
        if (f) {
            JsonDocument doc;
            DeserializationError err = deserializeJson(doc, f);
            f.close();

            if (!err && doc.is<JsonArray>()) {
                for (JsonObject obj : doc.as<JsonArray>()) {
                    CharacterTheme t;
                    t.id_name = obj["id"] | "Unknown";
                    t.display_name = obj["name"] | t.id_name.c_str();
                    t.image_path = obj["image"] | "";
                    t.wav_path = obj["wav"] | "";

                    if (obj["color"].is<const char*>()) {
                        t.theme_color = parse_color_string(obj["color"].as<const char*>());
                    } else if (obj["color"].is<uint32_t>()) {
                        t.theme_color = obj["color"].as<uint32_t>();
                    } else {
                        t.theme_color = 0x07FF;
                    }

                    s_themes.push_back(t);
                    Serial.printf("Loaded Theme: id=%s, name=%s, wav=%s, color=0x%04X\n",
                                  t.id_name.c_str(), t.display_name.c_str(),
                                  t.wav_path.c_str(), t.theme_color);
                }
            } else {
                Serial.printf("Failed to parse themes.json: %s\n", err.c_str());
            }
        }
    }

    // 万一 themes.json が存在しないか空だった場合の安全フォールバック
    if (s_themes.empty()) {
        CharacterTheme def;
        def.id_name = "Default";
        def.display_name = "STEP COUNTER";
        def.image_path = "";
        def.wav_path = "";
        def.theme_color = 0x07FF;
        s_themes.push_back(def);
        Serial.println("Warning: themes.json not loaded, using fallback default theme.");
    }

    s_audio_cache.resize(s_themes.size());
    s_theme_steps.assign(s_themes.size(), 0);
}

// ==========================================
// 音声ロード＆キャッシュ
// ==========================================

void load_audio_cache() {
    for (size_t i = 0; i < s_themes.size(); ++i) {
        if (s_themes[i].wav_path.isEmpty()) {
            s_audio_cache[i].data = nullptr;
            s_audio_cache[i].size = 0;
            continue;
        }

        if (SPIFFS.exists(s_themes[i].wav_path)) {
            File f = SPIFFS.open(s_themes[i].wav_path, "r");
            if (f) {
                size_t sz = f.size();
                // PSRAMまたは通常ヒープにアロケート
                uint8_t* buf = static_cast<uint8_t*>(heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                if (buf == nullptr) {
                    buf = static_cast<uint8_t*>(malloc(sz));
                }
                if (buf != nullptr) {
                    f.read(buf, sz);
                    s_audio_cache[i].data = buf;
                    s_audio_cache[i].size = sz;
                    Serial.printf("Cached audio %s (%u bytes)\n", s_themes[i].id_name.c_str(), sz);
                }
                f.close();
            }
        }
    }
}

void play_footstep() {
    if (s_current_theme >= 0 && s_current_theme < static_cast<int>(s_themes.size())) {
        if (s_audio_cache[s_current_theme].data != nullptr) {
            // M5UnifiedのSpeakerでWAVメモリ再生（ゼロレイテンシ）
            M5.Speaker.playWav(s_audio_cache[s_current_theme].data, s_audio_cache[s_current_theme].size);
        }
    }
}

void play_beep(int freq = 1200, int dur_ms = 60) {
    M5.Speaker.tone(freq, dur_ms);
}

void play_reset_sound() {
    M5.Speaker.tone(880, 80);
    delay(100);
    M5.Speaker.tone(1174, 80);
    delay(100);
    M5.Speaker.tone(1760, 150);
}

// ==========================================
// NVS データ永続化
// ==========================================

void load_data() {
    s_prefs.begin("step_counter", false);
    s_total_steps = s_prefs.getUInt("total_steps", 0);
    s_current_theme = s_prefs.getInt("theme", 0);
    if (s_current_theme < 0 || s_current_theme >= static_cast<int>(s_themes.size())) s_current_theme = 0;

    s_volume = s_prefs.getUChar("volume", 200);
    M5.Speaker.setVolume(s_volume);

    for (size_t i = 0; i < s_themes.size(); ++i) {
        char key[16];
        snprintf(key, sizeof(key), "step_%d", (int)i);
        s_theme_steps[i] = s_prefs.getUInt(key, 0);
    }
    s_last_save_steps = s_total_steps;
}

void save_data() {
    s_prefs.putUInt("total_steps", s_total_steps);
    s_prefs.putInt("theme", s_current_theme);
    s_prefs.putUChar("volume", s_volume);
    for (size_t i = 0; i < s_themes.size(); ++i) {
        char key[16];
        snprintf(key, sizeof(key), "step_%d", (int)i);
        s_prefs.putUInt(key, s_theme_steps[i]);
    }
    s_last_save_steps = s_total_steps;
    s_last_save_time_ms = millis();
}

void reset_data() {
    s_total_steps = 0;
    for (size_t i = 0; i < s_themes.size(); ++i) {
        s_theme_steps[i] = 0;
    }
    s_prefs.clear();
    save_data();
}

// ==========================================
// 画面描画
// ==========================================

void draw_main_screen() {
    if (s_themes.empty()) {
        M5.Lcd.fillScreen(TFT_BLACK);
        return;
    }

    const auto& theme = s_themes[s_current_theme];
    bool has_image = (!theme.image_path.isEmpty() && SPIFFS.exists(theme.image_path.c_str()));

    Serial.printf("[draw] theme=%d has_image=%d path=%s\n",
                  s_current_theme, has_image, theme.image_path.c_str());

    // ====================================================
    // Phase 1: 背景描画 (PNG は独立トランザクション)
    // drawPng は内部で SPI トランザクションを管理するため
    // 外側の startWrite/endWrite に入れ子にしない
    // ====================================================
    if (has_image) {
        File f = SPIFFS.open(theme.image_path.c_str(), "r");
        if (f) {
            M5.Lcd.drawPng(&f, 0, 0);
            f.close();
        }
    } else {
        M5.Lcd.startWrite();
        // デフォルト画面（無音）: スタイリッシュなダークグラデーション背景
        M5.Lcd.fillScreen(M5.Lcd.color565(12, 16, 24));

        // 装飾幾何学サークル
        M5.Lcd.drawCircle(160, 110, 80, M5.Lcd.color565(25, 35, 55));
        M5.Lcd.drawCircle(160, 110, 82, M5.Lcd.color565(20, 28, 45));
        M5.Lcd.drawCircle(160, 110, 95, M5.Lcd.color565(18, 24, 38));

        // メインタイトル
        M5.Lcd.setTextDatum(MC_DATUM);
        M5.Lcd.setTextColor(TFT_CYAN);
        M5.Lcd.setTextSize(3);
        M5.Lcd.drawString("Sound Pedometer", 160, 80);

        M5.Lcd.setTextColor(TFT_DARKGRAY);
        M5.Lcd.setTextSize(1);
        M5.Lcd.drawString("Swipe left/right to change footstep sound", 160, 122);
        M5.Lcd.drawString("Hold [A] [B] [C] buttons to operate", 160, 142);
        M5.Lcd.endWrite();
    }

    // ====================================================
    // Phase 2: オーバーレイ描画 (新しいトランザクション)
    // ====================================================
    M5.Lcd.startWrite();

    if (has_image) {
        // ── 画像付き画面: 右上に記号のみ (■ / ▶) をすっきり配置 ──
        const int badge_w = 28;
        const int badge_h = 28;
        const int badge_x = 320 - badge_w - 6; // 286
        const int badge_y = 6;

        // 背景バッジ（画像の上でも視認できる薄いグレーの角丸プレート）
        M5.Lcd.fillRoundRect(badge_x, badge_y, badge_w, badge_h, 6, M5.Lcd.color565(85, 92, 105));
        M5.Lcd.drawRoundRect(badge_x, badge_y, badge_w, badge_h, 6, M5.Lcd.color565(135, 145, 160));

        // 記号 (■: STOP / ▶: RUN)
        if (s_is_running) {
            // RUN: ▶ (緑色)
            M5.Lcd.fillTriangle(badge_x + 9, badge_y + 7,
                                badge_x + 9, badge_y + badge_h - 7,
                                badge_x + badge_w - 7, badge_y + badge_h / 2,
                                TFT_GREEN);
        } else {
            // STOP: ■ (オレンジ色)
            const int sq_size = 12;
            const int sq_x = badge_x + (badge_w - sq_size) / 2;
            const int sq_y = badge_y + (badge_h - sq_size) / 2;
            M5.Lcd.fillRoundRect(sq_x, sq_y, sq_size, sq_size, 2, TFT_ORANGE);
        }
    } else {
        // ── MUTE 画面: フル UI ──

        // 上部ヘッダーバー
        M5.Lcd.fillRect(0, 0, 320, 34, M5.Lcd.color565(18, 20, 26));
        M5.Lcd.drawFastHLine(0, 34, 320, theme.theme_color);

        // バッテリー残量 (左肩)
        int bat = M5.Power.getBatteryLevel();
        M5.Lcd.setTextDatum(ML_DATUM);
        M5.Lcd.setTextColor(bat < 20 ? TFT_RED : (bat < 50 ? TFT_YELLOW : TFT_GREEN));
        M5.Lcd.setTextSize(1);
        char bat_str[16];
        snprintf(bat_str, sizeof(bat_str), "BAT %d%%", bat);
        M5.Lcd.drawString(bat_str, 12, 17);

        // RUN/STOP ステータス (右肩)
        {
            uint16_t status_color = s_is_running ? static_cast<uint16_t>(TFT_GREEN) : static_cast<uint16_t>(TFT_ORANGE);
            const char* status_text = s_is_running ? "RUN" : "STOP";
            M5.Lcd.setTextSize(1);
            int tw = M5.Lcd.textWidth(status_text);
            int icon_x = 308 - tw - 10;
            int icon_y = 17;

            if (s_is_running) {
                // RUN: 緑の丸
                M5.Lcd.fillCircle(icon_x, icon_y, 4, TFT_GREEN);
            } else {
                // STOP: オレンジの四角
                M5.Lcd.fillRect(icon_x - 4, icon_y - 4, 8, 8, TFT_ORANGE);
            }
            M5.Lcd.setTextDatum(MR_DATUM);
            M5.Lcd.setTextColor(status_color);
            M5.Lcd.drawString(status_text, 308, icon_y);
        }

        // インジケータードット
        int theme_count = static_cast<int>(s_themes.size());
        int spacing = (theme_count > 8) ? 8 : 12;
        int start_x = 160 - (theme_count * spacing) / 2;
        for (int i = 0; i < theme_count; ++i) {
            int x = start_x + i * spacing;
            int y = 17;
            if (i == s_current_theme) {
                M5.Lcd.fillCircle(x, y, 4, theme.theme_color);
            } else {
                M5.Lcd.drawCircle(x, y, 3, TFT_DARKGRAY);
            }
        }

        // 下部カウンターバー
        M5.Lcd.fillRect(0, 174, 320, 66, M5.Lcd.color565(14, 16, 22));
        M5.Lcd.drawFastHLine(0, 174, 320, M5.Lcd.color565(45, 50, 65));

        // 歩数表示
        M5.Lcd.setTextDatum(ML_DATUM);
        M5.Lcd.setTextColor(TFT_WHITE);
        M5.Lcd.setTextSize(4);
        char step_str[32];
        snprintf(step_str, sizeof(step_str), "%lu", s_total_steps);
        M5.Lcd.drawString(step_str, 16, 206);

        int step_width = M5.Lcd.textWidth(step_str);
        M5.Lcd.setTextSize(2);
        M5.Lcd.setTextColor(TFT_LIGHTGRAY);
        M5.Lcd.drawString("Steps", 22 + step_width, 210);

        // 推定距離
        float dist_km = (s_total_steps * 0.65f) / 1000.0f;
        char dist_str[32];
        if (dist_km < 1.0f) {
            snprintf(dist_str, sizeof(dist_str), "%d m", static_cast<int>(s_total_steps * 0.65f));
        } else {
            snprintf(dist_str, sizeof(dist_str), "%.2f km", dist_km);
        }
        M5.Lcd.setTextDatum(MR_DATUM);
        M5.Lcd.setTextSize(2);
        M5.Lcd.setTextColor(M5.Lcd.color565(170, 195, 220));
        M5.Lcd.drawString(dist_str, 305, 206);

        // ボタンヒント
        {
            M5.Lcd.setTextSize(1);
            M5.Lcd.setTextColor(M5.Lcd.color565(110, 120, 140));
            M5.Lcd.setTextDatum(MC_DATUM);
            M5.Lcd.drawString("[A] Start/Stop", 53, 232);
            M5.Lcd.drawString("[B] Log", 160, 232);
            M5.Lcd.drawString("[C] Reset", 267, 232);
        }
    }

    Serial.printf("[draw] overlay done (has_image=%d)\n", has_image);

    M5.Lcd.endWrite();
}

void draw_log_screen() {
    M5.Lcd.startWrite();
    M5.Lcd.fillScreen(M5.Lcd.color565(16, 18, 26));

    // ヘッダー
    M5.Lcd.fillRect(0, 0, 320, 30, M5.Lcd.color565(26, 30, 42));
    M5.Lcd.drawFastHLine(0, 30, 320, TFT_CYAN);
    M5.Lcd.setTextDatum(MC_DATUM);
    M5.Lcd.setTextColor(TFT_WHITE);
    M5.Lcd.setTextSize(2);
    M5.Lcd.drawString("ACTIVITY & SETTINGS", 160, 15);

    // 1. トータル集計カード (コンパクト化)
    M5.Lcd.fillRoundRect(8, 34, 304, 38, 5, M5.Lcd.color565(30, 36, 52));
    M5.Lcd.drawRoundRect(8, 34, 304, 38, 5, TFT_DARKCYAN);

    M5.Lcd.setTextDatum(ML_DATUM);
    M5.Lcd.setTextColor(TFT_LIGHTGRAY);
    M5.Lcd.setTextSize(1);
    M5.Lcd.drawString("TOTAL STEPS:", 16, 53);

    M5.Lcd.setTextColor(TFT_YELLOW);
    M5.Lcd.setTextSize(2);
    char buf[32];
    snprintf(buf, sizeof(buf), "%lu", s_total_steps);
    M5.Lcd.drawString(buf, 92, 53);

    float dist_km = (s_total_steps * 0.65f) / 1000.0f;
    float kcal = s_total_steps * 0.035f;
    M5.Lcd.setTextDatum(MR_DATUM);
    M5.Lcd.setTextSize(1);
    M5.Lcd.setTextColor(TFT_CYAN);
    snprintf(buf, sizeof(buf), "%.2f km", dist_km);
    M5.Lcd.drawString(buf, 230, 53);

    M5.Lcd.setTextColor(TFT_ORANGE);
    snprintf(buf, sizeof(buf), "%.0f kcal", kcal);
    M5.Lcd.drawString(buf, 302, 53);

    // 2. キャラクター別内訳リスト (6キャラ分コンパクト表示)
    M5.Lcd.setTextDatum(ML_DATUM);
    M5.Lcd.setTextColor(TFT_LIGHTGRAY);
    M5.Lcd.setTextSize(1);
    M5.Lcd.drawString("FOOTSTEP BREAKDOWN", 12, 80);

    size_t display_count = std::min(s_themes.size(), (size_t)6);
    for (size_t i = 0; i < display_count; ++i) {
        int y = 92 + i * 11;
        M5.Lcd.fillCircle(18, y + 4, 3, s_themes[i].theme_color);
        M5.Lcd.setTextDatum(ML_DATUM);
        M5.Lcd.setTextColor(TFT_WHITE);
        M5.Lcd.setTextSize(1);
        M5.Lcd.drawString(s_themes[i].display_name.c_str(), 26, y + 4);

        // バーグラフ表示
        float ratio = (s_total_steps > 0 && i < s_theme_steps.size()) ? (float)s_theme_steps[i] / s_total_steps : 0.0f;
        int bar_w = static_cast<int>(ratio * 105.0f);
        M5.Lcd.fillRect(110, y + 1, 105, 6, M5.Lcd.color565(35, 40, 55));
        if (bar_w > 0) {
            M5.Lcd.fillRect(110, y + 1, bar_w, 6, s_themes[i].theme_color);
        }

        // 歩数と割合
        M5.Lcd.setTextDatum(MR_DATUM);
        uint32_t count = (i < s_theme_steps.size()) ? s_theme_steps[i] : 0;
        snprintf(buf, sizeof(buf), "%lu (%d%%)", count, static_cast<int>(ratio * 100));
        M5.Lcd.drawString(buf, 306, y + 4);
    }

    // 3. 音量調整スライダーカード (案2)
    M5.Lcd.fillRoundRect(8, 164, 304, 52, 6, M5.Lcd.color565(24, 28, 40));
    M5.Lcd.drawRoundRect(8, 164, 304, 52, 6, M5.Lcd.color565(50, 60, 85));

    int vol_pct = (static_cast<int>(s_volume) * 100 + 127) / 255;
    M5.Lcd.setTextDatum(MC_DATUM);
    M5.Lcd.setTextColor(TFT_CYAN);
    M5.Lcd.setTextSize(1);
    snprintf(buf, sizeof(buf), "VOLUME: %d%%", vol_pct);
    M5.Lcd.drawString(buf, 160, 175);

    // [-] ボタン
    M5.Lcd.fillRoundRect(16, 185, 34, 24, 4, M5.Lcd.color565(40, 48, 68));
    M5.Lcd.drawRoundRect(16, 185, 34, 24, 4, TFT_DARKCYAN);
    M5.Lcd.setTextColor(TFT_WHITE);
    M5.Lcd.setTextSize(2);
    M5.Lcd.drawString("-", 33, 196);

    // [+] ボタン
    M5.Lcd.fillRoundRect(270, 185, 34, 24, 4, M5.Lcd.color565(40, 48, 68));
    M5.Lcd.drawRoundRect(270, 185, 34, 24, 4, TFT_DARKCYAN);
    M5.Lcd.setTextColor(TFT_WHITE);
    M5.Lcd.setTextSize(2);
    M5.Lcd.drawString("+", 287, 196);

    // スライダートラック (X: 60〜260, 幅 200px)
    M5.Lcd.fillRoundRect(60, 194, 200, 6, 3, M5.Lcd.color565(35, 42, 58));
    int knob_x = 60 + (vol_pct * 200) / 100;
    if (knob_x > 60) {
        M5.Lcd.fillRoundRect(60, 194, knob_x - 60, 6, 3, TFT_CYAN);
    }
    M5.Lcd.fillCircle(knob_x, 197, 7, TFT_WHITE);
    M5.Lcd.drawCircle(knob_x, 197, 7, TFT_DARKCYAN);

    // 4. フッター操作案内
    M5.Lcd.fillRect(0, 222, 320, 18, M5.Lcd.color565(12, 14, 20));
    M5.Lcd.setTextDatum(MC_DATUM);
    M5.Lcd.setTextColor(TFT_DARKGRAY);
    M5.Lcd.setTextSize(1);
    M5.Lcd.drawString("[B] Back to Main   |   [C] Reset All Steps", 160, 231);

    M5.Lcd.endWrite();
}

// ==========================================
// 歩数検知アルゴリズム (IMU MPU6886)
// ==========================================

void update_pedometer() {
    float ax = 0, ay = 0, az = 0;
    if (!M5.Imu.getAccel(&ax, &ay, &az)) {
        return;
    }

    // 3軸合成加速度 (ノルム)
    float a_norm = std::sqrt(ax * ax + ay * ay + az * az);

    // ローパスフィルタ (平滑化して高周波振動・ノイズをカット)
    s_acc_filtered = s_acc_filtered * 0.65f + a_norm * 0.35f;

    const uint32_t now_ms = millis();

    // ピーク判定 (上りから下りに転じた点)
    if (s_acc_filtered > s_acc_prev) {
        s_acc_rising = true;
    } else if (s_acc_rising) {
        // 下降に転じた瞬間
        s_acc_rising = false;

        // ピーク値が閾値を超えており、かつ前回のステップから最低280ms経過しているか
        constexpr float kStepThresholdG = 1.25f;
        constexpr uint32_t kMinStepIntervalMs = 280;

        if (s_acc_prev >= kStepThresholdG && (now_ms - s_last_step_time_ms >= kMinStepIntervalMs)) {
            s_last_step_time_ms = now_ms;

            if (s_is_running) {
                s_total_steps++;
                if (s_current_theme >= 0 && s_current_theme < static_cast<int>(s_theme_steps.size())) {
                    s_theme_steps[s_current_theme]++;
                }

                // 足音再生（無音モード以外ならWAV即時再生）
                play_footstep();

                // 画面更新フラグ
                s_need_redraw = true;

                // 20歩ごと、または定期的にNVSへセーブ
                if (s_total_steps - s_last_save_steps >= 20 || (now_ms - s_last_save_time_ms >= 10000)) {
                    save_data();
                }
            }
        }
    }

    s_acc_prev = s_acc_filtered;
}

// ==========================================
// タッチ＆ボタン処理
// ==========================================

void handle_inputs() {
    auto touch = M5.Touch.getDetail();

    if (s_screen_mode == SCREEN_LOG) {
        // --- ログ＆設定画面: 音量スライダー操作 ---
        if (touch.wasClicked()) {
            if (touch.x >= 12 && touch.x <= 54 && touch.y >= 160 && touch.y <= 220) {
                // [-] ボタン: 音量 10% ダウン
                s_volume = (s_volume > 25) ? s_volume - 25 : 0;
                M5.Speaker.setVolume(s_volume);
                play_beep(900, 50);
                save_data();
                s_need_redraw = true;
            } else if (touch.x >= 266 && touch.x <= 308 && touch.y >= 160 && touch.y <= 220) {
                // [+] ボタン: 音量 10% アップ
                s_volume = (s_volume <= 230) ? s_volume + 25 : 255;
                M5.Speaker.setVolume(s_volume);
                play_beep(1400, 50);
                save_data();
                s_need_redraw = true;
            }
        } else if (touch.isPressed() && touch.x >= 54 && touch.x <= 266 && touch.y >= 160 && touch.y <= 220) {
            // スライダートラック (タッチ＆ドラッグ)
            int pct = constrain((touch.x - 60) * 100 / 200, 0, 100);
            uint8_t new_vol = static_cast<uint8_t>(pct * 255 / 100);
            if (new_vol != s_volume) {
                s_volume = new_vol;
                M5.Speaker.setVolume(s_volume);
                s_need_redraw = true;
            }
        }

        if (touch.wasReleased() && touch.x >= 54 && touch.x <= 266 && touch.y >= 160 && touch.y <= 220) {
            // スライダーから指を離した時に確認音を鳴らしてNVS保存
            play_beep(1200, 60);
            save_data();
            s_need_redraw = true;
        }
    } else {
        // --- メイン画面: 左右スワイプ操作 ---
        if (touch.wasPressed()) {
            s_touch_start_x = touch.x;
            s_touch_start_y = touch.y;
            s_touch_swiped = false;
        } else if (touch.isPressed() && !s_touch_swiped && s_touch_start_x >= 0) {
            int32_t dx = touch.x - s_touch_start_x;
            int32_t dy = touch.y - s_touch_start_y;

            // 水平方向へのスワイプ判定 (距離45px以上、かつ垂直移動が少ない)
            if (std::abs(dx) >= 45 && std::abs(dy) < 50) {
                s_touch_swiped = true;

                int theme_count = static_cast<int>(s_themes.size());
                if (theme_count > 0) {
                    if (dx < 0) {
                        // 左スワイプ: 次のキャラクターへ
                        s_current_theme = (s_current_theme + 1) % theme_count;
                    } else {
                        // 右スワイプ: 前のキャラクターへ
                        s_current_theme = (s_current_theme - 1 + theme_count) % theme_count;
                    }

                    play_footstep(); // 切り替え時にどんな音か1回サンプル再生！
                    save_data();
                    s_need_redraw = true;
                }
            }
        } else if (touch.wasReleased()) {
            s_touch_start_x = -1;
            s_touch_start_y = -1;
            s_touch_swiped = false;
        }
    }

    // 2. ボタン操作 (Core2 画面下部 A, B, C ボタン - すべて長押しで誤動作防止)
    // Button A: 長押しでカウント開始 / 停止 (約800ms)
    {
        static bool s_btnA_triggered = false;
        if (M5.BtnA.pressedFor(800)) {
            if (!s_btnA_triggered) {
                s_btnA_triggered = true;
                s_is_running = !s_is_running;
                play_beep(s_is_running ? 1500 : 750, 80);
                save_data();
                s_need_redraw = true;
            }
        } else if (M5.BtnA.wasReleased()) {
            s_btnA_triggered = false;
        }
    }

    // Button B: 長押しでログ画面表示 / メイン画面へ戻る (約800ms)
    {
        static bool s_btnB_triggered = false;
        if (M5.BtnB.pressedFor(800)) {
            if (!s_btnB_triggered) {
                s_btnB_triggered = true;
                s_screen_mode = (s_screen_mode == SCREEN_MAIN) ? SCREEN_LOG : SCREEN_MAIN;
                play_beep(1200, 50);
                s_need_redraw = true;
            }
        } else if (M5.BtnB.wasReleased()) {
            s_btnB_triggered = false;
        }
    }

    // Button C: 長押しでカウントリセット (約1500ms、どの画面でも常に有効)
    {
        static bool s_reset_triggered = false;
        if (M5.BtnC.pressedFor(1500)) {
            if (!s_reset_triggered) {
                s_reset_triggered = true;
                reset_data();
                play_reset_sound();
                s_need_redraw = true;
            }
        } else if (M5.BtnC.wasReleased()) {
            s_reset_triggered = false;
        }
    }
}

// ==========================================
// メインセットアップ & ループ
// ==========================================

void setup() {
    auto cfg = M5.config();
    cfg.internal_spk = true;
    cfg.internal_mic = false;
    cfg.internal_imu = true;
    M5.begin(cfg);

    Serial.begin(115200);
    Serial.println("M5Stack Core2 Step Sound Counter starting...");

    // スピーカー初期化＆音量設定 (腰装着でもしっかり聞こえるよう大きめに設定: 200/255)
    M5.Speaker.begin();
    M5.Speaker.setVolume(200);

    // SPIFFSマウント
    if (!SPIFFS.begin(true)) {
        Serial.println("SPIFFS Mount Failed!");
    } else {
        Serial.println("SPIFFS Mounted Successfully.");
    }

    // themes.json からテーマ定義を動的ロード
    load_themes_json();

    // 音声データをPSRAM/メモリにキャッシュ
    load_audio_cache();

    // NVSから保存データ復元
    load_data();

    // 起動音
    play_beep(1046, 100);

    // 画面初期描画
    draw_main_screen();
}

void loop() {
    M5.update();

    // 入力処理（スワイプ、ボタン）
    handle_inputs();

    // 歩数検出
    update_pedometer();

    // 画面再描画
    if (s_need_redraw) {
        s_need_redraw = false;
        if (s_screen_mode == SCREEN_MAIN) {
            draw_main_screen();
        } else {
            draw_log_screen();
        }
    }

    delay(10); // 約100Hzループ
}

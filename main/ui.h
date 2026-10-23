// ui.h — 屏幕与触摸（v0.3：主屏 Friday 动态图标 + 右滑音量页 + 左滑会话列表页，机身三键）
//
// 机身键：A（左）按住说话松开结束；B（中）把识别结果发给当前目标；C（右）弃用，不响应。
// 页内返回：反向滑、点左上「返回」，或放着不管自动回主屏。
#pragma once
#include "app.h"
#include "work.h"

void ui_init(void);
void ui_draw(const char *qr_payload);   // qr_payload 非空时画配对二维码页（白底大码）
void ui_qr_hint(bool scanned, int remain_ms);   // 二维码页底部那行（轮询里调，不动码）
void ui_portal(int clients, uint32_t remain_ms);  // SoftAP 配网页（配网循环里调）

// bot 选择页：列出名字（最多 5 个），等用户点；ui_pick_result 返回下标（未选=-1）
void ui_pick_bot(char (*names)[48], int n);
int ui_pick_result(void);
void ui_pick_reset(void);   // 选完后复位，恢复主界面刷新
void ui_tick(void);                     // 主循环里调：触摸/按键采样 + 屏幕刷新

// 发送目标（会话列表页选的；app_main 发送时读）
void ui_get_target(bool *is_assistant, char *conv_id, size_t idcap);
const char *ui_target_label(void);      // "助手" 或会话标题（已截断）

// ui 触发的动作回调（app_main 设）
typedef struct {
  void (*on_hold)(bool down);          // 键 A（左）：按住说话 / 松开结束（草稿留着）
  void (*on_send)(void);               // 键 B（中）：把草稿发给当前目标
  void (*on_stop)(void);               // 点屏幕中间图标：在播=打断播报，有草稿=丢弃草稿
  void (*on_update)(void);             // 点顶部状态栏：检查/安装 OTA 更新
  void (*on_volume_set)(int v);        // 音量页：设置音量 0..255
  void (*on_sessions_open)(void);      // 会话列表页打开：订阅 activity
  void (*on_sessions_close)(void);     // 会话列表页关闭：退订
} ui_callbacks_t;
void ui_set_callbacks(const ui_callbacks_t *cb);

#ifdef UI_DUMP
// 调试构建（编译时设了环境变量 UI_DUMP=1）：把各页面渲染到 PSRAM 画布，
// 由设备自己起个 HTTP 服务（端口 8098）让电脑来拉，见 tools/ui-dump-fetch.mjs。
extern "C" void ui_dump_all(void);
extern "C" void ui_dump_serve(void);
#endif

//----------------------------------------------------------------------------------
//	EditHistory for AviUtl ExEdit2
//	編集履歴を一覧表示し、項目をクリックするとその時点まで本体の「元に戻す」「やり直し」で戻すプラグイン
//
//	SDKにはUndo履歴を扱うAPIが無いため、本体のメモリ上にあるUndo履歴(Undo::UndoService)を直接読んで写しを作る。
//	・UndoServiceの場所は本体の「元に戻す」の処理の先頭の命令から、Undoの項目・設定項目の種類はRTTIのクラス名から、起動時に割り出す
//	・UPDATE_OBJECTの通知と定期的な確認で、本体の履歴(段の並びと現在位置)の変化を見て写しを合わせる
//	・各段の説明は、その段のUndoの項目が持つ変更前・変更後の状態と、対象のオブジェクト・エフェクトをメモリから読んで作る
//	本体のメモリには書き込まない。読み取りは例外を捕まえて行い、割り出せない版では機能を止める
//
//	By teruyoshi
//----------------------------------------------------------------------------------
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <tuple>

#include "plugin2.h"
#include "logger2.h"
#include "config2.h"

#define PLUGIN_NAME			L"EditHistory"
#define WINDOW_CLASS_NAME	L"EditHistoryWindow"
#define BUSY_CLASS_NAME		L"EditHistoryBusy"
#define IDC_LIST			1001

// 自ウィンドウへ処理を委譲するメッセージ
#define WM_APP_OBJECT_UPDATED	(WM_APP + 1)
#define WM_APP_SCENE_CHANGED	(WM_APP + 2)
#define WM_APP_PROJECT_LOADED	(WM_APP + 3)
#define WM_APP_INIT				(WM_APP + 5)
#define WM_APP_JUMP_KEEP		(WM_APP + 6)	// クリックで戻している間、投げたメッセージを絶やさずユーザーの入力を後回しにする

#define TIMER_ID_JUMP_TIMEOUT	1
#define TIMER_ID_BUSY			2		// 戻している途中の表示を回す
#define TIMER_ID_INIT			4		// 初期化の時にメインウィンドウ・メニューがまだ無ければ、少し待ってもう一度試す
#define TIMER_ID_POLL			5		// 通知の来ない変化(グループ化等)に備えて、本体の履歴を定期的に確かめる
#define INIT_RETRY_MS			100
#define POLL_INTERVAL_MS		100
#define BUSY_INTERVAL_MS		80
#define BUSY_DOTS				8		// ぐるぐるマークの点の数
#define JUMP_TIMEOUT_MS			10000	// クリックで戻す途中、送ったコマンドで本体の履歴がこの時間変わらなければ中止する (重い編集では1回の処理に時間がかかる)
#define JUMP_KEEP_MAX_MS		3000	// 送ったコマンドで本体の履歴がこの時間変わらなければ、入力を後回しにするメッセージを投げるのをやめる
#define LABEL_VALUE_MAX			24		// ラベルに出す値の最大文字数

EDIT_HANDLE* edit_handle = nullptr;
LOG_HANDLE* logger = nullptr;
CONFIG_HANDLE* config = nullptr;
HWND g_hwnd = nullptr;
HWND g_list = nullptr;
HFONT g_font = nullptr;
HFONT g_font_strike = nullptr;	// 取り消し線付き (テキストの消えた文字)

COMMON_PLUGIN_TABLE common_plugin_table = {
	PLUGIN_NAME,
	L"EditHistory version 1.0 By teruyoshi",
};

EXTERN_C __declspec(dllexport) void InitializeLogger(LOG_HANDLE* handle) {
	logger = handle;
}
EXTERN_C __declspec(dllexport) void InitializeConfig(CONFIG_HANDLE* handle) {
	config = handle;
}
EXTERN_C __declspec(dllexport) COMMON_PLUGIN_TABLE* GetCommonPluginTable(void) {
	return &common_plugin_table;
}

void log_line(const std::wstring& message) {
	if (logger) logger->log(logger, message.c_str());
}

std::wstring utf8_to_wide(const std::string& s) {
	if (s.empty()) return L"";
	int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
	std::wstring w(len, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), len);
	return w;
}

std::string wide_to_utf8(const std::wstring& w) {
	if (w.empty()) return "";
	int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s(len, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), len, nullptr, nullptr);
	return s;
}

//=======================================================================
//	履歴 (本体のUndoスタックの写し、シーンごと)
//=======================================================================

// 設定項目の値の変化
enum { VALUE_NUMBER, VALUE_OTHER, VALUE_COLOR_LIST };
struct ValueChange {
	std::wstring item;					// 「グループの最初の項目名\x1f項目名」
	std::string before, after;			// 数値・選択等: エイリアスと同じ書式のUTF-8 (選択等で変更前が分からなければ空) / 色リスト: encode_color_list()
	int kind = VALUE_NUMBER;
	std::vector<std::wstring> names;	// 色リストの色の名前 (開始色・中間色1…)
	std::string merge_sub;				// まとめる判定に加える区別 (色リスト: 変わった色の番号と、色か位置か)
};

struct Entry {
	std::wstring label;
	// 本体のUndoの段の項目(UndoItem*)の並び。本体の履歴と照らし合わせる目印 (開始時点は空)
	std::vector<uintptr_t> items;
	// 同じ対象への同じ種類の変更が続く行を、一覧で1行にまとめるための情報
	//   merge_key: 「\x1f…」文字列の項目への入力 / 「\x1e…」時間制御の編集 (最後の行の説明を出す)
	//              「\x1d…」数値・選択・色リスト等の値 / 「\x1c移動」「\x1c長さ」位置 / 「\x1b…」レイヤー設定 (最初の変更前→最後の変更後で説明し直す)
	uintptr_t merge_object = 0;		// 対象 (オブジェクト。レイヤー設定はレイヤー番号+1)
	std::string merge_key;
	std::wstring merge_prefix;		// 説明し直す時の頭 (「図形[L1] 標準描画 」等)
	std::vector<ValueChange> values;	// 説明し直す時の変更前後の値 (位置は「レイヤー,開始,終了」)
	// テキストの入力 (一覧で、増えた文字を緑、消えた文字を取り消し線付きの赤で描くための情報)
	bool has_text = false;
	std::wstring text_head;				// 「テキスト[L1] テキスト」等 (「入力 (」の前)
	bool text_full_known = false;		// 入力後の全文が分かっているか (最新の段の時だけSDKで読める)
	std::wstring text_after;			// 入力後の全文
	int text_pos = 0;					// 削除・挿入した位置
	std::wstring text_inserted, text_removed;
};

struct SceneHistory {
	std::vector<Entry> entries;	// entries[0]は開始時点、entries[1..]は本体のUndoの段(古い順)
	int pos = 0;
	bool initialized = false;
	uintptr_t hist = 0;			// 対応する本体の履歴(History*)
};

std::map<int, SceneHistory> g_scenes;
int g_scene_id = -1;
bool g_initialized = false;
// 本体の起動(プロジェクトの初期化)が済み、編集ハンドルの関数を使えるか。
// register_project_load_handlerのコールバック(起動時のプロジェクトの初期化でも呼ばれる)が来たら立てる
bool g_host_ready = false;

SceneHistory& current_history() { return g_scenes[g_scene_id]; }

void update_list();

//=======================================================================
//	説明の文字列
//=======================================================================

std::wstring shorten(const std::wstring& s) {
	std::wstring t;
	for (wchar_t c : s) t += (c == L'\n' || c == L'\r' || c == L'\t') ? L' ' : c;
	if (t.size() > LABEL_VALUE_MAX) t = t.substr(0, LABEL_VALUE_MAX) + L"…";
	return t;
}

// フレーム番号を時刻の文字列にする (60秒未満は「1.23秒」、以上は「1:05.20」)
int g_rate = 60, g_scale = 1;	// ラベルを作る時点のシーンのフレームレート

std::wstring time_text(int frame) {
	double sec = g_rate > 0 ? (double)frame * g_scale / g_rate : 0;
	wchar_t buf[32];
	if (sec < 60) swprintf_s(buf, L"%.2f秒", sec);
	else swprintf_s(buf, L"%d:%05.2f", (int)(sec / 60), sec - (int)(sec / 60) * 60);
	return buf;
}

bool is_numeric_value(const std::string& v) {
	if (v.empty()) return false;
	for (char c : v) if (!((c >= '0' && c <= '9') || c == '.' || c == '-' || c == ',' || c == ' ')) return false;
	return true;
}

// トラックバーの値の文字列 (エイリアスと同じ書式。メモリから読んだ値をこの書式にしてから比べる)
//   移動無し: 「100.00」 / 移動あり: 「開始値,(中間点の値…,)終了値,移動方法,設定」 / 参照式: 設定の8のビットが立ち「|」の後ろが式
#define TRACK_PARAM_EXPRESSION	8

struct TrackValue {
	bool track = false;					// 移動方法が付いているか
	std::vector<std::string> values;	// 値の並び
	std::string move;					// 移動方法
	int param = 0;						// 設定 (ビットの組)
	bool has_expression = false;
	std::string expression;				// 参照式
};

TrackValue parse_track(const std::string& v) {
	TrackValue t;
	size_t bar = v.find('|');
	std::string main = v.substr(0, bar);
	std::vector<std::string> tokens;
	size_t pos = 0;
	while (true) {
		size_t comma = main.find(',', pos);
		tokens.push_back(main.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos));
		if (comma == std::string::npos) break;
		pos = comma + 1;
	}
	size_t k = 0;
	while (k < tokens.size() && is_numeric_value(tokens[k])) k++;
	if (k == 0 || k == tokens.size()) {
		t.values = { main };
		return t;
	}
	t.track = true;
	t.values.assign(tokens.begin(), tokens.begin() + k);
	t.move = tokens[k];
	if (k + 1 < tokens.size()) t.param = atoi(tokens[k + 1].c_str());
	if ((t.param & TRACK_PARAM_EXPRESSION) && bar != std::string::npos) {
		t.has_expression = true;
		t.expression = v.substr(bar + 1);
	}
	return t;
}

// 表示用に、移動方法等を除いた値の並びにする
std::string track_value_text(const std::string& v) {
	TrackValue t = parse_track(v);
	if (!t.track) return v;
	std::string text;
	for (size_t j = 0; j < t.values.size(); j++) text += (j ? "," : "") + t.values[j];
	return text;
}

// 1つの設定項目の値の変化を説明する (項目名の後ろに付ける文字列)
std::wstring describe_value_change(const std::string& old_value, const std::string& value) {
	TrackValue a = parse_track(old_value), b = parse_track(value);
	if (a.track || b.track) {
		// 移動方法の変更と参照式の変更は別々に説明する (参照式は「移動無し」のまま付くことが多い)
		std::string move_a = a.track ? a.move : "移動無し";
		std::string move_b = b.track ? b.move : "移動無し";
		std::wstring text;
		if (move_a != move_b) text = move_b == "移動無し" ? L" の移動方法を解除" : L" に " + utf8_to_wide(move_b) + L" を適用";
		if (a.has_expression != b.has_expression || a.expression != b.expression) {
			if (!text.empty()) text += L" /";
			if (!b.has_expression) text += L" の参照式を解除";
			else if (!a.has_expression) text += L" に参照式を設定 (" + shorten(utf8_to_wide(b.expression)) + L")";
			else text += L" の参照式を変更 (" + shorten(utf8_to_wide(a.expression)) + L"→" + shorten(utf8_to_wide(b.expression)) + L")";
		}
		if (!text.empty()) return text;
		if (a.values != b.values) {
			// 値の並び(始点,中間点…,終点)の数が同じなら、変わった点だけを「(始点) 0.00→100.00」の形で出す
			if (a.values.size() == b.values.size() && a.values.size() >= 2) {
				std::vector<size_t> diffs;
				for (size_t j = 0; j < b.values.size(); j++) if (a.values[j] != b.values[j]) diffs.push_back(j);
				if (diffs.size() <= 2) {
					for (size_t j : diffs) {
						std::wstring point = j == 0 ? L"始点" : j == b.values.size() - 1 ? L"終点" : L"中間点" + std::to_wstring(j);
						text += L" (" + point + L") " + shorten(utf8_to_wide(a.values[j])) + L"→" + shorten(utf8_to_wide(b.values[j]));
					}
					return text;
				}
			}
			return L" " + shorten(utf8_to_wide(track_value_text(old_value))) + L"→" + shorten(utf8_to_wide(track_value_text(value)));
		}
		// 値・移動方法・設定・参照式が同じで項目が変わった: 時間制御のカーブや移動方法の設定値 (内容までは追わない)
		if (b.move.find("時間制御") != std::string::npos) return L" の時間制御を編集";
		return L" の移動方法の設定を変更";
	}
	return L" " + shorten(utf8_to_wide(old_value)) + L"→" + shorten(utf8_to_wide(value));
}

// 並びのi番目のエフェクトの表示名。同じエフェクトが複数ある時は「ぼかし[2]」のように上から何番目か(1から)を付ける
std::wstring effect_display_name(const std::vector<std::wstring>& names, size_t i) {
	if (i >= names.size()) return L"エフェクト";
	int total = 0, order = 0;
	for (size_t j = 0; j < names.size(); j++) {
		if (names[j] != names[i]) continue;
		total++;
		if (j <= i) order++;
	}
	return total > 1 ? names[i] + L"[" + std::to_wstring(order) + L"]" : names[i];
}

// エフェクトの順番の変更・追加・削除・種類の変更を説明する (いずれでもなければ空)
std::wstring describe_effect_list_change(const std::vector<std::wstring>& na, const std::vector<std::wstring>& nb) {
	// 多い方の並びから1つ抜くと少ない方と一致すれば、それを追加・削除したとみなす
	if (na.size() + 1 == nb.size() || nb.size() + 1 == na.size()) {
		const auto& longer = na.size() > nb.size() ? na : nb;
		const auto& shorter = na.size() > nb.size() ? nb : na;
		// 同じ名前のエフェクトが並ぶ時はどれでも一致するので、後ろから探す (追加は普通末尾に入る)
		for (size_t i = longer.size(); i-- > 0;) {
			std::vector<std::wstring> r = longer;
			r.erase(r.begin() + i);
			if (r == shorter) return effect_display_name(longer, i) + (nb.size() > na.size() ? L" を追加" : L" を削除");
		}
		return nb.size() > na.size() ? L"エフェクトを追加" : L"エフェクトを削除";
	}
	if (na.size() != nb.size()) return nb.size() > na.size() ? L"エフェクトを追加" : L"エフェクトを削除";
	if (na == nb) return L"";
	std::vector<std::wstring> sa = na, sb = nb;
	std::sort(sa.begin(), sa.end());
	std::sort(sb.begin(), sb.end());
	if (sa != sb) {
		// 1つだけ違う: エフェクトの種類の変更
		std::vector<size_t> diffs;
		for (size_t i = 0; i < na.size(); i++) if (na[i] != nb[i]) diffs.push_back(i);
		if (diffs.size() == 1) return effect_display_name(na, diffs[0]) + L" を " + effect_display_name(nb, diffs[0]) + L" に変更";
		return L"エフェクトを変更";
	}
	// 動かしたエフェクトの候補: 前後の並びから1つずつ抜くと残りが一致するもの (変更前の並びの位置)
	std::vector<size_t> moved;
	for (size_t i = 0; i < na.size(); i++) {
		std::vector<std::wstring> ra = na;
		ra.erase(ra.begin() + i);
		for (size_t j = 0; j < nb.size(); j++) {
			if (nb[j] != na[i]) continue;
			std::vector<std::wstring> rb = nb;
			rb.erase(rb.begin() + j);
			if (ra == rb) { moved.push_back(i); break; }
		}
	}
	if (moved.size() == 1) return effect_display_name(na, moved[0]) + L" の順番を変更";
	// 隣どうしの入れ替えは、どちらを動かしたか分からない
	if (moved.size() == 2) return effect_display_name(na, moved[0]) + L" と " + effect_display_name(na, moved[1]) + L" を入れ替え";
	return L"エフェクトの順番を変更";
}

// オブジェクトの位置
struct ObjPos {
	int layer = 0, start = 0, end = 0;
	bool operator==(const ObjPos& o) const { return layer == o.layer && start == o.start && end == o.end; }
	bool operator!=(const ObjPos& o) const { return !(*this == o); }
	std::string text() const { return std::to_string(layer) + "," + std::to_string(start) + "," + std::to_string(end); }
	static ObjPos parse(const std::string& s) {
		ObjPos p;
		sscanf_s(s.c_str(), "%d,%d,%d", &p.layer, &p.start, &p.end);
		return p;
	}
};

// 位置の変化(移動・レイヤー移動・長さの変更)を説明する (変化が無ければ空)
std::wstring describe_position_change(const ObjPos& a, const ObjPos& b) {
	bool layer = a.layer != b.layer;
	bool start = a.start != b.start;
	bool end = a.end != b.end;
	bool same_length = (a.end - a.start) == (b.end - b.start);
	if (!layer && !start && !end) return L"";
	if (layer) {
		std::wstring text = L"を L" + std::to_wstring(b.layer + 1) + L" へ移動";
		if (start) text += L" (開始 " + time_text(a.start) + L"→" + time_text(b.start) + L")";
		return text;
	}
	if (same_length) return L"を移動 (開始 " + time_text(a.start) + L"→" + time_text(b.start) + L")";
	if (start && !end) return L"の長さを変更 (開始 " + time_text(a.start) + L"→" + time_text(b.start) + L")";
	if (!start && end) return L"の長さを変更 (終了 " + time_text(a.end + 1) + L"→" + time_text(b.end + 1) + L")";
	return L"の長さを変更 (" + time_text(a.start) + L"〜" + time_text(a.end + 1) + L" → " +
		time_text(b.start) + L"〜" + time_text(b.end + 1) + L")";
}

// 中間点(オブジェクトの開始からの相対フレーム)の変化を説明する (変化が無ければ空)
std::wstring describe_section_change(const std::vector<int>& a, int start_a, const std::vector<int>& b, int start_b) {
	if (a.size() < b.size()) {
		for (int f : b) if (std::find(a.begin(), a.end(), f) == a.end()) return L"中間点を追加 (" + time_text(start_b + f) + L")";
		return L"中間点を追加";
	}
	if (a.size() > b.size()) {
		for (int f : a) if (std::find(b.begin(), b.end(), f) == b.end()) return L"中間点を削除 (" + time_text(start_a + f) + L")";
		return L"中間点を削除";
	}
	for (size_t i = 0; i < a.size(); i++) {
		if (a[i] != b[i]) return L"中間点を移動 (" + time_text(start_a + a[i]) + L"→" + time_text(start_b + b[i]) + L")";
	}
	return L"";
}

// 設定項目がグループ(X,Y,Z等)に属していれば、グループの最初の項目名を返す (属していなければ項目名)。結果は覚えておく
std::wstring item_group_head(const std::wstring& effect, const std::wstring& item) {
	static std::map<std::pair<std::wstring, std::wstring>, std::wstring> cache;
	auto key = std::make_pair(effect, item);
	auto it = cache.find(key);
	if (it != cache.end()) return it->second;
	std::wstring head = item;
	if (edit_handle && !effect.empty()) {
		LPCWSTR names[32] = {};
		int n = edit_handle->get_effect_item_group_names(effect.c_str(), item.c_str(), names, 32, nullptr);
		if (n > 0 && names[0]) head = names[0];
	}
	cache[key] = head;
	return head;
}

//=======================================================================
//	メインメニュー (「元に戻す」「やり直し」)
//=======================================================================

std::wstring get_menu_text(HMENU menu, int pos) {
	wchar_t buf[256] = {};
	MENUITEMINFOW mii = { sizeof(mii) };
	mii.fMask = MIIM_STRING;
	mii.dwTypeData = buf;
	mii.cch = _countof(buf);
	if (!GetMenuItemInfoW(menu, pos, TRUE, &mii)) return L"";
	return buf;
}

// メニューの項目名 (表示言語ごと。日本語・英語・韓国語・中国語)。先に並べた名前を優先して探す
const wchar_t* const UNDO_NAMES[] = { L"元に戻す", L"Undo", L"되돌리다", L"撤销", nullptr };
const wchar_t* const REDO_NAMES[] = { L"やり直し", L"やり直す", L"Redo", L"다시 하다", L"重做", nullptr };

// アクセスキーの印(&)を除く
std::wstring strip_access_key(std::wstring text) {
	text.erase(std::remove(text.begin(), text.end(), L'&'), text.end());
	return text;
}

// 表示名がnameで始まるか (大文字小文字は区別する)。
// 後ろにアクセスキー(「(&U)」等)やショートカットキー表記が付くことがあるので前方一致にする。
// 途中に含まれるだけの項目(「取消组合对象」に対する「组合对象」、「Ungroup Object」に対する「Group Object」等)は一致としない
bool is_menu_name(const std::wstring& text, const wchar_t* name) {
	return strip_access_key(text).compare(0, wcslen(name), name) == 0;
}

// メニューを再帰的に探し、表示名がnameの項目のIDを返す (見つからなければ0)
// IDはプラグインのメニュー登録数によってずれるため、固定値ではなく名前で探す
UINT find_menu_item(HMENU menu, const wchar_t* name) {
	int count = GetMenuItemCount(menu);
	for (int i = 0; i < count; i++) {
		MENUITEMINFOW mii = { sizeof(mii) };
		mii.fMask = MIIM_ID | MIIM_SUBMENU;
		if (!GetMenuItemInfoW(menu, i, TRUE, &mii)) continue;
		if (mii.hSubMenu) {
			if (UINT id = find_menu_item(mii.hSubMenu, name)) return id;
		} else if (is_menu_name(get_menu_text(menu, i), name)) {
			return mii.wID;
		}
	}
	return 0;
}

// namesのどれかの項目のIDを探す (先に並べた名前を優先する)
UINT find_menu_id(const wchar_t* const* names) {
	HMENU menu = GetMenu(edit_handle->get_host_app_window());
	if (!menu) return 0;
	for (; *names; names++) {
		if (UINT id = find_menu_item(menu, *names)) return id;
	}
	return 0;
}

//=======================================================================
//	本体のメモリの読み取り
//	SDKにUndo履歴を読むAPIは無いので、本体のメモリ上の構造を直接読む (読むだけで、書き込みはしない)。
//	構造は aviutl2.exe ver 2.1.19 を解析して確かめたもの:
//	  UndoService (グローバル変数が指す)
//	    +hist_off : 今のシーンの履歴(History*)。履歴はシーンごとにあり、シーンを切り替えると差し替わる
//	  History
//	    +pos_off-0x20 : 段の並び(std::deque<std::vector<UndoItem*>>)のmap / -0x18 mapの大きさ / -0x10 先頭の位置 / -0x08 段の数
//	    +pos_off      : Undoした回数 (0なら最新の状態。段[0]が一番新しい)
//	  UndoItem : +0 仮想関数表(種類はRTTIのクラス名で見分ける) / +8 対象(Object* / EffectBase* / Layer*等)のshared_ptr /
//	             +0x18〜 変更前・変更後の状態 (種類ごとの形式は下の定数)
//	段はコミットした後は変わらない。新しい編集は「やり直せる段を捨てて先頭に積み、上限を超えた古い段を捨てる」。
//	場所は版ごとに変わるので、起動時に次の方法で割り出す:
//	  ・「元に戻す」の処理(ログの文字列「Undo::UndoService::undo」を参照する関数)の中の
//	    mov r1,[rip+UndoService*] / mov r2,[r1+hist_off] / mov r3,[r2+pos_off] の並びと、
//	    今のシーンを取る mov r,[rip+編集の管理*] / … [r+scene_off] / [r+scene_off+8] (shared_ptr<Scene>) から読む
//	  ・仮想関数表は、クラス名の型情報 → Complete Object Locator → 仮想関数表の順に探す
//	オブジェクト・エフェクト・設定項目の中の位置は固定の値で、読んだ値が想定の範囲かを確かめながら使う
//=======================================================================

enum UndoKind {
	UK_UNKNOWN, UK_OBJECT_UPDATE, UK_OBJECT_CREATE_DELETE, UK_OBJECT_FLAG, UK_OBJECT_EFFECT, UK_OBJECT_GROUP,
	UK_OBJECT_SECTION, UK_OBJECT_LAYER_RANGE, UK_EFFECT_UPDATE, UK_LAYER_UPDATE, UK_LAYER_MOVE, UK_LAYER_CREATE_DELETE,
	UK_COLOR_SETTING,
	// 設定項目(EffectItem)の種類
	UK_ITEM_NUMBER, UK_ITEM_TEXT, UK_ITEM_STRING, UK_ITEM_CHECK, UK_ITEM_COLOR, UK_ITEM_COLOR_LIST, UK_ITEM_SELECT, UK_NO_MOVE,
	UK_COUNT
};
const char* const KIND_RTTI[UK_COUNT] = {
	nullptr,
	".?AV?$UndoItemTemplate@VObject@Edit@@VObjectUndoUpdate@2@@Undo@@",			// レイヤー・開始・終了・名前・中間点
	".?AV?$UndoItemTemplate@VObject@Edit@@VObjectUndoCreateDelete@2@@Undo@@",	// オブジェクトの追加・削除
	".?AV?$UndoItemTemplate@VObject@Edit@@VObjectUndoFlag@2@@Undo@@",			// クリッピング・カメラ制御等のフラグ
	".?AV?$UndoItemTemplate@VObject@Edit@@VObjectUndoEffect@2@@Undo@@",			// エフェクトの並び(追加・削除・順番)
	".?AV?$UndoItemTemplate@VObject@Edit@@VObjectUndoGroup@2@@Undo@@",			// オブジェクトのグループ化
	".?AV?$UndoItemTemplate@VObject@Edit@@VObjectUndoSection@2@@Undo@@",			// 中間点
	".?AV?$UndoItemTemplate@VObject@Edit@@VObjectUndoLayerRange@2@@Undo@@",
	".?AV?$UndoItemTemplate@VEffectBase@Effect@@VEffectUndoUpdate@2@@Undo@@",	// エフェクトの設定項目・表示・ロック (対象はEffectBase)
	".?AV?$UndoItemTemplate@VLayer@Edit@@VLayerUndoUpdate@2@@Undo@@",			// レイヤーの名前・表示・ロック・レイヤー設定
	".?AV?$UndoItemTemplate@VLayer@Edit@@VLayerUndoMove@2@@Undo@@",
	".?AV?$UndoItemTemplate@VLayer@Edit@@VLayerUndoCreateDelete@2@@Undo@@",
	".?AV?$UndoItemTemplate@VColorSetting@Component@@VColorSettingUndoUpdate@2@@Undo@@",
	".?AVEffectItemNumber@Effect@@",
	".?AVEffectItemText@Effect@@",
	".?AVEffectItemString@Effect@@",
	".?AVEffectItemCheck@Effect@@",
	".?AVEffectItemColor@Effect@@",
	".?AVEffectItemColorList@Effect@@",
	".?AVEffectItemSelect@Effect@@",
	".?AVNoMove@Effect@@",
};


// Object
#define OBJ_EFFECTS			0x18	// std::vector<std::shared_ptr<EffectBase>> (16バイトずつ)
#define OBJ_GROUP			0x30	// 所属するグループ (std::shared_ptr。0ならグループ無し)
#define OBJ_START			0x50	// int 開始フレーム
#define OBJ_END				0x54	// int 終了フレーム
#define OBJ_SECTIONS		0x58	// std::vector<int> 中間点 (開始からの相対フレーム)
#define OBJ_LAYER			0x70	// int レイヤー (SDKと同じ0始まり)
#define OBJ_FLAGS			0x74	// 1バイトずつ: クリッピング / カメラ制御の対象 / グループ制御の対象 / 上のオブジェクトでクリッピング
#define OBJ_NAME			0x78	// std::wstring オブジェクト名
// EffectBase
#define EFFECT_OWNER		0x28	// Object*
#define EFFECT_NAME			0x40	// const wchar_t* エフェクト名
#define EFFECT_LOCK			0x59
#define EFFECT_DISABLE		0x5A
// EffectItem (EffectUndoUpdateの中の複製)
#define ITEM_NAME			0x10	// std::wstring 項目名
#define ITEM_VALUE			0x38	// チェック: bool / 色: int (0xBBGGRR) / テキスト・文字列: 削除・挿入した文字列 (std::wstring)
#define TEXT_POSITION		0x58	// テキスト・文字列: 削除・挿入した位置 (変更前の複製が削除した文字列、変更後の複製が挿入した文字列を持つ)
#define NUMBER_VALUES		0x40	// std::vector<int> 値の並び (始点,中間点…,終点)
#define NUMBER_DENOMINATOR	0x64	// int 値の分母 (小数2桁なら100)
#define NUMBER_MOVE			0x68	// 移動方法のオブジェクト (+8が名前のconst wchar_t*)
#define NUMBER_EXPRESSION	0xA0	// std::wstring 参照式
#define NUMBER_PARAM		0xC0	// int 設定 (ビットの組)
#define SELECT_OPTIONS		0x38	// 選択: 選択肢の表 (std::map<int, const wchar_t*>*。番号→名前。本体のエフェクトもスクリプトの--selectも同じ)
#define SELECT_INDEX		0x40	// 選択: int 選んだ番号
#define COLORLIST_COLORS	0x38	// 色リスト: std::vector<{uint32 色(0xAABBGGRR), float 位置, uint32}>
#define COLORLIST_NAMES		0x58	// 色リスト: std::vector<std::wstring> 色の名前 (開始色・中間色1…)
// Undoの項目の変更前・変更後の状態
#define UPDATE_BEFORE		0x18	// ObjectUndoUpdate: {int レイヤー, int 開始, int 終了, int, std::wstring 名前, std::vector<int>* 中間点}
#define UPDATE_AFTER		0x50
#define UPDATE_NAME			0x10
#define UPDATE_SECTIONS		0x30	// std::vector<int>* (中間点が無ければnull)
#define CREATE_DELETE_STATE	0x18	// ObjectUndoCreateDelete: int 変更前・変更後のレイヤー (-1はオブジェクトが無い)
#define FLAG_STATE			0x18	// ObjectUndoFlag: int 変更前・変更後
#define EFFECTS_BEFORE		0x18	// ObjectUndoEffect: エフェクトの並び
#define EFFECTS_AFTER		0x30
#define GROUP_BEFORE		0x18	// ObjectUndoGroup: グループ
#define GROUP_AFTER			0x28
#define SECTIONS_BEFORE		0x18	// ObjectUndoSection: 中間点
#define SECTIONS_AFTER		0x30
#define EFFECT_ITEMS_BEFORE	0x18	// EffectUndoUpdate: 項目ごとの複製(変わった項目だけ、他はnull)の並び、+0x18 非表示、+0x19 ロック
#define EFFECT_ITEMS_AFTER	0x38
#define LAYER_BEFORE		0x18	// LayerUndoUpdate: {int X, int Y, float 音量, float 左右, bool 非表示, bool ロック, std::wstring 名前}
#define LAYER_AFTER			0x60
// Scene
#define SCENE_LAYERS		0x08	// std::vector<レイヤーの枠*>。枠の+0x20がLayer*
#define LAYER_SLOT_LAYER	0x20

struct UndoLayout {
	bool ok = false;
	uintptr_t service_slot = 0;		// UndoService*が入っているグローバル変数のアドレス
	int hist_off = 0, pos_off = 0;
	uintptr_t editor_slot = 0;		// 今のシーンを持つ管理オブジェクト*が入っているグローバル変数のアドレス (見つからなければ0)
	int scene_off = 0;
	std::map<uintptr_t, UndoKind> vtables;
	std::wstring error;
} g_undo;

// 例外を捕まえてメモリを読む (__tryはC++のオブジェクトを持たない関数の中でしか使えないので分ける)
static bool safe_copy(void* dst, uintptr_t src, size_t size) {
	if (src < 0x10000) return false;
	__try {
		memcpy(dst, (const void*)src, size);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

template<class T> bool read_mem(uintptr_t addr, T& out) { return safe_copy(&out, addr, sizeof(T)); }
template<class T> T read_or(uintptr_t addr, T fallback) { T v; return read_mem(addr, v) ? v : fallback; }

// std::wstring (MSVC: 16バイトの領域か先頭のポインタ / +0x10 長さ / +0x18 容量)
std::wstring read_wstring(uintptr_t addr) {
	uint64_t size = 0, cap = 0;
	if (!read_mem(addr + 0x10, size) || !read_mem(addr + 0x18, cap) || size > 100000 || cap < size) return L"";
	uintptr_t p = cap >= 8 ? read_or<uintptr_t>(addr, 0) : addr;
	std::wstring s((size_t)size, L'\0');
	if (size && !safe_copy(s.data(), p, (size_t)size * 2)) return L"";
	return s;
}

// const wchar_t* の文字列
std::wstring read_cstr(uintptr_t p) {
	std::wstring s;
	for (int i = 0; i < 256; i++) {
		wchar_t c;
		if (!read_mem(p + i * 2, c) || !c) break;
		s += c;
	}
	return s;
}

// std::vector<T>
template<class T> std::vector<T> read_vector(uintptr_t addr, size_t stride = sizeof(T)) {
	uintptr_t vec[2] = {};
	std::vector<T> out;
	if (!safe_copy(vec, addr, sizeof(vec)) || vec[1] < vec[0] || (vec[1] - vec[0]) / stride > 100000) return out;
	for (uintptr_t p = vec[0]; p + sizeof(T) <= vec[1]; p += stride) {
		T v;
		if (!read_mem(p, v)) break;
		out.push_back(v);
	}
	return out;
}

struct ImageRange {
	uintptr_t start = 0, end = 0;
};

// 本体(exe)の節を名前で探す
ImageRange image_section(const char* name) {
	uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
	auto* dos = (const IMAGE_DOS_HEADER*)base;
	auto* nt = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
	const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
	for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
		char sec_name[9] = {};
		memcpy(sec_name, sec[i].Name, 8);
		if (strcmp(sec_name, name) == 0) return { base + sec[i].VirtualAddress, base + sec[i].VirtualAddress + sec[i].Misc.VirtualSize };
	}
	return {};
}

uintptr_t find_bytes(const ImageRange& r, const void* data, size_t size) {
	const char* begin = (const char*)r.start;
	const char* end = (const char*)r.end;
	const char* it = std::search(begin, end, (const char*)data, (const char*)data + size);
	return it == end ? 0 : (uintptr_t)it;
}

bool locate_undo_fail(const std::wstring& reason) {
	g_undo.ok = false;
	g_undo.error = reason;
	return false;
}

// 本体のUndo履歴・今のシーンの場所と、項目の種類(仮想関数表)を割り出す
bool locate_undo() {
	g_undo = UndoLayout();
	uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
	ImageRange text = image_section(".text"), rdata = image_section(".rdata"), data = image_section(".data");
	if (!text.start || !rdata.start || !data.start) return locate_undo_fail(L"本体の節が見つからない");

	// 「元に戻す」の処理が出すログの関数名 (終端の\0まで含めて探し、「…::undoText」と区別する)
	static const char UNDO_NAME[] = "Undo::UndoService::undo";
	uintptr_t str = find_bytes(rdata, UNDO_NAME, sizeof(UNDO_NAME));
	if (!str) return locate_undo_fail(L"「元に戻す」の処理の目印が見つからない");
	// 文字列を参照する lea r64,[rip+disp32] を探す
	uintptr_t lea = 0;
	for (uintptr_t p = text.start + 3; p + 4 <= text.end; p++) {
		int32_t disp;
		memcpy(&disp, (const void*)p, 4);
		if (p + 4 + (intptr_t)disp != str) continue;
		const uint8_t* b = (const uint8_t*)p - 3;
		if ((b[0] & 0xF8) == 0x48 && b[1] == 0x8D && (b[2] & 0xC7) == 0x05) {
			lea = p - 3;
			break;
		}
	}
	if (!lea) return locate_undo_fail(L"「元に戻す」の処理が見つからない");
	DWORD64 image_base = 0;
	PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(lea, &image_base, nullptr);
	if (!rf) return locate_undo_fail(L"「元に戻す」の処理の先頭が見つからない");
	uintptr_t func = (uintptr_t)image_base + rf->BeginAddress;
	uintptr_t func_end = (uintptr_t)image_base + rf->EndAddress;

	auto is_mov = [](const uint8_t* b) { return (b[0] & 0xF8) == 0x48 && b[1] == 0x8B; };
	auto is_rip = [](const uint8_t* b) { return (b[2] & 0xC7) == 0x05; };
	auto reg_of = [](const uint8_t* b) { return ((b[0] & 4) ? 8 : 0) | ((b[2] >> 3) & 7); };
	auto base_of = [](const uint8_t* b) { return ((b[0] & 1) ? 8 : 0) | (b[2] & 7); };
	auto is_disp8 = [](const uint8_t* b) { return (b[2] & 0xC0) == 0x40 && (b[2] & 7) != 4; };
	auto is_disp32 = [](const uint8_t* b) { return (b[2] & 0xC0) == 0x80 && (b[2] & 7) != 4; };
	auto rip_target = [](const uint8_t* b) { int32_t d; memcpy(&d, b + 3, 4); return (uintptr_t)b + 7 + (intptr_t)d; };

	// mov r1,[rip+disp32] (7バイト) / mov r2,[r1+disp8] (4バイト) / mov r3,[r2+disp8] (4バイト) が続く所を探す
	for (uintptr_t p = func; p + 15 <= lea; p++) {
		const uint8_t* a = (const uint8_t*)p;
		const uint8_t* b = a + 7;
		const uint8_t* c = a + 11;
		if (!is_mov(a) || !is_rip(a)) continue;
		if (!is_mov(b) || !is_disp8(b) || base_of(b) != reg_of(a)) continue;
		if (!is_mov(c) || !is_disp8(c) || base_of(c) != reg_of(b)) continue;
		g_undo.service_slot = rip_target(a);
		g_undo.hist_off = (int8_t)b[3];
		g_undo.pos_off = (int8_t)c[3];
		break;
	}
	if (!g_undo.service_slot) return locate_undo_fail(L"Undo履歴の場所を読み取れない");
	if (g_undo.service_slot < data.start || g_undo.service_slot >= data.end || g_undo.pos_off < 0x20 || (g_undo.pos_off & 7) ||
		g_undo.hist_off < 0 || (g_undo.hist_off & 7)) {
		return locate_undo_fail(L"Undo履歴の場所が想定と違う");
	}

	// 今のシーン: mov r1,[rip+X] の後に、r1を土台にした mov r,[r1+D] と mov r,[r1+D-8] (shared_ptr<Scene>の制御ブロックと本体) がある所。
	// レイヤーの番号を求めるのに使う (見つからなければレイヤーの番号を出さない)
	for (uintptr_t p = lea; p + 7 <= func_end && !g_undo.editor_slot; p++) {
		const uint8_t* a = (const uint8_t*)p;
		if (!is_mov(a) || !is_rip(a)) continue;
		int r = reg_of(a);
		std::set<int32_t> disps;
		for (uintptr_t q = p + 7; q + 7 <= std::min(func_end, p + 120); q++) {
			const uint8_t* b = (const uint8_t*)q;
			if (!is_mov(b) || !is_disp32(b) || base_of(b) != r) continue;
			int32_t d;
			memcpy(&d, b + 3, 4);
			disps.insert(d);
		}
		for (int32_t d : disps) {
			if (d >= 8 && disps.count(d - 8) && rip_target(a) >= data.start && rip_target(a) < data.end) {
				g_undo.editor_slot = rip_target(a);
				g_undo.scene_off = d - 8;
				break;
			}
		}
	}

	// RTTIから仮想関数表を探す。
	// 型情報(TypeDescriptor)は .data にあり、クラス名は先頭から0x10の位置。
	// Complete Object Locator(.rdata): +0 signature(1) / +0x0C 型情報のRVA / +0x14 自身のRVA。仮想関数表の直前の8バイトがCOLを指す
	for (int k = 1; k < UK_COUNT; k++) {
		const char* name = KIND_RTTI[k];
		uintptr_t td_name = find_bytes(data, name, strlen(name) + 1);
		if (!td_name) continue;	// その種類が無い版もありうる
		DWORD td_rva = (DWORD)(td_name - 0x10 - base);
		for (uintptr_t p = rdata.start + 12; p + 12 <= rdata.end; p += 4) {
			if (*(const DWORD*)p != td_rva) continue;
			uintptr_t col = p - 12;
			if (*(const DWORD*)col != 1 || *(const DWORD*)(col + 4) != 0 || *(const DWORD*)(col + 20) != (DWORD)(col - base)) continue;
			for (uintptr_t q = rdata.start; q + 16 <= rdata.end; q += 8) {
				if (*(const uint64_t*)q == (uint64_t)col) g_undo.vtables[q + 8] = (UndoKind)k;
			}
		}
	}
	std::set<int> found;
	for (auto& [vt, kind] : g_undo.vtables) found.insert(kind);
	if (!found.count(UK_OBJECT_UPDATE) || !found.count(UK_EFFECT_UPDATE)) return locate_undo_fail(L"Undoの項目の種類が見つからない");
	g_undo.ok = true;
	return true;
}

// 本体の履歴の写し (段は古い順)
struct UndoSnapshot {
	uintptr_t hist = 0;
	size_t pos = 0;		// Undoした回数
	std::vector<std::vector<uintptr_t>> steps;
};

// 本体の履歴の目印 (今のシーンの履歴・段の数・Undoした回数・最新の段の最初の項目)。
// 古い側の段は上限で捨てられる以外に変わらないので、これが同じなら履歴は変わっていない。全段を読む前にこれだけ比べる
struct UndoKey {
	uintptr_t hist = 0;
	uint64_t size = 0, pos = 0;
	uintptr_t newest = 0;
};

bool read_undo_key(UndoKey& k) {
	uintptr_t service = 0;
	if (!g_undo.ok || !read_mem(g_undo.service_slot, service) || !service) return false;
	if (!read_mem(service + g_undo.hist_off, k.hist) || !k.hist) return false;
	uint64_t map = 0, map_size = 0, off = 0;
	int p = g_undo.pos_off;
	if (!read_mem(k.hist + p - 0x20, map) || !read_mem(k.hist + p - 0x18, map_size) || !read_mem(k.hist + p - 0x10, off) ||
		!read_mem(k.hist + p - 0x08, k.size) || !read_mem(k.hist + p, k.pos)) return false;
	if (!k.size) return true;
	if (!map || !map_size || (map_size & (map_size - 1))) return false;
	uintptr_t block = 0, first = 0;	// 段[0](一番新しい段)のstd::vector<UndoItem*>の先頭
	if (!read_mem(map + 8 * (off & (map_size - 1)), block) || !read_mem(block, first)) return false;
	return !first || read_mem(first, k.newest);
}

bool read_undo_snapshot(UndoSnapshot& s) {
	uintptr_t service = 0;
	if (!g_undo.ok || !read_mem(g_undo.service_slot, service) || !service) return false;
	if (!read_mem(service + g_undo.hist_off, s.hist) || !s.hist) return false;
	uint64_t map = 0, map_size = 0, off = 0, size = 0, pos = 0;
	int p = g_undo.pos_off;
	if (!read_mem(s.hist + p - 0x20, map) || !read_mem(s.hist + p - 0x18, map_size) || !read_mem(s.hist + p - 0x10, off) ||
		!read_mem(s.hist + p - 0x08, size) || !read_mem(s.hist + p, pos)) return false;
	if (size > 100000 || pos > size) return false;
	if (size && (!map || !map_size || (map_size & (map_size - 1)))) return false;
	s.pos = (size_t)pos;
	s.steps.assign((size_t)size, {});
	for (uint64_t i = 0; i < size; i++) {
		uintptr_t block = 0;
		if (!read_mem(map + 8 * ((off + i) & (map_size - 1)), block)) return false;
		uintptr_t vec[2] = {};	// std::vector<UndoItem*>の先頭・末尾
		if (!safe_copy(vec, block, sizeof(vec)) || vec[1] < vec[0] || ((vec[1] - vec[0]) & 7) || (vec[1] - vec[0]) / 8 > 100000) return false;
		auto& items = s.steps[(size_t)(size - 1 - i)];	// 段[0]が一番新しいので、古い順に並べ替える
		items.resize((vec[1] - vec[0]) / 8);
		if (!items.empty() && !safe_copy(items.data(), vec[0], items.size() * 8)) return false;
	}
	return true;
}

// オブジェクト(仮想関数表を持つもの)の種類
UndoKind kind_of(uintptr_t object) {
	uintptr_t vt = 0;
	if (!read_mem(object, vt)) return UK_UNKNOWN;
	auto it = g_undo.vtables.find(vt);
	return it == g_undo.vtables.end() ? UK_UNKNOWN : it->second;
}

uintptr_t item_target(uintptr_t item) { return read_or<uintptr_t>(item + 8, 0); }

// オブジェクトのエフェクトの並び
std::vector<uintptr_t> effect_list(uintptr_t list_addr) {
	std::vector<uintptr_t> out;
	for (auto& sp : read_vector<std::pair<uintptr_t, uintptr_t>>(list_addr)) out.push_back(sp.first);
	return out;
}

std::vector<std::wstring> effect_names(const std::vector<uintptr_t>& effects) {
	std::vector<std::wstring> names;
	for (uintptr_t e : effects) names.push_back(read_cstr(read_or<uintptr_t>(e + EFFECT_NAME, 0)));
	return names;
}

// [Lレイヤー] とオブジェクトの表示名 (名前が付いていればその名前、無ければ最初のエフェクト名)
std::wstring object_title(uintptr_t obj, int layer, const std::wstring& name) {
	std::wstring title = name;
	if (title.empty()) {
		auto names = effect_names(effect_list(obj + OBJ_EFFECTS));
		if (!names.empty()) title = names[0];
	}
	return L"[L" + std::to_wstring(layer + 1) + L"]" + (title.empty() ? L"オブジェクト" : title);
}

std::wstring object_title(uintptr_t obj) {
	return object_title(obj, read_or<int>(obj + OBJ_LAYER, 0), read_wstring(obj + OBJ_NAME));
}

ObjPos object_pos(uintptr_t obj) {
	return { read_or<int>(obj + OBJ_LAYER, 0), read_or<int>(obj + OBJ_START, 0), read_or<int>(obj + OBJ_END, 0) };
}

// レイヤー(Layer*)の番号 (今のシーンから探す。見つからなければ-1)
int layer_index(uintptr_t layer) {
	if (!g_undo.editor_slot) return -1;
	uintptr_t editor = read_or<uintptr_t>(g_undo.editor_slot, 0);
	uintptr_t scene = editor ? read_or<uintptr_t>(editor + g_undo.scene_off, 0) : 0;
	if (!scene) return -1;
	auto slots = read_vector<uintptr_t>(scene + SCENE_LAYERS);
	for (size_t i = 0; i < slots.size(); i++) {
		if (read_or<uintptr_t>(slots[i] + LAYER_SLOT_LAYER, 0) == layer) return (int)i;
	}
	return -1;
}

// 色リストの色
struct ListColor {
	uint32_t color = 0;	// 0xAABBGGRR
	float pos = 0;
	bool operator==(const ListColor& o) const { return color == o.color && pos == o.pos; }
	bool operator!=(const ListColor& o) const { return !(*this == o); }
};

std::vector<ListColor> read_color_list(uintptr_t item) {
	struct Raw { uint32_t color; float pos; uint32_t reserved; };
	std::vector<ListColor> out;
	for (auto& r : read_vector<Raw>(item + COLORLIST_COLORS)) out.push_back({ r.color, r.pos });
	return out;
}

std::vector<std::wstring> read_color_list_names(uintptr_t item) {
	uintptr_t vec[2] = {};
	std::vector<std::wstring> out;
	if (!safe_copy(vec, item + COLORLIST_NAMES, sizeof(vec)) || vec[1] < vec[0] || (vec[1] - vec[0]) / 0x20 > 1000) return out;
	for (uintptr_t p = vec[0]; p + 0x20 <= vec[1]; p += 0x20) out.push_back(read_wstring(p));
	return out;
}

// 色リストを文字列にして持つ (まとめた行の説明を作り直すため)。「AABBGGRR/位置」をカンマで並べる
std::string encode_color_list(const std::vector<ListColor>& list) {
	std::string s;
	for (auto& c : list) {
		char buf[48];
		sprintf_s(buf, "%s%08x/%g", s.empty() ? "" : ",", c.color, c.pos);
		s += buf;
	}
	return s;
}

std::vector<ListColor> decode_color_list(const std::string& s) {
	std::vector<ListColor> out;
	size_t pos = 0;
	while (pos < s.size()) {
		size_t comma = s.find(',', pos);
		std::string t = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
		ListColor c;
		unsigned int color = 0;
		if (sscanf_s(t.c_str(), "%x/%f", &color, &c.pos) == 2) { c.color = color; out.push_back(c); }
		if (comma == std::string::npos) break;
		pos = comma + 1;
	}
	return out;
}

// 色の表示 (rrggbb。不透明でなければ透明度の16進を後ろに付ける)
std::wstring color_text(uint32_t c) {
	wchar_t buf[16];
	if ((c >> 24) == 0xff) swprintf_s(buf, L"%02x%02x%02x", c & 0xff, (c >> 8) & 0xff, (c >> 16) & 0xff);
	else swprintf_s(buf, L"%02x%02x%02x%02x", c & 0xff, (c >> 8) & 0xff, (c >> 16) & 0xff, c >> 24);
	return buf;
}

// 色リストの変化を説明する (変わった色だけを名前で出す)
std::wstring describe_color_list(const std::vector<std::wstring>& names, const std::vector<ListColor>& a, const std::vector<ListColor>& b) {
	auto name = [&](size_t i) {
		if (i < names.size() && !names[i].empty()) return names[i];
		return (names.empty() ? std::wstring(L"色") : names.back()) + L"(" + std::to_wstring(i + 1) + L")";
	};
	auto pos_text = [](float p) { wchar_t buf[32]; swprintf_s(buf, L"%g", p); return std::wstring(buf); };
	// 1色の追加・削除: 多い方から1つ抜くと少ない方と一致するもの (同じ色が並ぶ時は後ろから探す)
	if (a.size() + 1 == b.size() || b.size() + 1 == a.size()) {
		const auto& longer = a.size() > b.size() ? a : b;
		const auto& shorter = a.size() > b.size() ? b : a;
		for (size_t i = longer.size(); i-- > 0;) {
			auto r = longer;
			r.erase(r.begin() + i);
			if (r == shorter) return b.size() > a.size() ? name(i) + L" を追加 (" + color_text(longer[i].color) + L")" : name(i) + L" を削除";
		}
	}
	if (a.size() != b.size()) return L"色の数 " + std::to_wstring(a.size()) + L"→" + std::to_wstring(b.size());
	std::vector<std::wstring> texts;
	for (size_t i = 0; i < a.size(); i++) {
		if (a[i].color != b[i].color) texts.push_back(name(i) + L" " + color_text(a[i].color) + L"→" + color_text(b[i].color));
		if (a[i].pos != b[i].pos) texts.push_back(name(i) + L" の位置 " + pos_text(a[i].pos) + L"→" + pos_text(b[i].pos));
	}
	if (texts.empty()) return L"色リストを変更";
	if (texts.size() > 2) return std::to_wstring(texts.size()) + L"箇所の色を変更";
	std::wstring text;
	for (size_t i = 0; i < texts.size(); i++) text += (i ? L", " : L"") + texts[i];
	return text;
}

// 選択肢の表(std::map<int, const wchar_t*>)から番号の名前を探す (見つからなければ空)
// std::mapの節: +0 左 / +8 親 / +0x10 右 / +0x19 番兵か / +0x20 キー / +0x28 値。表の+0が番兵(+8が根)
std::wstring select_option_name(uintptr_t options, int index) {
	uintptr_t head = read_or<uintptr_t>(options, 0);
	if (!head) return L"";
	uintptr_t node = read_or<uintptr_t>(head + 8, 0);
	for (int depth = 0; depth < 64 && node && node != head && !read_or<uint8_t>(node + 0x19, 1); depth++) {
		int key = read_or<int>(node + 0x20, 0);
		if (key == index) return read_cstr(read_or<uintptr_t>(node + 0x28, 0));
		node = read_or<uintptr_t>(node + (index < key ? 0 : 0x10), 0);
	}
	return L"";
}

// 設定項目の値をエイリアスと同じ書式にする (分からない種類はfalse)
bool item_value_text(uintptr_t item, std::string& out) {
	switch (kind_of(item)) {
	case UK_ITEM_NUMBER: {
		auto values = read_vector<int>(item + NUMBER_VALUES);
		int denominator = read_or<int>(item + NUMBER_DENOMINATOR, 1);
		if (values.empty() || denominator <= 0) return false;
		int digits = 0;
		for (int d = denominator; d >= 10; d /= 10) digits++;
		auto number = [&](int v) {
			char buf[64];
			sprintf_s(buf, "%.*f", digits, (double)v / denominator);
			return std::string(buf);
		};
		uintptr_t move = read_or<uintptr_t>(item + NUMBER_MOVE, 0);
		int param = read_or<int>(item + NUMBER_PARAM, 0);
		bool no_move = !move || kind_of(move) == UK_NO_MOVE;
		if (no_move && !(param & TRACK_PARAM_EXPRESSION)) {
			out = number(values[0]);
			return true;
		}
		out.clear();
		for (size_t i = 0; i < values.size(); i++) out += (i ? "," : "") + number(values[i]);
		std::wstring move_name = move ? read_cstr(read_or<uintptr_t>(move + 8, 0)) : L"";
		out += "," + wide_to_utf8(move_name.empty() ? L"移動無し" : move_name) + "," + std::to_string(param);
		if (param & TRACK_PARAM_EXPRESSION) out += "|" + wide_to_utf8(read_wstring(item + NUMBER_EXPRESSION));
		return true;
	}
	case UK_ITEM_CHECK:
		out = read_or<uint8_t>(item + ITEM_VALUE, 0) ? "1" : "0";
		return true;
	case UK_ITEM_SELECT: {
		std::wstring name = select_option_name(read_or<uintptr_t>(item + SELECT_OPTIONS, 0), read_or<int>(item + SELECT_INDEX, -1));
		if (name.empty()) return false;
		out = wide_to_utf8(name);
		return true;
	}
	case UK_ITEM_COLOR: {
		uint32_t c = read_or<uint32_t>(item + ITEM_VALUE, 0);
		char buf[16];
		sprintf_s(buf, "%02x%02x%02x", c & 0xff, (c >> 8) & 0xff, (c >> 16) & 0xff);
		out = buf;
		return true;
	}
	default:
		return false;
	}
}

//=======================================================================
//	段の説明 (Undoの項目の変更前・変更後の状態から作る)
//=======================================================================

// 段が最新の状態の時に説明しているか (最新なら、本体にある今の値がその段の変更後の値)
bool g_describe_latest = false;

// 種類の分からない設定項目(選択・ファイル・フォント等)は、最新の段なら変更後の値をSDKで読み、変更前の値は前に読んだ値を使う
// (EFFECT_HANDLEはEffectBase*と同じ値。持ち主のオブジェクトのエフェクトの並びに含まれるかを確かめてから使う)
std::map<std::pair<uintptr_t, std::wstring>, std::string> g_item_value_cache;

bool sdk_item_value(uintptr_t effect, const std::wstring& item, std::string& out) {
	uintptr_t owner = read_or<uintptr_t>(effect + EFFECT_OWNER, 0);
	if (!owner || !edit_handle) return false;
	struct Param { uintptr_t effect, owner; const std::wstring* item; std::string value; bool ok; } param = { effect, owner, &item, "", false };
	edit_handle->call_read_section_param(&param, [](void* p, EDIT_SECTION* edit) {
		auto* q = (Param*)p;
		EFFECT_HANDLE list[64] = {};
		int n = edit->get_effect_list((OBJECT_HANDLE)q->owner, list, 64);
		bool found = false;
		for (int i = 0; i < n && i < 64; i++) if ((uintptr_t)list[i] == q->effect) found = true;
		if (!found) return;
		LPCSTR v = edit->get_effect_item_value((EFFECT_HANDLE)q->effect, q->item->c_str());
		if (v) { q->value = v; q->ok = true; }
	});
	if (param.ok) out = param.value;
	return param.ok;
}

// 1つのオブジェクトへの変化 (同じ段の項目をオブジェクトごとにまとめる)
struct ObjDelta {
	uintptr_t obj = 0;
	bool created = false, deleted = false;
	int created_layer = 0, deleted_layer = 0;	// 追加した・削除したオブジェクトのレイヤー (削除した後のオブジェクトはレイヤーを持たない)
	bool has_pos = false;
	ObjPos pos_before, pos_after;
	std::wstring name_before, name_after;
	bool has_flags = false;
	uint32_t flags_before = 0, flags_after = 0;
	bool has_sections = false;
	std::vector<int> sections_before, sections_after;
	bool has_effects = false;
	std::vector<uintptr_t> effects_before, effects_after;
	std::vector<uintptr_t> effect_items;	// EffectUndoUpdateの項目
};

// エフェクトの設定項目・表示・ロックの変化を説明する。
// 文字列の項目への入力・時間制御の編集・数値の値だけの変更なら、まとめるための情報をeに入れる
void describe_effect_items(const ObjDelta& d, std::wstring title, std::vector<std::wstring>& changes, Entry& e, bool single) {
	auto effects = effect_list(d.obj + OBJ_EFFECTS);
	auto names = effect_names(effects);
	int lock_on = 0, lock_off = 0, hide_on = 0, hide_off = 0, other = 0;
	bool hide_first = false;
	std::vector<std::wstring> texts;
	std::vector<ValueChange> value_changes;	// 値だけの変更 (数値・選択等・色リスト)
	bool values_only = true;
	std::wstring value_effect;
	int value_effect_count = 0;
	std::string text_key, timectl_key;
	int text_count = 0, timectl_count = 0;
	for (uintptr_t item : d.effect_items) {
		uintptr_t effect = item_target(item);
		size_t index = std::find(effects.begin(), effects.end(), effect) - effects.begin();
		std::wstring label = effect_display_name(names, index);
		std::wstring effect_name = index < names.size() ? names[index] : read_cstr(read_or<uintptr_t>(effect + EFFECT_NAME, 0));
		uint8_t hide_a = read_or<uint8_t>(item + EFFECT_ITEMS_BEFORE + 0x18, 0), hide_b = read_or<uint8_t>(item + EFFECT_ITEMS_AFTER + 0x18, 0);
		uint8_t lock_a = read_or<uint8_t>(item + EFFECT_ITEMS_BEFORE + 0x19, 0), lock_b = read_or<uint8_t>(item + EFFECT_ITEMS_AFTER + 0x19, 0);
		std::vector<std::pair<std::wstring, std::wstring>> items;	// このエフェクトで変わった項目名と説明
		if (lock_a != lock_b) {
			items.push_back({ L"", lock_b ? L"をロック" : L"のロックを解除" });
			(lock_b ? lock_on : lock_off)++;
		}
		if (hide_a != hide_b) {
			items.push_back({ L"", hide_b ? L"を非表示" : L"を表示" });
			(hide_b ? hide_on : hide_off)++;
			if (index == 0) hide_first = true;
		}
		auto before = read_vector<uintptr_t>(item + EFFECT_ITEMS_BEFORE);
		auto after = read_vector<uintptr_t>(item + EFFECT_ITEMS_AFTER);
		std::wstring timectl_items;
		for (size_t j = 0; j < after.size(); j++) {
			uintptr_t cb = j < before.size() ? before[j] : 0, ca = after[j];
			if (!ca) continue;
			std::wstring key = read_wstring(ca + ITEM_NAME);
			UndoKind type = kind_of(ca);
			std::string va, vb;
			bool known = item_value_text(ca, vb) && cb && item_value_text(cb, va);
			if (type == UK_ITEM_TEXT || type == UK_ITEM_STRING) {
				// 入力後の内容を出す (1文字ずつの項目を表示上まとめた行でも、最後の項目のラベルがそのまま使える)。
				// 段が持つのは削除・挿入した文字列だけなので、最新の段なら今の全文をSDKで読み、そうでなければ挿入・削除した文字列を出す
				std::wstring head = effect_name == L"テキスト" && key == L"テキスト" ? L"テキスト" : label + L" " + key + L" ";
				std::string full;
				std::wstring text;
				std::wstring inserted = read_wstring(ca + ITEM_VALUE), removed = cb ? read_wstring(cb + ITEM_VALUE) : L"";
				e.has_text = true;
				e.text_head = title + L" " + head;
				e.text_pos = read_or<int>(ca + TEXT_POSITION, 0);
				e.text_inserted = inserted;
				e.text_removed = removed;
				e.text_full_known = g_describe_latest && sdk_item_value(effect, key, full);
				e.text_after = utf8_to_wide(full);
				if (e.text_full_known) {
					text = shorten(e.text_after);
				} else {
					text = (removed.empty() ? L"" : L"-" + shorten(removed)) + (inserted.empty() ? L"" : L"+" + shorten(inserted));
				}
				texts.push_back(head + L"入力 (" + text + L")");
				text_count++;
				text_key = "\x1f" + std::to_string(index) + "\x1f" + wide_to_utf8(key);
				values_only = false;
				continue;
			}
			if (type == UK_ITEM_COLOR_LIST) {
				// 色リスト: 変わった色だけを名前で出す
				if (!cb) continue;
				auto list_a = read_color_list(cb), list_b = read_color_list(ca);
				auto list_names = read_color_list_names(ca);
				items.push_back({ L"", describe_color_list(list_names, list_a, list_b) });
				// 色の追加・削除はまとめない (後の色の変更が追加の説明に吸収されて見えなくなるため)。
				// 色・位置の変更は、同じ色の同じ変更(色か位置か)が続く時だけまとめる
				if (list_a.size() != list_b.size()) {
					values_only = false;
					continue;
				}
				std::string sub;
				for (size_t i = 0; i < list_a.size(); i++) {
					if (list_a[i].color != list_b[i].color) sub += "c" + std::to_string(i) + ",";
					if (list_a[i].pos != list_b[i].pos) sub += "p" + std::to_string(i) + ",";
				}
				value_changes.push_back({ item_group_head(effect_name, key) + L"\x1f" + key, encode_color_list(list_a), encode_color_list(list_b),
					VALUE_COLOR_LIST, list_names, sub });
				continue;
			}
			std::wstring desc;
			bool other_value = false;	// 選択等の値をSDKで読めた
			std::string other_before, other_after;
			if (known) {
				desc = describe_value_change(va, vb);
			} else {
				// 種類の分からない項目: 変更後の値をSDKで読む (変更前は前に読んだ値があれば使う)
				std::string now;
				auto cache_key = std::make_pair(effect, key);
				if (g_describe_latest && sdk_item_value(effect, key, now)) {
					auto it = g_item_value_cache.find(cache_key);
					other_before = it != g_item_value_cache.end() && it->second != now ? it->second : "";
					other_after = now;
					other_value = true;
					desc = other_before.empty() ? L" を " + shorten(utf8_to_wide(now)) + L" に変更" : describe_value_change(other_before, now);
					g_item_value_cache[cache_key] = now;
				} else {
					desc = L" を変更";
				}
			}
			// 移動方法の適用・時間制御の編集等、値を含まない同じ説明が続く時は「X,Y,Z に 直線移動 を適用」とまとめる
			bool merged = false;
			if (desc.find(L'→') == std::wstring::npos) {
				for (auto& [k, dsc] : items) {
					if (!k.empty() && dsc == desc) { k += L"," + key; merged = true; break; }
				}
			}
			if (!merged) items.push_back({ key, desc });
			if (desc == L" の時間制御を編集") timectl_items += key + L",";
			// 数値の値だけの変更か (移動方法・設定・参照式の数が変わらない)
			TrackValue ta = parse_track(va), tb = parse_track(vb);
			bool value_only = known && type == UK_ITEM_NUMBER && desc.find(L'→') != std::wstring::npos && ta.track == tb.track &&
				ta.move == tb.move && ta.param == tb.param && ta.expression == tb.expression && ta.values.size() == tb.values.size();
			if (value_only) value_changes.push_back({ item_group_head(effect_name, key) + L"\x1f" + key, va, vb });
			else if (known && type == UK_ITEM_SELECT) value_changes.push_back({ item_group_head(effect_name, key) + L"\x1f" + key, va, vb, VALUE_OTHER });
			else if (other_value) value_changes.push_back({ item_group_head(effect_name, key) + L"\x1f" + key, other_before, other_after, VALUE_OTHER });
			else values_only = false;
		}
		if (items.empty()) continue;
		if (items.size() == 1 && items[0].second == L" の時間制御を編集") {
			timectl_count++;
			timectl_key = "\x1e" + std::to_string(index) + "\x1f" + wide_to_utf8(timectl_items);
		}
		if (lock_a != lock_b || hide_a != hide_b) values_only = false;
		if (items.size() > (size_t)((lock_a != lock_b) + (hide_a != hide_b))) other++;
		std::wstring text = label + L" ";
		for (size_t j = 0; j < items.size(); j++) text += (j ? L", " : L"") + items[j].first + items[j].second;
		texts.push_back(text);
		value_effect = label;
		value_effect_count++;
	}
	// オブジェクト自体のロックは、オブジェクト本体(最初のエフェクト)と標準描画等のロックが一度に変わる。
	// 2つ以上のエフェクトのロックだけが同じ向きに変わったら、オブジェクトのロックとして1件にする
	if (!other && !text_count && lock_on + lock_off >= 2 && !hide_on && !hide_off && (lock_on == 0 || lock_off == 0)) {
		changes.push_back(lock_on ? L"をロック" : L"のロックを解除");
		return;
	}
	// オブジェクト本体の表示/非表示、または複数のエフェクトが一度に変わった場合はオブジェクトの表示/非表示とする
	if (!other && !text_count && !lock_on && !lock_off && (hide_first || hide_on + hide_off >= 2) && (hide_on == 0 || hide_off == 0)) {
		changes.push_back(hide_on ? L"を非表示" : L"を表示");
		return;
	}
	for (auto& t : texts) changes.push_back(t);
	if (!single || texts.size() != 1) return;
	// まとめるための情報 (1つのオブジェクトの1つのエフェクトだけが変わった時)
	e.merge_object = d.obj;
	if (text_count == 1) {
		e.merge_key = text_key;
	} else if (timectl_count == 1) {
		e.merge_key = timectl_key;
	} else if (values_only && !value_changes.empty() && value_effect_count == 1) {
		// 同じグループ(X,Y,Z等)の項目はグループで数える (「X,Yの変更」と「Xだけの変更」を同じパラメータとしてまとめる)
		std::set<std::wstring> groups;
		for (auto& v : value_changes) groups.insert(v.item.substr(0, v.item.find(L'\x1f')));
		e.merge_key = "\x1d" + wide_to_utf8(value_effect);
		for (auto& g : groups) e.merge_key += "\x1f" + wide_to_utf8(g);
		for (auto& v : value_changes) if (!v.merge_sub.empty()) e.merge_key += "\x1f" + v.merge_sub;
		e.merge_prefix = title + L" " + value_effect + L" ";
		e.values = value_changes;
	} else {
		e.merge_object = 0;
	}
}

// 1つのオブジェクトの変化を説明する
std::wstring describe_object(const ObjDelta& d, Entry& e, bool single) {
	if (d.created) return object_title(d.obj, d.created_layer, read_wstring(d.obj + OBJ_NAME)) + L" を追加 (" + time_text(read_or<int>(d.obj + OBJ_START, 0)) + L")";
	if (d.deleted) return object_title(d.obj, d.deleted_layer, read_wstring(d.obj + OBJ_NAME)) + L" を削除";
	ObjPos pos = d.has_pos ? d.pos_before : object_pos(d.obj);
	std::wstring title = object_title(d.obj, pos.layer, d.has_pos ? d.name_before : read_wstring(d.obj + OBJ_NAME));
	std::wstring position = d.has_pos ? describe_position_change(d.pos_before, d.pos_after) : L"";
	std::vector<std::wstring> changes;
	if (d.has_sections) {
		// 中間点の追加・削除では各設定項目の値の並びも変わるので、設定の変更としては出さない
		std::wstring s = describe_section_change(d.sections_before, d.has_pos ? d.pos_before.start : pos.start,
			d.sections_after, d.has_pos ? d.pos_after.start : pos.start);
		if (!s.empty()) return title + L" " + s + (position.empty() ? L"" : L" / " + position);
	}
	if (d.has_pos && d.name_before != d.name_after) {
		changes.push_back(L"の名前を変更 (" + (d.name_before.empty() ? L"なし" : shorten(d.name_before)) + L"→" +
			(d.name_after.empty() ? L"なし" : shorten(d.name_after)) + L")");
	}
	if (d.has_flags) {
		struct Flag { int byte; const wchar_t* on; const wchar_t* off; };
		static const Flag flags[] = {
			{ 0, L"のクリッピングを有効化", L"のクリッピングを解除" },
			{ 3, L"の上のオブジェクトでクリッピングを有効化", L"の上のオブジェクトでクリッピングを解除" },
			{ 1, L"をカメラ制御の対象にする", L"をカメラ制御の対象から外す" },
			{ 2, L"をグループ制御の対象にする", L"をグループ制御の対象から外す" },
		};
		std::vector<std::wstring> texts;
		for (auto& f : flags) {
			bool a = (d.flags_before >> (f.byte * 8)) & 0xff, b = (d.flags_after >> (f.byte * 8)) & 0xff;
			if (a != b) texts.push_back(b ? f.on : f.off);
		}
		// クリッピングを有効にすると本体がカメラ制御の対象から外すので、それは別に出さない
		if (texts.size() == 2 && texts[0] == flags[0].on && texts[1] == flags[2].off) texts.pop_back();
		for (auto& t : texts) changes.push_back(t);
	}
	if (d.has_effects) {
		// 追加・削除はエフェクトそのもの(EffectBase*)で見分ける (同じ名前のエフェクトが並んでいても、どれかが分かる)
		auto na = effect_names(d.effects_before), nb = effect_names(d.effects_after);
		std::vector<size_t> added, removed;
		for (size_t i = 0; i < d.effects_after.size(); i++) {
			if (std::find(d.effects_before.begin(), d.effects_before.end(), d.effects_after[i]) == d.effects_before.end()) added.push_back(i);
		}
		for (size_t i = 0; i < d.effects_before.size(); i++) {
			if (std::find(d.effects_after.begin(), d.effects_after.end(), d.effects_before[i]) == d.effects_after.end()) removed.push_back(i);
		}
		std::wstring s;
		if (added.size() == 1 && removed.empty()) s = effect_display_name(nb, added[0]) + L" を追加";
		else if (removed.size() == 1 && added.empty()) s = effect_display_name(na, removed[0]) + L" を削除";
		else s = describe_effect_list_change(na, nb);
		if (!s.empty()) changes.push_back(s);
	}
	bool effect_items_only = !d.has_pos && !d.has_flags && !d.has_effects && !d.has_sections;
	if (!d.effect_items.empty()) describe_effect_items(d, title, changes, e, single && effect_items_only);
	if (single && changes.empty() && !position.empty()) {
		// 位置だけの変更: 同じオブジェクトの移動・長さの変更が続く行をまとめる
		e.merge_object = d.obj;
		bool same_length = (d.pos_before.end - d.pos_before.start) == (d.pos_after.end - d.pos_after.start);
		e.merge_key = same_length ? "\x1c移動" : "\x1c長さ";
		e.merge_prefix = title + L" ";
		e.values = { { L"", d.pos_before.text(), d.pos_after.text() } };
	}
	std::wstring content;
	if (!changes.empty()) content = changes[0] + (changes.size() > 1 ? L" 他" + std::to_wstring(changes.size() - 1) + L"件" : L"");
	if (!position.empty() && !content.empty()) return title + L" " + position + L" / " + content;
	if (!position.empty()) return title + L" " + position;
	if (!content.empty()) return title + L" " + content;
	return title + L" を変更";
}

// 複数のオブジェクトの分割・移動・長さの変更を1件にまとめた説明 (当てはまらなければ空)
std::wstring describe_multi_objects(const std::vector<ObjDelta>& list) {
	// 分割: 同じレイヤーで、変化の前の区間 [s, e] が、変化の後の区間 [s, m-1] [m, e] の2つに分かれた
	std::vector<std::pair<ObjPos, uintptr_t>> before, after;
	bool positions_only = true;
	for (auto& d : list) {
		ObjPos p = object_pos(d.obj);
		if (d.deleted) before.push_back({ { d.deleted_layer, p.start, p.end }, d.obj });
		else if (d.created) after.push_back({ { d.created_layer, p.start, p.end }, d.obj });
		else if (d.has_pos) { before.push_back({ d.pos_before, d.obj }); after.push_back({ d.pos_after, d.obj }); }
		if (d.created || d.deleted || !d.has_pos || d.has_flags || d.has_effects || d.has_sections || !d.effect_items.empty() ||
			d.name_before != d.name_after) positions_only = false;
	}
	if (!before.empty() && after.size() == before.size() * 2) {
		std::vector<bool> used(after.size(), false);
		std::vector<int> points;
		for (auto& [b, obj] : before) {
			int first = -1, second = -1;
			for (size_t i = 0; i < after.size() && first < 0; i++) {
				const ObjPos& a = after[i].first;
				if (!used[i] && a.layer == b.layer && a.start == b.start && a.end < b.end) first = (int)i;
			}
			if (first < 0) break;
			for (size_t i = 0; i < after.size() && second < 0; i++) {
				const ObjPos& a = after[i].first;
				if (!used[i] && (int)i != first && a.layer == b.layer && a.start == after[first].first.end + 1 && a.end == b.end) second = (int)i;
			}
			if (second < 0) break;
			used[first] = used[second] = true;
			points.push_back(after[second].first.start);
		}
		if (points.size() == before.size()) {
			bool same_point = std::all_of(points.begin(), points.end(), [&](int f) { return f == points[0]; });
			std::wstring at = same_point ? L" (" + time_text(points[0]) + L")" : L"";
			if (before.size() == 1) return object_title(before[0].second, before[0].first.layer, L"") + L" を分割" + at;
			return std::to_wstring(before.size()) + L"個のオブジェクトを分割" + at;
		}
	}
	if (!positions_only || list.size() < 2) return L"";
	// 複数のオブジェクトをまとめて移動・長さ変更
	std::vector<const ObjDelta*> sorted;
	for (auto& d : list) sorted.push_back(&d);
	std::sort(sorted.begin(), sorted.end(), [](const ObjDelta* x, const ObjDelta* y) {
		return x->pos_before.layer != y->pos_before.layer ? x->pos_before.layer < y->pos_before.layer : x->pos_before.start < y->pos_before.start;
	});
	bool any_layer = false, any_start = false, any_end = false, all_same_length = true;
	std::vector<int> layer_a, layer_b, start_a, start_b, end_a, end_b;
	for (auto* d : sorted) {
		const ObjPos& a = d->pos_before;
		const ObjPos& b = d->pos_after;
		any_layer |= a.layer != b.layer;
		any_start |= a.start != b.start;
		any_end |= a.end != b.end;
		all_same_length &= (a.end - a.start) == (b.end - b.start);
		layer_a.push_back(a.layer); layer_b.push_back(b.layer);
		start_a.push_back(a.start); start_b.push_back(b.start);
		end_a.push_back(a.end + 1); end_b.push_back(b.end + 1);
	}
	// 変更前→変更後の一覧。4個以上は長くなるので、ずれ幅が揃っていれば「+0.50秒」の形にする
	auto frames_text = [&](const std::vector<int>& fa, const std::vector<int>& fb) -> std::wstring {
		if (fa.size() <= 3) {
			std::wstring ta, tb;
			for (size_t i = 0; i < fa.size(); i++) { ta += (i ? L"," : L"") + time_text(fa[i]); tb += (i ? L"," : L"") + time_text(fb[i]); }
			return ta + L"→" + tb;
		}
		int delta = fb[0] - fa[0];
		for (size_t i = 0; i < fa.size(); i++) if (fb[i] - fa[i] != delta) return L"";
		return (delta >= 0 ? L"+" : L"-") + time_text(delta >= 0 ? delta : -delta);
	};
	auto layers_text = [&]() -> std::wstring {
		if (layer_a.size() > 3) {
			int delta = layer_b[0] - layer_a[0];
			for (size_t i = 0; i < layer_a.size(); i++) if (layer_b[i] - layer_a[i] != delta) return L"";
			return L"レイヤー " + std::wstring(delta >= 0 ? L"+" : L"") + std::to_wstring(delta);
		}
		std::wstring ta, tb;
		for (size_t i = 0; i < layer_a.size(); i++) {
			ta += (i ? L"," : L"") + (L"L" + std::to_wstring(layer_a[i] + 1));
			tb += (i ? L"," : L"") + (L"L" + std::to_wstring(layer_b[i] + 1));
		}
		return ta + L"→" + tb;
	};
	std::wstring count = std::to_wstring(sorted.size()) + L"個のオブジェクト";
	std::vector<std::wstring> details;
	std::wstring what;
	if (all_same_length) {
		what = L"を移動";
		if (any_layer) details.push_back(layers_text());
		if (any_start) { std::wstring t = frames_text(start_a, start_b); if (!t.empty()) details.push_back(L"開始 " + t); }
	} else if (!any_layer) {
		what = L"の長さを変更";
		if (any_start && !any_end) { std::wstring t = frames_text(start_a, start_b); if (!t.empty()) details.push_back(L"開始 " + t); }
		if (any_end && !any_start) { std::wstring t = frames_text(end_a, end_b); if (!t.empty()) details.push_back(L"終了 " + t); }
	}
	if (what.empty()) return L"";
	std::wstring detail;
	for (auto& t : details) if (!t.empty()) detail += (detail.empty() ? L"" : L" / ") + t;
	return count + what + (detail.empty() ? L"" : L" (" + detail + L")");
}

// レイヤー設定の値の表示 (小数は末尾の0を省く)
std::wstring number_text(double v) {
	wchar_t buf[32];
	swprintf_s(buf, L"%.2f", v);
	std::wstring s = buf;
	while (!s.empty() && s.back() == L'0') s.pop_back();
	if (!s.empty() && s.back() == L'.') s.pop_back();
	return s;
}

// レイヤーの名前・表示・ロック・レイヤー設定の変化を説明する
std::wstring describe_layer(uintptr_t item, Entry& e, bool single) {
	int index = layer_index(item_target(item));
	std::wstring layer = index >= 0 ? L"レイヤー" + std::to_wstring(index + 1) : L"レイヤー";
	struct LayerState { int x = 0, y = 0; float volume = 0, balance = 0; uint8_t hide = 0, lock = 0; std::wstring name; };
	auto read_state = [&](int off) {
		LayerState s;
		s.x = read_or<int>(item + off, 0);
		s.y = read_or<int>(item + off + 4, 0);
		s.volume = read_or<float>(item + off + 8, 0);
		s.balance = read_or<float>(item + off + 12, 0);
		s.hide = read_or<uint8_t>(item + off + 0x10, 0);
		s.lock = read_or<uint8_t>(item + off + 0x11, 0);
		s.name = read_wstring(item + off + 0x18);
		return s;
	};
	LayerState a = read_state(LAYER_BEFORE), b = read_state(LAYER_AFTER);
	std::vector<std::wstring> texts;
	if (a.name != b.name) texts.push_back(b.name.empty() ? L"名前を標準に戻す" : L"名前「" + shorten(b.name) + L"」");
	if (a.hide != b.hide) texts.push_back(b.hide ? L"非表示" : L"表示");
	if (a.lock != b.lock) texts.push_back(b.lock ? L"ロック" : L"ロック解除");
	struct Setting { const wchar_t* name; std::wstring before, after; };
	std::vector<Setting> settings;
	if (a.x != b.x) settings.push_back({ L"X", std::to_wstring(a.x), std::to_wstring(b.x) });
	if (a.y != b.y) settings.push_back({ L"Y", std::to_wstring(a.y), std::to_wstring(b.y) });
	if (a.volume != b.volume) settings.push_back({ L"音量", number_text(a.volume), number_text(b.volume) });
	if (a.balance != b.balance) settings.push_back({ L"左右", number_text(a.balance), number_text(b.balance) });
	for (auto& s : settings) texts.push_back(std::wstring(s.name) + L" " + s.before + L"→" + s.after);
	if (texts.empty()) return layer + L" を変更";
	std::wstring text = layer + L" ";
	for (size_t i = 0; i < texts.size(); i++) text += (i ? L", " : L"") + texts[i];
	// 同じレイヤーの同じレイヤー設定の変更が続く時は表示上まとめる
	if (single && texts.size() == 1 && settings.size() == 1 && index >= 0) {
		e.merge_object = (uintptr_t)index + 1;
		e.merge_key = "\x1b" + wide_to_utf8(settings[0].name);
		e.merge_prefix = layer + L" " + settings[0].name + L" ";
		e.values = { { L"", wide_to_utf8(settings[0].before), wide_to_utf8(settings[0].after) } };
	}
	return text;
}

// 段の説明を作る (段の項目がまだ本体にある間に呼ぶ)。latest: 本体が今この段を適用した最新の状態か
void describe_step(Entry& e, bool latest) {
	g_describe_latest = latest;
	EDIT_INFO info = {};
	edit_handle->get_edit_info(&info, sizeof(info));
	if (info.rate > 0) { g_rate = info.rate; g_scale = info.scale; }
	e.merge_object = 0;
	e.merge_key.clear();
	e.merge_prefix.clear();
	e.values.clear();

	std::vector<ObjDelta> objects;
	auto delta_of = [&](uintptr_t obj) -> ObjDelta& {
		for (auto& d : objects) if (d.obj == obj) return d;
		objects.push_back(ObjDelta());
		objects.back().obj = obj;
		return objects.back();
	};
	std::vector<uintptr_t> layers;
	std::vector<std::tuple<uintptr_t, uintptr_t, uintptr_t>> groups;	// グループの変化 (オブジェクト, 変更前, 変更後)
	std::vector<std::wstring> others;
	for (uintptr_t item : e.items) {
		UndoKind kind = kind_of(item);
		uintptr_t target = item_target(item);
		switch (kind) {
		case UK_OBJECT_UPDATE: {
			ObjDelta& d = delta_of(target);
			d.has_pos = true;
			d.pos_before = { read_or<int>(item + UPDATE_BEFORE, 0), read_or<int>(item + UPDATE_BEFORE + 4, 0), read_or<int>(item + UPDATE_BEFORE + 8, 0) };
			d.pos_after = { read_or<int>(item + UPDATE_AFTER, 0), read_or<int>(item + UPDATE_AFTER + 4, 0), read_or<int>(item + UPDATE_AFTER + 8, 0) };
			d.name_before = read_wstring(item + UPDATE_BEFORE + UPDATE_NAME);
			d.name_after = read_wstring(item + UPDATE_AFTER + UPDATE_NAME);
			// 中間点の移動はこの項目の中間点の並びだけが変わる
			uintptr_t sa = read_or<uintptr_t>(item + UPDATE_BEFORE + UPDATE_SECTIONS, 0), sb = read_or<uintptr_t>(item + UPDATE_AFTER + UPDATE_SECTIONS, 0);
			auto va = sa ? read_vector<int>(sa) : std::vector<int>(), vb = sb ? read_vector<int>(sb) : std::vector<int>();
			if (va != vb) {
				d.has_sections = true;
				d.sections_before = va;
				d.sections_after = vb;
			}
			break;
		}
		case UK_OBJECT_CREATE_DELETE: {
			int layers_ba[2] = { 0, 0 };	// 変更前・変更後のレイヤー (-1はオブジェクトが無い)
			safe_copy(layers_ba, item + CREATE_DELETE_STATE, sizeof(layers_ba));
			ObjDelta& d = delta_of(target);
			if (layers_ba[0] < 0) { d.created = true; d.created_layer = layers_ba[1]; }
			else if (layers_ba[1] < 0) { d.deleted = true; d.deleted_layer = layers_ba[0]; }
			break;
		}
		case UK_OBJECT_FLAG: {
			ObjDelta& d = delta_of(target);
			d.has_flags = true;
			d.flags_before = read_or<uint32_t>(item + FLAG_STATE, 0);
			d.flags_after = read_or<uint32_t>(item + FLAG_STATE + 4, 0);
			break;
		}
		case UK_OBJECT_EFFECT: {
			ObjDelta& d = delta_of(target);
			d.has_effects = true;
			d.effects_before = effect_list(item + EFFECTS_BEFORE);
			d.effects_after = effect_list(item + EFFECTS_AFTER);
			break;
		}
		case UK_OBJECT_SECTION: {
			ObjDelta& d = delta_of(target);
			d.has_sections = true;
			d.sections_before = read_vector<int>(item + SECTIONS_BEFORE);
			d.sections_after = read_vector<int>(item + SECTIONS_AFTER);
			break;
		}
		case UK_OBJECT_GROUP:
			groups.push_back({ target, read_or<uintptr_t>(item + GROUP_BEFORE, 0), read_or<uintptr_t>(item + GROUP_AFTER, 0) });
			break;
		case UK_EFFECT_UPDATE: {
			uintptr_t owner = read_or<uintptr_t>(target + EFFECT_OWNER, 0);
			if (owner) delta_of(owner).effect_items.push_back(item);
			break;
		}
		case UK_LAYER_UPDATE: layers.push_back(item); break;
		case UK_LAYER_MOVE: others.push_back(L"レイヤーを移動"); break;
		case UK_LAYER_CREATE_DELETE: others.push_back(L"レイヤーを挿入・削除"); break;
		case UK_COLOR_SETTING: others.push_back(L"色の設定を変更"); break;
		default: others.push_back(L"(種類の分からない編集)"); break;
		}
	}
	// 作って削除した等、変化の無いもの(中間点を追加すると各エフェクトの値の並びも変わる等)は数えない
	std::vector<ObjDelta> changed;
	for (auto& d : objects) {
		if (d.created && d.deleted) continue;
		changed.push_back(d);
	}
	// 中間点の変更に伴う設定項目の値の並びの変化は、中間点の変更として説明する
	for (auto& d : changed) if (d.has_sections) d.effect_items.clear();
	// グループ化。同じ段で追加したオブジェクトがグループに入るのは追加の一部 (グループ化したオブジェクトの分割・貼り付け等)
	int grouped = 0, ungrouped = 0, regrouped = 0;
	for (auto& [obj, a, b] : groups) {
		bool created = false;
		for (auto& d : changed) if (d.obj == obj && d.created) created = true;
		if (created) continue;
		if (!a && b) grouped++;
		else if (a && !b) ungrouped++;
		else if (a != b) regrouped++;
	}

	std::vector<std::wstring> texts;
	bool single = changed.size() == 1 && layers.empty() && others.empty() && !grouped && !ungrouped && !regrouped;
	std::wstring multi = layers.empty() && others.empty() && !grouped && !ungrouped ? describe_multi_objects(changed) : L"";
	if (!multi.empty()) {
		texts.push_back(multi);
	} else {
		for (auto& d : changed) {
			texts.push_back(describe_object(d, e, single));
		}
	}
	if (grouped) texts.push_back(std::to_wstring(grouped) + L"個のオブジェクトをグループ化");
	if (ungrouped) texts.push_back(std::to_wstring(ungrouped) + L"個のオブジェクトのグループを解除");
	if (regrouped) texts.push_back(std::to_wstring(regrouped) + L"個のオブジェクトのグループを変更");
	for (uintptr_t item : layers) texts.push_back(describe_layer(item, e, layers.size() == 1 && changed.empty() && others.empty()));
	for (auto& t : others) if (std::find(texts.begin(), texts.end(), t) == texts.end()) texts.push_back(t);
	if (texts.empty()) {
		e.label = L"(内容の無い編集)";
		return;
	}
	e.label = texts[0] + (texts.size() > 1 ? L" 他" + std::to_wstring(texts.size() - 1) + L"件" : L"");
}

// 一覧でまとめた行(entries[first..last])の説明を、最初の変更前→最後の変更後で作り直す
std::wstring merged_label(const std::vector<Entry>& entries, int first, int last) {
	const Entry& f = entries[first];
	const Entry& l = entries[last];
	const std::string& key = l.merge_key;
	if (key.empty() || f.values.empty() || l.values.empty()) return l.label;
	// 頭(「[L1]図形 」等)は最初の行のもの (途中でレイヤーを移動しても、最初のレイヤーで出す)
	if (key[0] == '\x1c') {
		ObjPos a = ObjPos::parse(f.values[0].before), b = ObjPos::parse(l.values[0].after);
		std::wstring desc = describe_position_change(a, b);
		bool layer_moved = false;
		for (int i = first; i <= last; i++) {
			if (!entries[i].values.empty() && ObjPos::parse(entries[i].values[0].before).layer != ObjPos::parse(entries[i].values[0].after).layer) layer_moved = true;
		}
		// 別のレイヤーへ移動して元のレイヤーに戻った: 「を L1 へ移動」とする (最初と最後だけ比べると変化が無く見える)
		if (layer_moved && a.layer == b.layer) {
			desc = L"を L" + std::to_wstring(b.layer + 1) + L" へ移動";
			if (a.start != b.start) desc += L" (開始 " + time_text(a.start) + L"→" + time_text(b.start) + L")";
		}
		if (desc.empty()) desc = L"を移動 (開始 " + time_text(a.start) + L"→" + time_text(b.start) + L")";
		return f.merge_prefix + desc;
	}
	if (key[0] == '\x1b') {
		return f.merge_prefix + utf8_to_wide(f.values[0].before) + L"→" + utf8_to_wide(l.values[0].after);
	}
	if (key[0] == '\x1d') {
		// 項目ごとに、その項目が最初に変わった時の変更前と、最後に変わった時の変更後を比べる
		std::vector<std::wstring> items;
		std::map<std::wstring, ValueChange> range;
		for (int i = first; i <= last; i++) {
			for (auto& v : entries[i].values) {
				auto it = range.find(v.item);
				if (it == range.end()) {
					range[v.item] = v;
					items.push_back(v.item);
				} else {
					it->second.after = v.after;
				}
			}
		}
		std::wstring text = f.merge_prefix;
		for (size_t i = 0; i < items.size(); i++) {
			const ValueChange& v = range[items[i]];
			std::wstring name = items[i].substr(items[i].find(L'\x1f') + 1);
			text += i ? L", " : L"";
			if (v.kind == VALUE_COLOR_LIST) text += describe_color_list(v.names, decode_color_list(v.before), decode_color_list(v.after));
			else if (v.kind == VALUE_OTHER && v.before.empty()) text += name + L" を " + shorten(utf8_to_wide(v.after)) + L" に変更";
			else text += name + describe_value_change(v.before, v.after);
		}
		return text;
	}
	return l.label;
}

//=======================================================================
//	Keychain to Transformとの連携
//	Keychain to Transformの「操作中の変更をUndo 1件にまとめる」方式では、操作中に本体のUndoと書き込みを繰り返す。
//	途中の状態を読むと余計な段が見えるので、操作中(メインウィンドウに印のプロパティが付いている間)は読まず、
//	終わった後の確認(通知・定期的な確認)で本体の履歴に合わせる
//=======================================================================
const wchar_t* KEYCHAIN_ACTIVE_PROP_NAME = L"KeychainToTransform.SingleUndoActive";

bool is_keychain_active() {
	HWND main = edit_handle ? edit_handle->get_host_app_window() : nullptr;
	return main && GetPropW(main, KEYCHAIN_ACTIVE_PROP_NAME) != nullptr;
}

//=======================================================================
//	写しを本体の履歴に合わせる
//=======================================================================

// クリックで戻している途中の状態
struct Jump {
	bool active = false;
	int target = 0;		// 目標のentriesのインデックス
	int sent = 0;
	int max_send = 0;
	int last_pos = 0;
	ULONGLONG sent_tick = 0;	// 最後にコマンドを送った時刻
	bool keeping = false;		// WM_APP_JUMP_KEEPを投げ続けているか
	UINT undo_id = 0, redo_id = 0;	// 「元に戻す」「やり直し」のメニューのID (始める時に1回探す)
} g_jump;

void abort_jump(const std::wstring& reason) {
	if (!g_jump.active) return;
	g_jump.active = false;
	KillTimer(g_hwnd, TIMER_ID_JUMP_TIMEOUT);
	log_line(L"[EditHistory] 移動を中止しました (" + reason + L")");
}

// entries[from..to)を消す時、クリックで戻す目標の番号を補正する。目標が消える場合はlandedを目標にする
void adjust_jump_target(int from, int to, int landed) {
	if (!g_jump.active || to <= from) return;
	if (g_jump.target >= from && g_jump.target < to) g_jump.target = landed;
	else if (g_jump.target >= to) g_jump.target -= to - from;
}

// 新しい段: 説明を作る (段が本体から捨てられた後は項目のメモリを読めないので、見えた時に作っておく)
Entry make_entry(const std::vector<uintptr_t>& items, bool latest) {
	Entry e;
	e.items = items;
	describe_step(e, latest);
	return e;
}

struct SyncResult {
	bool ok = false;		// 本体の履歴を読めた
	bool changed = false;	// 本体の履歴か現在位置が変わった
	bool new_edit = false;	// 新しい段が積まれた
};

// 本体の履歴を読み、写しを合わせる
SyncResult sync_history(SceneHistory& sh) {
	SyncResult res;
	// 変わっていなければ何もしない (目印だけを読んで比べる。ほとんどの確認はここで終わる)
	UndoKey key;
	if (!read_undo_key(key)) return res;
	int m0 = (int)sh.entries.size() - 1;
	if (sh.hist == key.hist && (uint64_t)m0 == key.size && (uint64_t)(m0 - sh.pos) == key.pos &&
		(m0 == 0 || (!sh.entries.back().items.empty() && sh.entries.back().items[0] == key.newest))) {
		res.ok = true;
		return res;
	}
	UndoSnapshot s;
	if (!read_undo_snapshot(s)) return res;
	res.ok = true;
	res.changed = true;
	int n = (int)s.steps.size();
	int new_pos = n - (int)s.pos;

	int matched = 0;
	if (sh.hist != s.hist) {
		// 初めて読む(またはプロジェクトが変わった): 本体にある段を全て写す
		sh.entries.assign(1, Entry());
		sh.hist = s.hist;
		abort_jump(L"履歴が切り替わった");
		if (n) log_line(L"[EditHistory] 監視を始める前の編集が" + std::to_wstring(n) + L"件あります");
	} else {
		// 写しの段と本体の段を照らし合わせる。本体の最古の段が写しのどこにあるかを探し、そこから一致する所までを残す
		int m = (int)sh.entries.size() - 1;
		int d = -1;
		if (n > 0) {
			for (int i = 0; i < m; i++) {
				if (sh.entries[1 + i].items == s.steps[0]) { d = i; break; }
			}
		}
		if (d > 0) {
			// 上限を超えて捨てられた古い段: 写しの先頭を詰める (entries[d]が新しい開始時点になる)
			adjust_jump_target(0, d, 0);
			sh.entries.erase(sh.entries.begin(), sh.entries.begin() + d);
			m -= d;
		}
		if (d >= 0) {
			while (matched < m && matched < n && sh.entries[1 + matched].items == s.steps[matched]) matched++;
		} else if (m > 0) {
			log_line(L"[EditHistory] 本体の履歴と照らし合わせられないので、写しを作り直しました");
		}
		// 本体から無くなった新しい側の段 (やり直せる段を捨てた新しい編集)
		if (1 + matched < (int)sh.entries.size()) {
			adjust_jump_target(1 + matched, (int)sh.entries.size(), matched);
			sh.entries.resize(1 + matched);
		}
	}
	sh.entries[0] = Entry();
	sh.entries[0].label = L"開始時点";
	for (int k = matched; k < n; k++) sh.entries.push_back(make_entry(s.steps[k], k == n - 1 && s.pos == 0));
	res.new_edit = matched < n;
	sh.pos = new_pos;
	return res;
}

// 通知・定期的な確認で、本体の履歴の変化を写しに反映する
void send_next_jump_command();
bool is_escape_pressed();
bool g_read_error_logged = false;

void process_update() {
	if (!g_initialized || !g_undo.ok) return;
	// Keychain to Transformの操作中は途中の状態を読まない (終わった後の確認で合わせる)
	if (is_keychain_active()) return;
	EDIT_INFO info = {};
	edit_handle->get_edit_info(&info, sizeof(info));
	bool scene_changed = info.scene_id != g_scene_id;
	g_scene_id = info.scene_id;
	if (scene_changed) abort_jump(L"シーンが切り替わった");
	SceneHistory& sh = current_history();
	if (!sh.initialized) {
		sh.entries.assign(1, Entry());
		sh.entries[0].label = L"開始時点";
		sh.initialized = true;
	}
	SyncResult res = sync_history(sh);
	if (!res.ok) {
		if (!g_read_error_logged) log_line(L"[EditHistory] 本体のUndo履歴を読めませんでした");
		g_read_error_logged = true;
		return;
	}
	if (!res.changed && !scene_changed) return;
#ifdef EH_DEBUG_LOG
	{
		std::wstring dump = L"[EditHistory] 写し pos=" + std::to_wstring(sh.pos) + L"/" + std::to_wstring(sh.entries.size() - 1);
		for (size_t i = sh.entries.size() > 6 ? sh.entries.size() - 6 : 0; i < sh.entries.size(); i++) dump += L"\n  " + std::wstring((int)i == sh.pos ? L"*" : L" ") + std::to_wstring(i) + L" " + sh.entries[i].label;
		log_line(dump);
	}
#endif

	// クリックで戻している途中なら、進み具合を確かめる
	bool send_next = false;
	if (g_jump.active) {
		if (res.new_edit) {
			abort_jump(L"移動中に編集された");
		} else if (g_jump.target >= (int)sh.entries.size()) {
			abort_jump(L"目標の項目が無くなった");
		} else if (sh.pos == g_jump.target) {
			g_jump.active = false;
			KillTimer(g_hwnd, TIMER_ID_JUMP_TIMEOUT);
		} else if (sh.pos == g_jump.last_pos) {
			// まだ本体が処理していない
		} else if ((g_jump.target < g_jump.last_pos) != (sh.pos < g_jump.last_pos)) {
			abort_jump(L"想定と違う向きに移動した");
		} else {
			send_next = true;
		}
	}
	// Escで中止 (戻している間は入力が処理されないので、キーの状態を直接見る)
	if (send_next && is_escape_pressed()) {
		send_next = false;
		abort_jump(L"Escで中止した");
	}
	// 次のコマンドはすぐに送る。投げたメッセージは入力・再描画より先に処理されるので、戻し終わるまで
	// ユーザーの入力は処理されず、戻し終わった後に順に処理される。
	// 本体の画面・一覧も更新されないので、戻している途中の帯はsend_next_jump_command()で直接描く
	if (send_next) send_next_jump_command();
	update_list();
}

// Escが押されたか (AviUtl2が前面の時だけ)。押し続けていなくても、前回調べてから押されていれば真
bool is_escape_pressed() {
	SHORT state = GetAsyncKeyState(VK_ESCAPE);
	if (!(state & 0x8001)) return false;
	DWORD process = 0;
	GetWindowThreadProcessId(GetForegroundWindow(), &process);
	return process == GetCurrentProcessId();
}

//=======================================================================
//	クリックで戻す
//=======================================================================

void update_busy();
extern HWND g_busy;

void send_next_jump_command() {
	SceneHistory& sh = current_history();
	// 戻している途中の表示をすぐ描く (本体の処理中は描けないので、送る前に描いておく)
	update_busy();
	if (g_busy) UpdateWindow(g_busy);
	if (g_jump.target >= (int)sh.entries.size()) {
		abort_jump(L"目標の項目が無くなった");
		return;
	}
	// 元に戻せるか・やり直せるかは写しの現在位置で分かる (写しは本体の履歴と一致している)
	bool undo = g_jump.target < sh.pos;
	UINT id = undo ? g_jump.undo_id : g_jump.redo_id;
	if (undo ? sh.pos <= 0 : sh.pos >= (int)sh.entries.size() - 1) {
		abort_jump(undo ? L"これより前に戻せない" : L"これより後にやり直せない");
		return;
	}
	if (++g_jump.sent > g_jump.max_send) {
		abort_jump(L"送った回数が上限を超えた");
		return;
	}
	g_jump.last_pos = sh.pos;
	g_jump.sent_tick = GetTickCount64();
	SetTimer(g_hwnd, TIMER_ID_JUMP_TIMEOUT, JUMP_TIMEOUT_MS, nullptr);
	PostMessageW(edit_handle->get_host_app_window(), WM_COMMAND, MAKEWPARAM(id, 0), 0);
	// 戻し終わるまで投げたメッセージを絶やさない (絶えると、その隙にユーザーの入力が処理されて編集が割り込む)。
	// 本体の履歴の変化もこの中で確かめる (タイマーは処理されなくなるため)
	if (!g_jump.keeping) {
		g_jump.keeping = true;
		PostMessageW(g_hwnd, WM_APP_JUMP_KEEP, 0, 0);
	}
}

void start_jump(int target) {
	SceneHistory& sh = current_history();
	if (g_jump.active || target == sh.pos || target < 0 || target >= (int)sh.entries.size()) return;
	g_jump.undo_id = find_menu_id(UNDO_NAMES);
	g_jump.redo_id = find_menu_id(REDO_NAMES);
	if (!g_jump.undo_id || !g_jump.redo_id) {
		log_line(L"[EditHistory] メニューに「元に戻す」「やり直し」が見つからないので移動できません");
		return;
	}
	g_jump.active = true;
	g_jump.target = target;
	g_jump.sent = 0;
	g_jump.max_send = std::abs(target - sh.pos) + 5;
	GetAsyncKeyState(VK_ESCAPE);	// 前に押されたEscで中止しないよう、押された記録を読み捨てる
	// 戻している間は本体がマウスの処理(カーソルの形を決め直すWM_SETCURSOR)をしないので、ここで待機にすればAviUtl2全体で待機のままになる
	SetCursor(LoadCursor(nullptr, IDC_WAIT));
	send_next_jump_command();
	// 戻している途中であることを表示する (終わったかはタイマーで見て消す)
	update_busy();
	if (g_jump.active) SetTimer(g_hwnd, TIMER_ID_BUSY, BUSY_INTERVAL_MS, nullptr);
}

//=======================================================================
//	一覧の表示
//=======================================================================

// 一覧の1行 (同じ対象への同じ種類の連続した変更は1行にまとめる)
struct Row {
	int first = 0, last = 0;	// まとめたentriesの範囲
};
std::vector<Row> g_rows;

COLORREF color_from_config(LPCSTR key, COLORREF fallback) {
	if (!config) return fallback;
	int code = config->get_color_code(config, key);
	if (!code && strcmp(key, "Background") != 0) return fallback;
	return RGB((code >> 16) & 0xff, (code >> 8) & 0xff, code & 0xff);
}

void build_rows() {
	g_rows.clear();
	SceneHistory& sh = current_history();
	for (int i = 0; i < (int)sh.entries.size(); i++) {
		const Entry& e = sh.entries[i];
		if (!g_rows.empty() && !e.merge_key.empty() && i > 0) {
			const Row& prev = g_rows.back();
			const Entry& p = sh.entries[prev.last];
			// 現在位置をまたいでまとめると、やり直せる分との境目が分からなくなるのでまとめない
			bool crosses = prev.last <= sh.pos && i > sh.pos;
			if (p.merge_object == e.merge_object && p.merge_key == e.merge_key && !crosses) {
				g_rows.back().last = i;
				continue;
			}
		}
		g_rows.push_back({ i, i });
	}
}

void update_list() {
	if (!g_list) return;
	build_rows();
	SendMessageW(g_list, WM_SETREDRAW, FALSE, 0);
	SendMessageW(g_list, LB_RESETCONTENT, 0, 0);
	for (size_t i = 0; i < g_rows.size(); i++) SendMessageW(g_list, LB_ADDSTRING, 0, (LPARAM)L"");
	// 現在位置の行が見えるようにする
	SceneHistory& sh = current_history();
	for (size_t i = 0; i < g_rows.size(); i++) {
		if (g_rows[i].first <= sh.pos && sh.pos <= g_rows[i].last) {
			SendMessageW(g_list, LB_SETTOPINDEX, (WPARAM)std::max(0, (int)i - 3), 0);
			break;
		}
	}
	SendMessageW(g_list, WM_SETREDRAW, TRUE, 0);
	InvalidateRect(g_list, nullptr, TRUE);
}

COLORREF mix_color(COLORREF a, COLORREF b, int percent);

// テキストの入力の差分の表示 (増えた文字・消えた文字と、その前後の少しの文字)
struct TextSegment {
	std::wstring text;
	int style = 0;	// 0: そのまま / 1: 増えた文字 / 2: 消えた文字
};

// 入力前の全文 (入力後の全文から、挿入した文字列を除き削除した文字列を戻す)
bool text_before(const Entry& e, std::wstring& out) {
	const std::wstring& a = e.text_after;
	size_t pos = (size_t)std::max(0, e.text_pos);
	if (!e.text_full_known || pos > a.size() || a.compare(pos, e.text_inserted.size(), e.text_inserted) != 0) return false;
	out = a.substr(0, pos) + e.text_removed + a.substr(pos + e.text_inserted.size());
	return true;
}

// 一覧の行(entries[first..last])のテキストの入力の差分 (テキストの入力の行でなければfalse)
bool text_segments(const std::vector<Entry>& entries, int first, int last, std::vector<TextSegment>& out) {
	const Entry& f = entries[first];
	const Entry& l = entries[last];
	// 1つの文字列の項目だけを変えた段(まとめる情報が付く)に限る (他の変更も含む段は、ふつうの説明を出す)
	if (!l.has_text || l.merge_key.empty() || l.merge_key[0] != '\x1f') return false;
	auto flat = [](const std::wstring& t) {
		std::wstring r;
		for (wchar_t c : t) r += (c == L'\n' || c == L'\r' || c == L'\t') ? L' ' : c;
		return r;
	};
	const size_t CONTEXT = 8;
	std::wstring before;
	out.clear();
	if (l.text_full_known && text_before(f, before)) {
		// 最初の入力前と最後の入力後を比べ、前後の一致する部分を除いた所を増えた・消えた文字とする
		const std::wstring& after = l.text_after;
		size_t p = 0;
		while (p < before.size() && p < after.size() && before[p] == after[p]) p++;
		size_t q = 0;
		while (q < before.size() - p && q < after.size() - p && before[before.size() - 1 - q] == after[after.size() - 1 - q]) q++;
		std::wstring head = after.substr(0, p), tail = after.substr(after.size() - q);
		if (head.size() > CONTEXT) head = L"…" + head.substr(head.size() - CONTEXT);
		if (tail.size() > CONTEXT) tail = tail.substr(0, CONTEXT) + L"…";
		out.push_back({ flat(head), 0 });
		out.push_back({ shorten(before.substr(p, before.size() - p - q)), 2 });
		out.push_back({ shorten(after.substr(p, after.size() - p - q)), 1 });
		out.push_back({ flat(tail), 0 });
		return true;
	}
	if (first != last) return false;
	// 入力後の全文が分からない (最新でない段): 削除・挿入した文字列だけ
	out.push_back({ shorten(l.text_removed), 2 });
	out.push_back({ shorten(l.text_inserted), 1 });
	return true;
}

void draw_item(const DRAWITEMSTRUCT* dis) {
	if (dis->itemID == (UINT)-1 || dis->itemID >= g_rows.size()) return;
	const Row& row = g_rows[dis->itemID];
	SceneHistory& sh = current_history();
	bool current = row.first <= sh.pos && sh.pos <= row.last;
	bool future = row.first > sh.pos;
	bool merged = row.last > row.first;
	COLORREF back = current ? color_from_config("GroupingSelect", RGB(0x48, 0x48, 0x48)) : color_from_config("Background", RGB(0x20, 0x20, 0x20));
	COLORREF text = future ? color_from_config("TextDisable", RGB(0x90, 0x90, 0x90)) : color_from_config("Text", RGB(0xff, 0xff, 0xff));
	HBRUSH brush = CreateSolidBrush(back);
	FillRect(dis->hDC, &dis->rcItem, brush);
	DeleteObject(brush);

	// まとめた行は、最後の項目のラベル (入力後の内容等) を出す。
	// 同じパラメータ・位置・レイヤー設定の連続した変更をまとめた行は、最初の変更前→最後の変更後の変化として説明し直す
	std::wstring label = merged ? merged_label(sh.entries, row.first, row.last) : sh.entries[row.last].label;
	std::vector<TextSegment> segments;
	bool has_segments = g_undo.ok && text_segments(sh.entries, row.first, row.last, segments);
	// 本体のUndo履歴を割り出せなかった版では、記録できないことを表示する
	if (g_initialized && !g_undo.ok) label = L"この版のAviUtl2には対応していません (" + g_undo.error + L")";
	SetBkMode(dis->hDC, TRANSPARENT);
	SetTextColor(dis->hDC, text);
	HGDIOBJ old = g_font ? SelectObject(dis->hDC, g_font) : nullptr;
	// 行番号: 現在の編集時点を0、Undo N回で戻れる時点をN、Redo M回で進める時点を-Mとする
	// (まとめた行はクリックした時の行き先であるまとまりの最後で数える)
	int number = current ? 0 : sh.pos - row.last;
	SIZE digit = {};
	GetTextExtentPoint32W(dis->hDC, L"-000", 4, &digit);
	RECT rc_number = dis->rcItem;
	rc_number.left += 4;
	rc_number.right = rc_number.left + digit.cx;
	// 複数の編集をまとめた行は、左端から番号の右までを少し明るくして区別する
	if (merged) {
		RECT mark = dis->rcItem;
		mark.right = rc_number.right + 5;
		HBRUSH mark_brush = CreateSolidBrush(mix_color(back, color_from_config("Text", RGB(0xff, 0xff, 0xff)), 12));
		FillRect(dis->hDC, &mark, mark_brush);
		DeleteObject(mark_brush);
	}
	std::wstring number_text = std::to_wstring(number);
	DrawTextW(dis->hDC, number_text.c_str(), -1, &rc_number, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
	RECT rc = dis->rcItem;
	rc.left = rc_number.right + 10;
	rc.right -= 4;
	if (has_segments) {
		// テキストの入力: 増えた文字を緑、消えた文字を取り消し線付きの赤で描く
		const Entry& l = sh.entries[row.last];
		std::vector<TextSegment> pieces = { { l.text_head + L"入力 (", 0 } };
		for (auto& seg : segments) if (!seg.text.empty()) pieces.push_back(seg);
		pieces.push_back({ L")", 0 });
		COLORREF added = RGB(0x5c, 0xd0, 0x6c), removed = RGB(0xf0, 0x64, 0x64);
		if (future) { added = mix_color(added, back, 45); removed = mix_color(removed, back, 45); }
		int x = rc.left;
		for (auto& piece : pieces) {
			if (x >= rc.right) break;
			HGDIOBJ font = piece.style == 2 && g_font_strike ? g_font_strike : g_font;
			if (font) SelectObject(dis->hDC, font);
			SetTextColor(dis->hDC, piece.style == 1 ? added : piece.style == 2 ? removed : text);
			RECT r = { x, rc.top, rc.right, rc.bottom };
			DrawTextW(dis->hDC, piece.text.c_str(), -1, &r, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
			SIZE size = {};
			GetTextExtentPoint32W(dis->hDC, piece.text.c_str(), (int)piece.text.size(), &size);
			x += size.cx;
		}
		SetTextColor(dis->hDC, text);
	} else {
		DrawTextW(dis->hDC, label.c_str(), -1, &rc, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
	}
	if (old) SelectObject(dis->hDC, old);
	// 現在の時点の行の下に線を引く (ここより下がやり直せる分)
	if (current) {
		RECT line = dis->rcItem;
		line.top = line.bottom - std::max(2, (int)(line.bottom - line.top) / 12);
		HBRUSH line_brush = CreateSolidBrush(color_from_config("Text", RGB(0xff, 0xff, 0xff)));
		FillRect(dis->hDC, &line, line_brush);
		DeleteObject(line_brush);
	}
}

HBRUSH g_back_brush = nullptr;

//-----------------------------------------------------------------------
//	戻している途中の表示 (一覧の下端に重ねる帯。ぐるぐるマークと残りの回数)
//-----------------------------------------------------------------------

HWND g_busy = nullptr;
int g_busy_phase = 0;

int list_item_height();

COLORREF mix_color(COLORREF a, COLORREF b, int percent) {
	return RGB((GetRValue(a) * (100 - percent) + GetRValue(b) * percent) / 100,
		(GetGValue(a) * (100 - percent) + GetGValue(b) * percent) / 100,
		(GetBValue(a) * (100 - percent) + GetBValue(b) * percent) / 100);
}

void layout_busy() {
	if (!g_busy || !g_hwnd) return;
	RECT rc;
	GetClientRect(g_hwnd, &rc);
	int height = list_item_height() + 4;
	SetWindowPos(g_busy, HWND_TOP, 0, std::max(0, (int)rc.bottom - height), rc.right, height, SWP_NOACTIVATE);
}

// 戻している途中なら帯を出して回し、終わっていれば消す
void update_busy() {
	if (!g_busy) return;
	if (g_jump.active) {
		g_busy_phase = (g_busy_phase + 1) % BUSY_DOTS;
		if (!IsWindowVisible(g_busy)) {
			layout_busy();
			ShowWindow(g_busy, SW_SHOWNOACTIVATE);
		}
		InvalidateRect(g_busy, nullptr, FALSE);
	} else {
		KillTimer(g_hwnd, TIMER_ID_BUSY);
		if (IsWindowVisible(g_busy)) {
			ShowWindow(g_busy, SW_HIDE);
			// 待機のカーソルを戻す (マウスを動かしたことにして、下のウィンドウにカーソルの形を決め直させる)
			POINT pt;
			if (GetCursorPos(&pt)) SetCursorPos(pt.x, pt.y);
		}
	}
}

void paint_busy(HWND hwnd) {
	PAINTSTRUCT ps;
	HDC hdc = BeginPaint(hwnd, &ps);
	RECT rc;
	GetClientRect(hwnd, &rc);
	COLORREF back = color_from_config("GroupingSelect", RGB(0x48, 0x48, 0x48));
	COLORREF fore = color_from_config("Text", RGB(0xff, 0xff, 0xff));
	HBRUSH brush = CreateSolidBrush(back);
	FillRect(hdc, &rc, brush);
	DeleteObject(brush);
	// ぐるぐるマーク: 円周上の点の明るさを順にずらす
	int size = rc.bottom - rc.top - 6;
	int cx = 6 + size / 2, cy = (rc.top + rc.bottom) / 2;
	double radius = size / 2.0 - size / 8.0;
	int dot = std::max(2, size / 6);
	HGDIOBJ old_pen = SelectObject(hdc, GetStockObject(NULL_PEN));
	for (int i = 0; i < BUSY_DOTS; i++) {
		double angle = 2 * 3.14159265358979 * i / BUSY_DOTS;
		int x = cx + (int)(radius * sin(angle)), y = cy - (int)(radius * cos(angle));
		int age = (g_busy_phase - i + BUSY_DOTS) % BUSY_DOTS;	// 0が先頭 (一番明るい)
		HBRUSH dot_brush = CreateSolidBrush(mix_color(back, fore, 100 - age * 100 / BUSY_DOTS));
		HGDIOBJ old_brush = SelectObject(hdc, dot_brush);
		Ellipse(hdc, x - dot / 2, y - dot / 2, x + dot / 2 + 1, y + dot / 2 + 1);
		SelectObject(hdc, old_brush);
		DeleteObject(dot_brush);
	}
	SelectObject(hdc, old_pen);
	// 残りの回数 (写しの項目の数で数えるので目安)
	SceneHistory& sh = current_history();
	int remain = std::abs(g_jump.target - sh.pos);
	std::wstring text = (g_jump.target < sh.pos ? L"元に戻しています" : L"やり直しています") +
		std::wstring(L" (残り ") + std::to_wstring(remain) + L") Escで中止";
	HGDIOBJ old_font = g_font ? SelectObject(hdc, g_font) : nullptr;
	SetBkMode(hdc, TRANSPARENT);
	SetTextColor(hdc, fore);
	RECT rc_text = rc;
	rc_text.left = cx + size / 2 + 8;
	DrawTextW(hdc, text.c_str(), -1, &rc_text, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
	if (old_font) SelectObject(hdc, old_font);
	EndPaint(hwnd, &ps);
}

LRESULT CALLBACK busy_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
	switch (message) {
	case WM_PAINT:
		paint_busy(hwnd);
		return 0;
	case WM_ERASEBKGND:
		return 1;
	}
	return DefWindowProcW(hwnd, message, wparam, lparam);
}

// 文字の大きさ (%)。一覧の上でCtrl+マウスホイールで変え、アプリケーションデータフォルダのEditHistory.iniに保存する
#define ZOOM_MIN	50
#define ZOOM_MAX	300
#define ZOOM_STEP	10
int g_zoom = 100;
std::wstring g_font_name = L"Yu Gothic UI";
float g_font_size = 13.0f;	// 100%の時の大きさ (style.confのFontのControl)
int g_item_height = 26;		// 100%の時の行の高さ (style.confのLayoutのListItemHeight)

std::wstring ini_path() {
	return config && config->app_data_path ? std::wstring(config->app_data_path) + L"\\EditHistory.ini" : L"";
}

void load_zoom() {
	std::wstring path = ini_path();
	if (path.empty()) return;
	g_zoom = std::clamp((int)GetPrivateProfileIntW(L"EditHistory", L"Zoom", 100, path.c_str()), ZOOM_MIN, ZOOM_MAX);
}

void save_zoom() {
	std::wstring path = ini_path();
	if (path.empty()) return;
	WritePrivateProfileStringW(L"EditHistory", L"Zoom", std::to_wstring(g_zoom).c_str(), path.c_str());
}


int list_item_height() {
	return std::max(g_item_height * g_zoom / 100, (int)(g_font_size * g_zoom / 100.0f * 1.4f));
}

// 倍率に合わせてフォントと行の高さを作り直す
void apply_zoom() {
	HFONT font = CreateFontW(-(int)(g_font_size * g_zoom / 100.0f + 0.5f), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
		OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, g_font_name.c_str());
	if (!font) return;
	if (g_font) DeleteObject(g_font);
	g_font = font;
	HFONT strike = CreateFontW(-(int)(g_font_size * g_zoom / 100.0f + 0.5f), 0, 0, 0, FW_NORMAL, FALSE, FALSE, TRUE, DEFAULT_CHARSET,
		OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, g_font_name.c_str());
	if (g_font_strike) DeleteObject(g_font_strike);
	g_font_strike = strike;
	if (g_list) {
		SendMessageW(g_list, LB_SETITEMHEIGHT, 0, list_item_height());
		InvalidateRect(g_list, nullptr, TRUE);
	}
	layout_busy();
}

// このプラグインのウィンドウにフォーカスがあると、本体のショートカットキー(Ctrl+Z等)が効かなくなる。
// 一覧はキー入力を使わないので、フォーカスを受け取ったら直前のウィンドウ(無ければメインウィンドウ)へすぐ返す
void return_focus(HWND previous) {
	HWND main = edit_handle ? edit_handle->get_host_app_window() : nullptr;
	HWND target = previous && IsWindow(previous) && previous != g_list && previous != g_hwnd ? previous : main;
	if (target) SetFocus(target);
}

// 一覧の上でのクリックで戻す・Ctrl+マウスホイールで文字の大きさを変える
LRESULT CALLBACK list_subclass_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR) {
	// 戻している途中は一覧の上でも待機のカーソルにする
	if (message == WM_SETCURSOR && g_jump.active) {
		SetCursor(LoadCursor(nullptr, IDC_WAIT));
		return TRUE;
	}
	// クリックはリストボックスに渡さず自分で処理する (リストボックスはクリックでフォーカスを取ってしまうため)
	if (message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK) {
		LRESULT hit = SendMessageW(hwnd, LB_ITEMFROMPOINT, 0, lparam);
		int index = LOWORD(hit);
		if (!HIWORD(hit) && index < (int)g_rows.size() && g_initialized) start_jump(g_rows[index].last);
		return 0;
	}
	if (message == WM_SETFOCUS) {
		return_focus((HWND)wparam);
		return 0;
	}
	if (message == WM_MOUSEWHEEL && (GET_KEYSTATE_WPARAM(wparam) & MK_CONTROL)) {
		int zoom = std::clamp(g_zoom + (GET_WHEEL_DELTA_WPARAM(wparam) > 0 ? ZOOM_STEP : -ZOOM_STEP), ZOOM_MIN, ZOOM_MAX);
		if (zoom != g_zoom) {
			g_zoom = zoom;
			apply_zoom();
			save_zoom();
		}
		return 0;
	}
	return DefSubclassProc(hwnd, message, wparam, lparam);
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
	switch (message) {
	case WM_SETFOCUS:
		// 本体がウィンドウの切り替え等でフォーカスを渡してきても、ショートカットキーが効くように返す
		return_focus((HWND)wparam);
		return 0;

	case WM_SIZE:
		if (g_list) MoveWindow(g_list, 0, 0, LOWORD(lparam), HIWORD(lparam), TRUE);
		layout_busy();
		return 0;

	case WM_MEASUREITEM: {
		auto* mis = (MEASUREITEMSTRUCT*)lparam;
		mis->itemHeight = list_item_height();
		return TRUE;
	}

	case WM_DRAWITEM:
		draw_item((const DRAWITEMSTRUCT*)lparam);
		return TRUE;

	case WM_CTLCOLORLISTBOX:
		if (!g_back_brush) g_back_brush = CreateSolidBrush(color_from_config("Background", RGB(0x20, 0x20, 0x20)));
		return (LRESULT)g_back_brush;

	case WM_APP_INIT:
		// 本体の起動(プロジェクトの初期化)が済むまで初期化しない。
		// 編集ハンドルの関数はRegisterPluginの処理の間は使えない(SDKのEDIT_HANDLEの説明)。
		// 起動時に「スクリプト・プラグインの追加」の確認ダイアログが出ると、そのメッセージループで投げておいたメッセージが
		// 起動の途中に処理される。そこで編集ハンドルの関数を呼ぶと本体が落ちた(アクセス違反)
		if (!g_initialized && g_host_ready) {
			HWND main = edit_handle->get_host_app_window();
			if (!main || !GetMenu(main)) {
				// メインウィンドウ・メニューがまだ無ければ後でもう一度 (投げ直すと空回りするのでタイマーで待つ)
				SetTimer(hwnd, TIMER_ID_INIT, INIT_RETRY_MS, nullptr);
				return 0;
			}
			if (!find_menu_id(UNDO_NAMES) || !find_menu_id(REDO_NAMES)) log_line(L"[EditHistory] メニューに「元に戻す」「やり直し」が見つかりません");
			if (locate_undo()) {
				wchar_t buf[160];
				swprintf_s(buf, L"[EditHistory] 本体のUndo履歴を見つけました (+0x%zX, 履歴=+0x%X, 位置=+0x%X, 項目の種類=%zu)",
					g_undo.service_slot - (uintptr_t)GetModuleHandleW(nullptr), g_undo.hist_off, g_undo.pos_off, g_undo.vtables.size());
				log_line(buf);
			} else {
				log_line(L"[EditHistory] この版の本体には対応していません (" + g_undo.error + L")");
			}
			g_initialized = true;
			process_update();
			update_list();
			if (g_undo.ok) SetTimer(hwnd, TIMER_ID_POLL, POLL_INTERVAL_MS, nullptr);
		}
		return 0;

	case WM_APP_OBJECT_UPDATED:
	case WM_APP_SCENE_CHANGED:
		if (g_initialized && !g_jump.active) process_update();
		return 0;

	case WM_APP_JUMP_KEEP: {
		// クリックで戻している間、自分宛てに投げ続けてユーザーの入力を戻し終わるまで後回しにする
		// (タイマーも処理されなくなるので、本体の履歴の確認・Escと応答なしの確認・ぐるぐるを回すのもここで行う)
		if (!g_jump.active) {
			g_jump.keeping = false;
			return 0;
		}
		process_update();
		if (!g_jump.active) {
			g_jump.keeping = false;
			return 0;
		}
		ULONGLONG now = GetTickCount64();
		if (is_escape_pressed()) {
			g_jump.keeping = false;
			abort_jump(L"Escで中止した");
			update_list();
			return 0;
		}
		if (now - g_jump.sent_tick > JUMP_TIMEOUT_MS) {
			g_jump.keeping = false;
			abort_jump(L"応答が無い");
			update_list();
			return 0;
		}
		// 長く変化が無ければ投げるのをやめる (次にコマンドを送る時にまた始める。応答なしの判定はタイマーで行われる)
		if (now - g_jump.sent_tick > JUMP_KEEP_MAX_MS) {
			g_jump.keeping = false;
			return 0;
		}
		static ULONGLONG last_spin = 0;
		if (now - last_spin >= BUSY_INTERVAL_MS) {
			last_spin = now;
			update_busy();
			if (g_busy) UpdateWindow(g_busy);
		}
		Sleep(1);	// 投げ続けてCPUを使い切らないように
		PostMessageW(hwnd, WM_APP_JUMP_KEEP, 0, 0);
		return 0;
	}

	case WM_APP_PROJECT_LOADED:
		// プロジェクトの読み込み・新規作成で本体のUndo履歴は空になるので、全シーンの写しを捨てる
		abort_jump(L"プロジェクトが読み込まれた");
		g_scenes.clear();
		g_scene_id = -1;
		// プロジェクトの初期化・読み込みが済んだので、編集ハンドルの関数を使える (起動時にも必ず1回来る)
		g_host_ready = true;
		if (g_initialized) {
			process_update();
			update_list();
		} else {
			PostMessageW(hwnd, WM_APP_INIT, 0, 0);
		}
		return 0;

	case WM_TIMER:
		if (wparam == TIMER_ID_INIT) {
			KillTimer(hwnd, TIMER_ID_INIT);
			PostMessageW(hwnd, WM_APP_INIT, 0, 0);
			return 0;
		}
		if (wparam == TIMER_ID_POLL) {
			// 通知の来ない編集(グループ化等)・Undo/Redoもあるので、定期的に本体の履歴を確かめる (変化が無ければ数十個の値を読むだけ)
			if (g_initialized && !g_jump.active) process_update();
			return 0;
		}
		if (wparam == TIMER_ID_BUSY) {
			update_busy();
			return 0;
		}
		if (wparam == TIMER_ID_JUMP_TIMEOUT) {
			KillTimer(hwnd, TIMER_ID_JUMP_TIMEOUT);
			abort_jump(L"応答が無い");
			update_list();
			return 0;
		}
		break;
	}
	return DefWindowProcW(hwnd, message, wparam, lparam);
}

//=======================================================================
//	コールバック・登録
//=======================================================================

// イベント通知スレッドから呼ばれるため、処理は自ウィンドウへ委譲する
void on_object_updated(void*) { PostMessageW(g_hwnd, WM_APP_OBJECT_UPDATED, 0, 0); }
void on_scene_changed(void*) { PostMessageW(g_hwnd, WM_APP_SCENE_CHANGED, 0, 0); }
void on_project_load(PROJECT_FILE*) { PostMessageW(g_hwnd, WM_APP_PROJECT_LOADED, 0, 0); }

EXTERN_C __declspec(dllexport) void UninitializePlugin() {
	if (g_list) RemoveWindowSubclass(g_list, list_subclass_proc, 1);
	if (g_font) DeleteObject(g_font);
	if (g_font_strike) DeleteObject(g_font_strike);
	if (g_back_brush) DeleteObject(g_back_brush);
}

EXTERN_C __declspec(dllexport) void RegisterPlugin(HOST_APP_TABLE* host) {
	WNDCLASSEXW wcex = {};
	wcex.cbSize = sizeof(WNDCLASSEX);
	wcex.lpszClassName = WINDOW_CLASS_NAME;
	wcex.lpfnWndProc = wnd_proc;
	wcex.hInstance = GetModuleHandle(0);
	wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
	if (!RegisterClassEx(&wcex)) return;
	// 親ウィンドウの指定無しでWS_CHILDが作れないので一旦WS_POPUPで作成する (SDKのサンプルと同じ)
	g_hwnd = CreateWindowEx(0, WINDOW_CLASS_NAME, PLUGIN_NAME, WS_POPUP,
		CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, nullptr, nullptr, GetModuleHandle(0), nullptr);
	if (!g_hwnd) return;
	if (config) {
		FONT_INFO* font = config->get_font_info(config, "Control");
		if (font && font->name) {
			g_font_name = font->name;
			if (font->size > 0) g_font_size = font->size;
		}
		int height = config->get_layout_size(config, "ListItemHeight");
		if (height > 0) g_item_height = height;
	}
	load_zoom();
	apply_zoom();
	// 戻している途中の帯と重なるので、一覧は兄弟ウィンドウの上に描かないようにする
	// (クリックはサブクラスで自分で処理するので、選択の通知(LBS_NOTIFY)は使わない)
	g_list = CreateWindowEx(0, WC_LISTBOX, L"",
		WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_CLIPSIBLINGS | LBS_OWNERDRAWFIXED | LBS_NOINTEGRALHEIGHT,
		0, 0, 100, 100, g_hwnd, (HMENU)IDC_LIST, GetModuleHandle(0), nullptr);
	if (g_list) SetWindowSubclass(g_list, list_subclass_proc, 1, 0);
	WNDCLASSEXW busy_class = {};
	busy_class.cbSize = sizeof(WNDCLASSEX);
	busy_class.lpszClassName = BUSY_CLASS_NAME;
	busy_class.lpfnWndProc = busy_proc;
	busy_class.hInstance = GetModuleHandle(0);
	busy_class.hCursor = LoadCursor(nullptr, IDC_WAIT);
	if (RegisterClassEx(&busy_class)) {
		g_busy = CreateWindowEx(0, BUSY_CLASS_NAME, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 100, 20, g_hwnd, nullptr, GetModuleHandle(0), nullptr);
	}

	host->register_window_client(PLUGIN_NAME, g_hwnd);
	edit_handle = host->create_edit_handle();
	host->register_event_listener(EVENT_TYPE::UPDATE_OBJECT, nullptr, on_object_updated);
	host->register_event_listener(EVENT_TYPE::CHANGE_EDIT_SCENE, nullptr, on_scene_changed);
	host->register_project_load_handler(on_project_load);
	// 初期化(フック・メニューの読み取り・状態の読み取り)は、起動時のプロジェクトの初期化の通知(on_project_load)を待ってから行う。
	// ここで自分宛てに投げておくと、起動の途中に出る確認ダイアログのメッセージループで処理されて本体が落ちる
}

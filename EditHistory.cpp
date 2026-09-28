//----------------------------------------------------------------------------------
//	EditHistory for AviUtl ExEdit2
//	編集履歴を一覧表示し、項目をクリックするとその時点まで本体の「元に戻す」「やり直し」で戻すプラグイン
//
//	SDKにはUndo履歴を扱うAPIが無いため、次の方法で本体のUndo履歴の写しを作る。
//	・UPDATE_OBJECTの通知ごとに状態を読み取り、状態のハッシュを記録する
//	・マウスの押下中の通知はまとめ、解放で1件に区切る(ドラッグ1回=本体のUndo 1件)
//	・Undo/Redoの向きは、ショートカットキー(キーボードフック)とメニュー(WM_COMMAND)の観測で知る
//	・行き先は、その向きでハッシュが一致する最も近い項目を探して決める
//	・「元に戻す」「やり直し」の有効・無効は、WM_INITMENUPOPUPを送ってからメニューを読む
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
#include <deque>
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <tuple>
#include <cmath>
#include <fstream>
#include <iterator>

#include "plugin2.h"
#include "logger2.h"
#include "config2.h"

#define PLUGIN_NAME			L"EditHistory"
#define WINDOW_CLASS_NAME	L"EditHistoryWindow"
#define BUSY_CLASS_NAME		L"EditHistoryBusy"
#define IDC_LIST			1001

// 自ウィンドウへ処理を委譲するメッセージ
#define WM_APP_OBJECT_UPDATED	(WM_APP + 1)	// lparam: 通知を受けた時刻(GetTickCount64の下位)
#define WM_APP_SCENE_CHANGED	(WM_APP + 2)
#define WM_APP_PROJECT_LOADED	(WM_APP + 3)
#define WM_APP_RELEASED			(WM_APP + 4)	// マウスのボタン解放後、本体が解放を処理し終えてから取るため
#define WM_APP_INIT				(WM_APP + 5)
#define WM_APP_JUMP_KEEP		(WM_APP + 6)	// クリックで戻している間、投げたメッセージを絶やさずユーザーの入力を後回しにする
#define WM_APP_GROUP_COMMAND	(WM_APP + 7)	// グループ化・グループ解除のコマンドを本体が処理し終えてから確かめるため

#define TIMER_ID_JUMP_TIMEOUT	1
#define TIMER_ID_BUSY			2		// 戻している途中の表示を回す
#define TIMER_ID_UNNOTIFIED		3		// 観測したUndo/Redoに通知が来ないか確かめる
#define UNNOTIFIED_WAIT_MS		200		// 観測したUndo/Redoに、この時間通知が来なければプロジェクトファイルを読んで確かめる
#define BUSY_INTERVAL_MS		80
#define BUSY_DOTS				8		// ぐるぐるマークの点の数
#define JUMP_TIMEOUT_MS			10000	// クリックで戻す途中、送ったコマンドの通知がこの時間来なければ中止する (重い編集では1回の処理に時間がかかる)
#define JUMP_KEEP_MAX_MS		3000	// 送ったコマンドの通知がこの時間来なければ、入力を後回しにするメッセージを投げるのをやめる
#define COMMAND_EXPIRE_MS		500		// 観測したUndo/Redoのコマンドは、この時間内に通知が来なければ捨てる
#define HISTORY_MAX				100		// 本体のUndoの上限と同じ
#define LABEL_VALUE_MAX			24		// ラベルに出す値の最大文字数
#define INPUT_KEY_WINDOW_MS		1000	// 数値欄への入力とみなす、キー入力から通知までの時間
#define INPUT_CLOSE_GRACE_MS	200		// 入力を区切った後も、確定の通知を同じ項目にまとめる時間

EDIT_HANDLE* edit_handle = nullptr;
LOG_HANDLE* logger = nullptr;
CONFIG_HANDLE* config = nullptr;
HWND g_hwnd = nullptr;
HWND g_list = nullptr;
HFONT g_font = nullptr;

COMMON_PLUGIN_TABLE common_plugin_table = {
	PLUGIN_NAME,
	L"EditHistory version 0.1 By teruyoshi",
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

// FNV-1a 64bit
uint64_t hash_bytes(const void* data, size_t size, uint64_t h = 1469598103934665603ULL) {
	const unsigned char* p = (const unsigned char*)data;
	for (size_t i = 0; i < size; i++) {
		h ^= p[i];
		h *= 1099511628211ULL;
	}
	return h;
}

//=======================================================================
//	状態 (シーンの全オブジェクトとレイヤー)
//=======================================================================

struct ObjState {
	OBJECT_LAYER_FRAME lf = {};
	std::string alias;
	uint64_t alias_hash = 0;
	int group = 0;	// オブジェクトのグループ化(Ctrl+G)の番号 (0:無し)。プロジェクトファイルからしか読めない
};

bool same_lf(const OBJECT_LAYER_FRAME& a, const OBJECT_LAYER_FRAME& b) {
	return a.layer == b.layer && a.start == b.start && a.end == b.end;
}

// レイヤーの状態 (名前は標準の名前なら空)
struct LayerState {
	std::wstring name;
	bool enable = true;
	bool lock = false;
	bool is_default() const { return name.empty() && enable && !lock; }
	bool operator==(const LayerState& o) const { return name == o.name && enable == o.enable && lock == o.lock; }
	bool operator!=(const LayerState& o) const { return !(*this == o); }
};

// レイヤー設定(座標・音量等)。プロジェクトファイルの[layer.N]セクションの項目 (標準と違う項目だけが書かれる)
using LayerSettings = std::map<std::string, std::string>;

struct State {
	std::map<OBJECT_HANDLE, ObjState> objects;
	std::map<int, LayerState> layers;	// 標準から変更されているレイヤーだけ
	std::map<int, LayerSettings> layer_settings;	// レイヤー設定のあるレイヤーだけ
};

// 状態全体のハッシュ。ハンドルが変わっても内容が同じなら同じ値になるよう、レイヤー・開始フレーム順に並べて計算する
uint64_t state_hash(const State& state) {
	std::vector<std::pair<std::pair<int, int>, const ObjState*>> sorted;
	for (auto& [h, o] : state.objects) sorted.push_back({ { o.lf.layer, o.lf.start }, &o });
	std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) { return a.first < b.first; });
	uint64_t hash = 1469598103934665603ULL;
	std::map<int, int> group_order;	// グループの番号は付け直されることがあるので、並び順で最初に現れた順の番号にする
	for (auto& [key, o] : sorted) {
		hash = hash_bytes(&o->lf, sizeof(o->lf), hash);
		hash = hash_bytes(&o->alias_hash, sizeof(o->alias_hash), hash);
		int group = 0;
		if (o->group) {
			auto it = group_order.find(o->group);
			group = it != group_order.end() ? it->second : (group_order[o->group] = (int)group_order.size() + 1);
		}
		hash = hash_bytes(&group, sizeof(group), hash);
	}
	for (auto& [layer, s] : state.layers) {
		hash = hash_bytes(&layer, sizeof(layer), hash);
		hash = hash_bytes(s.name.data(), s.name.size() * sizeof(wchar_t), hash);
		unsigned char flags = (s.enable ? 1 : 0) | (s.lock ? 2 : 0);
		hash = hash_bytes(&flags, sizeof(flags), hash);
	}
	for (auto& [layer, settings] : state.layer_settings) {
		hash = hash_bytes(&layer, sizeof(layer), hash);
		for (auto& [key, value] : settings) {
			std::string line = key + "=" + value + "\n";
			hash = hash_bytes(line.data(), line.size(), hash);
		}
	}
	return hash;
}

//=======================================================================
//	履歴 (本体のUndoスタックの写し、シーンごと)
//=======================================================================

struct Entry {
	uint64_t hash = 0;
	std::wstring label;
	std::vector<OBJECT_HANDLE> objects;	// この編集で変わったオブジェクト (Undo/Redo時に読み直す対象)
	OBJECT_HANDLE text_object = nullptr;	// 文字列の項目への入力・時間制御の編集なら対象のオブジェクト (表示上まとめる)
	std::string text_item;					// 文字列の項目への入力なら「セクション名\x1fエフェクト名\x1f項目名」、
											// 時間制御の編集なら「\x1eセクション名\x1fエフェクト名\x1f項目名,…」
											// (同じ項目の入力・編集だけをまとめる)
											// 同じ数値項目の変更なら「\x1dセクション名\x1f項目名…」、位置の変更なら「\x1c移動」「\x1c長さ」
	// 1つのオブジェクトだけの変更なら、その変更前後の状態 (まとめた行の説明を、最初の変更前→最後の変更後で作り直すため)
	bool has_change = false;
	ObjState change_before, change_after;
	// 1つのレイヤーの1つのレイヤー設定だけの変更なら「レイヤーN 項目 」と変更前後の値 (まとめた行の説明を作り直すため)
	std::wstring setting_prefix, setting_before, setting_after;
	// 数値欄へのキーボード入力の途中なら、対象の設定項目と入力前の値 (確定まで同じ項目を上書きする)
	OBJECT_HANDLE input_object = nullptr;
	std::string input_effect, input_key, input_before;
	int input_session = -1;
	// 入力を始めた時に捨てた「やり直せる分」。入力は確定するまで本体の履歴に積まれず本体のやり直せる分も残っているので、
	// 入力した結果が入力前と同じ状態に戻った時に元へ戻す (本体ではEscもEnterと同じく入力中の値で確定する)
	std::vector<Entry> stashed_future;
};

struct SceneHistory {
	std::vector<Entry> entries;	// entries[0]は開始時点
	int pos = 0;
	State state;
	bool initialized = false;
};

std::map<int, SceneHistory> g_scenes;
int g_scene_id = -1;
bool g_initialized = false;

SceneHistory& current_history() { return g_scenes[g_scene_id]; }

void update_list();

//=======================================================================
//	エイリアスの差分からラベルを作る
//=======================================================================

// エイリアスを「セクション名 → (キー, 値)の並び」に分解する
struct AliasSection {
	std::string name;
	std::vector<std::pair<std::string, std::string>> items;
};

std::vector<AliasSection> parse_alias(const std::string& alias) {
	std::vector<AliasSection> sections;
	size_t pos = 0;
	while (pos < alias.size()) {
		size_t end = alias.find('\n', pos);
		if (end == std::string::npos) end = alias.size();
		std::string line = alias.substr(pos, end - pos);
		if (!line.empty() && line.back() == '\r') line.pop_back();
		pos = end + 1;
		if (line.empty()) continue;
		if (line.front() == '[' && line.back() == ']') {
			sections.push_back({ line.substr(1, line.size() - 2), {} });
		} else if (!sections.empty()) {
			size_t eq = line.find('=');
			if (eq != std::string::npos) sections.back().items.push_back({ line.substr(0, eq), line.substr(eq + 1) });
		}
	}
	return sections;
}

std::string section_value(const AliasSection& s, const char* key) {
	for (auto& [k, v] : s.items) if (k == key) return v;
	return "";
}

std::wstring shorten(const std::wstring& s) {
	std::wstring t;
	for (wchar_t c : s) t += (c == L'\n' || c == L'\r' || c == L'\t') ? L' ' : c;
	if (t.size() > LABEL_VALUE_MAX) t = t.substr(0, LABEL_VALUE_MAX) + L"…";
	return t;
}

// オブジェクトの表示名 (名前が付いていればその名前、無ければ最初のエフェクト名) と [Lレイヤー]
std::wstring object_title(const ObjState& o) {
	auto sections = parse_alias(o.alias);
	std::string name;
	for (auto& s : sections) {
		if (s.name == "Object") name = section_value(s, "name");
	}
	if (name.empty()) {
		for (auto& s : sections) {
			if (s.name.rfind("Object.", 0) == 0) { name = section_value(s, "effect.name"); break; }
		}
	}
	return utf8_to_wide(name.empty() ? "オブジェクト" : name) + L"[L" + std::to_wstring(o.lf.layer + 1) + L"]";
}

bool is_numeric_value(const std::string& v) {
	if (v.empty()) return false;
	for (char c : v) if (!((c >= '0' && c <= '9') || c == '.' || c == '-' || c == ',' || c == ' ')) return false;
	return true;
}

// トラックバーの設定値。エイリアスでは次の形になる (プロジェクトファイルで確認)
//   移動無し: 「100.00」
//   移動あり: 「開始値,(中間点の値…,)終了値,移動方法,設定」 例:「0.00,10000.00,直線移動,0」
//   時間制御等: 上の後ろに「|データ」 例:「0.00,16.63,直線移動(時間制御),0|0.238636,0.488024,…」
//   参照式: 設定の8のビットが立ち、「|」の直後が式 例:「0.00,移動無し,8|X*2+5」「440.50,451.50,直線移動,8|layer35.Z軸回転-角度」
//   (設定はビットの組で、4は移動方法の設定データあり。12=8+4では「…,12|式|移動方法の設定|時間制御のデータ」の順に並ぶ)
#define TRACK_PARAM_EXPRESSION	8

struct TrackValue {
	bool track = false;					// 移動方法が付いているか
	std::vector<std::string> values;	// 値の並び
	std::string move, extra;			// 移動方法、「|」の後ろ (参照式を除く)
	int param = 0;						// 設定 (ビットの組)
	bool has_expression = false;
	std::string expression;				// 参照式
};

TrackValue parse_track(const std::string& v) {
	TrackValue t;
	size_t bar = v.find('|');
	std::string main = v.substr(0, bar);
	if (bar != std::string::npos) t.extra = v.substr(bar + 1);
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
		// 数値で始まらない(文字列等)か、全て数値(移動無し)
		t.values = { main };
		return t;
	}
	t.track = true;
	t.values.assign(tokens.begin(), tokens.begin() + k);
	t.move = tokens[k];
	if (k + 1 < tokens.size()) t.param = atoi(tokens[k + 1].c_str());
	if ((t.param & TRACK_PARAM_EXPRESSION) && bar != std::string::npos) {
		t.has_expression = true;
		size_t next = t.extra.find('|');
		t.expression = t.extra.substr(0, next);
		t.extra = next == std::string::npos ? "" : t.extra.substr(next + 1);
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
		// 時間制御のカーブ等は内容まで追わない
		if (a.extra != b.extra && b.move.find("時間制御") != std::string::npos) return L" の時間制御を編集";
		return L" の移動方法の設定を変更";
	}
	return L" " + shorten(utf8_to_wide(old_value)) + L"→" + shorten(utf8_to_wide(value));
}

// エフェクトの設定項目の種別 (EFFECT_ITEM_TYPE_*、分からなければ0)。enum_effect_itemで調べた結果を覚えておく
int effect_item_type(const std::string& effect, const std::string& key) {
	static std::map<std::pair<std::string, std::string>, int> cache;
	auto cache_key = std::make_pair(effect, key);
	auto it = cache.find(cache_key);
	if (it != cache.end()) return it->second;
	struct Param { std::wstring name; int type; } param = { utf8_to_wide(key), 0 };
	if (edit_handle && !effect.empty()) {
		edit_handle->enum_effect_item(utf8_to_wide(effect).c_str(), &param, [](void* p, LPCWSTR name, int type) {
			auto* q = (Param*)p;
			if (q->name == name) q->type = type;
		});
	}
	cache[cache_key] = param.type;
	return param.type;
}

// 文字列の設定項目か (テキストオブジェクトのテキスト、スクリプトのテキスト・文字列項目、スクリプト制御等)
// 本体はこれらへの入力で1文字ずつUndoを積む (ユーザー確認済み)
bool is_text_item(const std::string& effect, const std::string& key) {
	if (effect == "テキスト" && key == "テキスト") return true;
	int type = effect_item_type(effect, key);
	return type == EDIT_HANDLE::EFFECT_ITEM_TYPE_TEXT || type == EDIT_HANDLE::EFFECT_ITEM_TYPE_STRING;
}

// [Object]セクションのレイヤー・フレーム以外の変化を説明する。キーはプロジェクトファイルで確認した:
//   clipping=1: クリッピング / clipping.upper=1: 上のオブジェクトでクリッピング (無い時は無効)
//   camera=0: カメラ制御の対象外 / group.control=0: グループ制御の対象外 (無い時は対象)
//   name=: オブジェクト名
std::vector<std::wstring> describe_object_section_change(const AliasSection& a, const AliasSection& b) {
	struct Flag { const char* key; const char* fallback; const wchar_t* on; const wchar_t* off; };
	static const Flag flags[] = {
		{ "clipping", "0", L"のクリッピングを有効化", L"のクリッピングを解除" },
		{ "clipping.upper", "0", L"の上のオブジェクトでクリッピングを有効化", L"の上のオブジェクトでクリッピングを解除" },
		{ "camera", "1", L"をカメラ制御の対象にする", L"をカメラ制御の対象から外す" },
		{ "group.control", "1", L"をグループ制御の対象にする", L"をグループ制御の対象から外す" },
	};
	auto value_of = [](const AliasSection& s, const std::string& key, bool& found) -> std::string {
		for (auto& [k, v] : s.items) if (k == key) { found = true; return v; }
		found = false;
		return "";
	};
	std::vector<std::string> keys;
	for (auto* s : { &a, &b }) {
		for (auto& [k, v] : s->items) {
			if (k == "layer" || k == "frame" || k == "focus") continue;
			if (std::find(keys.begin(), keys.end(), k) == keys.end()) keys.push_back(k);
		}
	}
	std::vector<std::wstring> texts;
	for (auto& key : keys) {
		bool found_a = false, found_b = false;
		std::string va = value_of(a, key, found_a), vb = value_of(b, key, found_b);
		const Flag* flag = nullptr;
		for (auto& f : flags) if (key == f.key) flag = &f;
		if (flag) {
			if (!found_a) va = flag->fallback;
			if (!found_b) vb = flag->fallback;
			if (va != vb) texts.push_back(vb != "0" ? flag->on : flag->off);
		} else if (key == "name") {
			if (va != vb) {
				texts.push_back(L"の名前を変更 (" + (va.empty() ? L"なし" : shorten(utf8_to_wide(va))) + L"→" +
					(vb.empty() ? L"なし" : shorten(utf8_to_wide(vb))) + L")");
			}
		} else if (found_a != found_b || va != vb) {
			// 未確認のキーはそのまま出す
			texts.push_back(utf8_to_wide(key) + L" " + (found_a ? shorten(utf8_to_wide(va)) : L"なし") + L"→" +
				(found_b ? shorten(utf8_to_wide(vb)) : L"なし"));
		}
	}
	return texts;
}

// 並びのi番目のエフェクトの表示名。同じエフェクトが複数ある時は「ぼかし[2]」のように上から何番目か(1から)を付ける
std::string effect_display_name(const std::vector<std::string>& names, size_t i) {
	int total = 0, order = 0;
	for (size_t j = 0; j < names.size(); j++) {
		if (names[j] != names[i]) continue;
		total++;
		if (j <= i) order++;
	}
	return total > 1 ? names[i] + "[" + std::to_string(order) + "]" : names[i];
}

// セクション名 → 表示用のエフェクト名
std::map<std::string, std::string> effect_display_names(const std::vector<AliasSection>& sections) {
	std::vector<std::string> names, section_names;
	for (auto& s : sections) {
		if (s.name == "Object") continue;
		names.push_back(section_value(s, "effect.name"));
		section_names.push_back(s.name);
	}
	std::map<std::string, std::string> result;
	for (size_t i = 0; i < names.size(); i++) result[section_names[i]] = effect_display_name(names, i);
	return result;
}

// エフェクトの順番の変更・追加・削除を説明する (いずれでもなければ空)。
// セクション名は並び順の番号なので、順番を変えると同じ番号のセクションのエフェクトが入れ替わって見える
std::wstring describe_effect_reorder(const std::vector<AliasSection>& a, const std::vector<AliasSection>& b) {
	std::vector<std::string> na, nb;
	for (auto& s : a) if (s.name != "Object") na.push_back(section_value(s, "effect.name"));
	for (auto& s : b) if (s.name != "Object") nb.push_back(section_value(s, "effect.name"));
	// 途中のエフェクトの追加・削除も、それより下のセクションの番号がずれて別のエフェクトに変わったように見えるので、並びで見分ける
	// (多い方の並びから1つ抜くと少ない方と一致すれば、それを追加・削除したとみなす)
	if (na.size() + 1 == nb.size() || nb.size() + 1 == na.size()) {
		const auto& longer = na.size() > nb.size() ? na : nb;
		const auto& shorter = na.size() > nb.size() ? nb : na;
		for (size_t i = 0; i < longer.size(); i++) {
			std::vector<std::string> r = longer;
			r.erase(r.begin() + i);
			if (r == shorter) return utf8_to_wide(effect_display_name(longer, i)) + (nb.size() > na.size() ? L" を追加" : L" を削除");
		}
		return L"";
	}
	if (na.size() != nb.size() || na == nb) return L"";
	std::vector<std::string> sa = na, sb = nb;
	std::sort(sa.begin(), sa.end());
	std::sort(sb.begin(), sb.end());
	if (sa != sb) return L"";
	// 動かしたエフェクトの候補: 前後の並びから1つずつ抜くと残りが一致するもの (変更前の並びの位置)
	std::vector<size_t> moved;
	for (size_t i = 0; i < na.size(); i++) {
		std::vector<std::string> ra = na;
		ra.erase(ra.begin() + i);
		for (size_t j = 0; j < nb.size(); j++) {
			if (nb[j] != na[i]) continue;
			std::vector<std::string> rb = nb;
			rb.erase(rb.begin() + j);
			if (ra == rb) { moved.push_back(i); break; }
		}
	}
	if (moved.size() == 1) return utf8_to_wide(effect_display_name(na, moved[0])) + L" の順番を変更";
	// 隣どうしの入れ替えは、どちらを動かしたか分からない
	if (moved.size() == 2) {
		return utf8_to_wide(effect_display_name(na, moved[0])) + L" と " + utf8_to_wide(effect_display_name(na, moved[1])) + L" を入れ替え";
	}
	return L"エフェクトの順番を変更";
}

// 同じオブジェクトの変更前後のエイリアスを比べ、変わった設定項目を1つ説明する。
// text_item: 変わったのが1つの文字列の項目だけなら「セクション名\x1fエフェクト名\x1f項目名」、それ以外は空
std::wstring describe_content_change(const ObjState& before, const ObjState& after, std::string& text_item) {
	text_item.clear();
	auto a = parse_alias(before.alias);
	auto b = parse_alias(after.alias);
	std::wstring reorder = describe_effect_reorder(a, b);
	if (!reorder.empty()) return reorder;
	// 表示用のエフェクト名 (同じエフェクトが複数ある時は「ぼかし[2]」)。まとめる判定にはセクション名(並び順の番号)も使う
	auto names_a = effect_display_names(a);
	auto names_b = effect_display_names(b);
	std::vector<std::wstring> changes;
	bool only_text = true;
	int text_count = 0;
	std::string text_candidate;
	int lock_on = 0, lock_off = 0;	// ロックだけが変わったエフェクトの数
	int hide_on = 0, hide_off = 0;	// 表示/非表示(effect.disable)だけが変わったエフェクトの数
	bool hide_first = false;		// 最初のエフェクト(オブジェクト本体)の表示/非表示が変わったか
	int effect_index = 0;			// 何番目のエフェクトか (新しく追加されたもの・種類が変わったものを除く)
	int timectl_count = 0;			// 時間制御の編集だけが変わったエフェクトの数
	std::string timectl_keys, timectl_candidate;
	for (size_t i = 0; i < b.size(); i++) {
		const AliasSection* old_section = nullptr;
		for (auto& s : a) if (s.name == b[i].name) { old_section = &s; break; }
		if (!old_section) {
			std::string effect = names_b[b[i].name];
			changes.push_back(utf8_to_wide(effect.empty() ? b[i].name : effect) + L" を追加");
			only_text = false;
			continue;
		}
		if (b[i].name == "Object") {
			// レイヤー・フレームは位置の変化として別に扱う。それ以外(クリッピング・名前等)をここで説明する
			for (auto& text : describe_object_section_change(*old_section, b[i])) changes.push_back(text);
			if (!changes.empty()) only_text = false;
			continue;
		}
		std::string effect = section_value(b[i], "effect.name");
		std::string old_effect = section_value(*old_section, "effect.name");
		std::string label = names_b[b[i].name];	// 表示用の名前
		if (effect != old_effect) {
			changes.push_back(utf8_to_wide(names_a[old_section->name]) + L" を " + utf8_to_wide(label) + L" に変更");
			only_text = false;
			continue;
		}
		std::vector<std::pair<std::wstring, std::wstring>> items;	// このエフェクトで変わった項目名と説明 (プレビューでX,Yを同時に動かした等)
		// 変わった項目 (項目名, 変更前, 変更後)。ロック等は無効にするとキーごと消えるので、変更前にだけある項目も含める
		std::vector<std::tuple<std::string, std::string, std::string>> item_changes;
		for (auto& [key, value] : b[i].items) {
			std::string old_value;
			bool found = false;
			for (auto& [k, v] : old_section->items) if (k == key) { old_value = v; found = true; break; }
			if (!found || old_value != value) item_changes.push_back({ key, old_value, value });
		}
		for (auto& [key, old_value] : old_section->items) {
			bool exists = false;
			for (auto& [k, v] : b[i].items) if (k == key) { exists = true; break; }
			if (!exists) item_changes.push_back({ key, old_value, "" });
		}
		if (item_changes.size() == 1 && std::get<0>(item_changes[0]) == "display.lock") {
			const std::string& v = std::get<2>(item_changes[0]);
			((!v.empty() && v != "0") ? lock_on : lock_off)++;
		}
		if (item_changes.size() == 1 && std::get<0>(item_changes[0]) == "effect.disable") {
			const std::string& v = std::get<2>(item_changes[0]);
			((!v.empty() && v != "0") ? hide_on : hide_off)++;
			if (effect_index == 0) hide_first = true;
		}
		effect_index++;
		for (auto& [key, old_value, value] : item_changes) {
			if (key == "display.lock") {
				// エフェクト(オブジェクトの最初のエフェクトならオブジェクト)のロック。無効の時はキーが無い
				items.push_back({ L"", (!value.empty() && value != "0") ? L"をロック" : L"のロックを解除" });
				only_text = false;
			} else if (key == "effect.disable") {
				// エフェクトのチェックを外す(非表示)と1、付ける(表示)とキーが無くなる
				items.push_back({ L"", (!value.empty() && value != "0") ? L"を非表示" : L"を表示" });
				only_text = false;
			} else if (is_text_item(effect, key)) {
				// 入力後の内容を出す (1文字ずつの項目を表示上まとめた行でも、最後の項目のラベルがそのまま使える)
				std::wstring item = effect == "テキスト" && key == "テキスト" ? L"テキスト" :
					(label.empty() ? L"" : utf8_to_wide(label) + L" ") + utf8_to_wide(key) + L" ";
				changes.push_back(item + L"入力 (" + shorten(utf8_to_wide(value)) + L")");
				text_count++;
				text_candidate = b[i].name + "\x1f" + effect + "\x1f" + key;
			} else {
				// 移動方法の適用・時間制御の編集等、値を含まない同じ説明が続く時は「X,Y,Z に 直線移動 を適用」とまとめる
				// (X,Y,Zのようにグループ化された項目は同時に変わるため)
				std::wstring desc = describe_value_change(old_value, value);
				bool merged = false;
				if (desc.find(L'→') == std::wstring::npos) {
					for (auto& [keys, d] : items) {
						if (d == desc) { keys += L"," + utf8_to_wide(key); merged = true; break; }
					}
				}
				if (!merged) items.push_back({ utf8_to_wide(key), desc });
				only_text = false;
				if (desc == L" の時間制御を編集") timectl_keys += key + ",";
			}
		}
		// このエフェクトの変化が時間制御の編集だけなら、同じ項目の編集が続く行を表示上まとめるための候補にする
		if (items.size() == 1 && items[0].second == L" の時間制御を編集") {
			timectl_count++;
			timectl_candidate = "\x1e" + b[i].name + "\x1f" + effect + "\x1f" + timectl_keys;
		}
		timectl_keys.clear();
		if (!items.empty()) {
			std::wstring text = label.empty() ? L"" : utf8_to_wide(label) + L" ";
			for (size_t j = 0; j < items.size(); j++) text += (j ? L", " : L"") + items[j].first + items[j].second;
			changes.push_back(text);
		}
	}
	for (auto& s : a) {
		bool exists = false;
		for (auto& t : b) if (t.name == s.name) { exists = true; break; }
		if (!exists) {
			std::string effect = names_a[s.name];
			changes.push_back(utf8_to_wide(effect.empty() ? s.name : effect) + L" を削除");
			only_text = false;
		}
	}
	if (changes.empty()) return L"を変更";
	// オブジェクト自体のロックは、オブジェクト本体(最初のエフェクト)と標準描画等のdisplay.lockが一度に変わる
	// (追加エフェクトは変わらない。プロジェクトファイルで確認)。エフェクトを個別にロックする操作では1つしか変わらないので、
	// 2つ以上のエフェクトのロックだけが同じ向きに変わったら、オブジェクトのロックとして1件にする
	if (lock_on + lock_off >= 2 && (int)changes.size() == lock_on + lock_off && (lock_on == 0 || lock_off == 0)) {
		return lock_on ? L"をロック" : L"のロックを解除";
	}
	// オブジェクト本体(最初のエフェクト)の表示/非表示、または複数のエフェクトが一度に変わった場合はオブジェクトの表示/非表示とする
	if ((hide_first || hide_on + hide_off >= 2) && (int)changes.size() == hide_on + hide_off && (hide_on == 0 || hide_off == 0)) {
		return hide_on ? L"を非表示" : L"を表示";
	}
	if (only_text && text_count == 1 && changes.size() == 1) text_item = text_candidate;
	// 同じ項目の時間制御の編集が続く時も、テキストの入力と同じく表示上まとめる (本体は編集ごとにUndoを積む)
	if (timectl_count == 1 && changes.size() == 1) text_item = timectl_candidate;
	std::wstring text = changes[0];
	if (changes.size() > 1) text += L" 他" + std::to_wstring(changes.size() - 1) + L"件";
	return text;
}

//=======================================================================
//	状態の読み取り
//=======================================================================

struct ObjChange {
	OBJECT_HANDLE handle = nullptr;
	bool added = false, removed = false, moved = false, content = false;
	ObjState before, after;
};

struct LayerChange {
	int layer = 0;
	LayerState before, after;
};

struct LayerSettingChange {
	int layer = 0;
	LayerSettings before, after;
};

struct GroupChange {
	OBJECT_HANDLE handle = nullptr;
	int before = 0, after = 0;
};

struct Refresh {
	bool ok = false;
	std::vector<ObjChange> objects;
	std::vector<LayerChange> layers;
	std::vector<LayerSettingChange> settings;
	std::vector<GroupChange> groups;
	bool empty() const { return objects.empty() && layers.empty() && settings.empty() && groups.empty(); }
};

//-----------------------------------------------------------------------
//	プロジェクトファイルからしか読めないもの (レイヤー設定・オブジェクトのグループ化)
//	・レイヤー設定: SDKに読む関数が無く、各オブジェクトのエイリアスにも現れないが、変更すると本体のUndoが積まれUPDATE_OBJECTも来る
//	・グループ化(Ctrl+G): SDKに読む関数が無くエイリアスにも現れず、本体のUndoは積まれるがUPDATE_OBJECTも来ない
//	プロジェクトを一時ファイルに保存(自動バックアップと同じ処理)して[layer.N]セクションと各オブジェクトのgroup=を読む。
//	保存は重いので、一覧の右クリックのメニューで有効にした時だけ読む。読む時も、他で変化が見つからない時・全体を読み直す時・
//	レイヤーの状態が変わった時・オブジェクトが増えた時・グループ化のコマンドや通知の来ないUndo/Redoを観測した時に限る
//-----------------------------------------------------------------------

bool g_track_layer_settings = true;	// レイヤー設定・グループ化の変更も記録するか (EditHistory.iniに保存する)
// 変化の見つからない通知で、選択中・再生位置以外のオブジェクトの変更を探すために全オブジェクトを読み直すか (EditHistory.iniに保存する)。
// エイリアスの取得は1個約0.8msかかるので、オブジェクトが多いと編集が重くなる。
// 無効の時も、再生位置にあるオブジェクトは読み直す (プレビューでは選択していないオブジェクトもマウスで動かせるため)
bool g_track_other_objects = true;

// プロジェクトを一時ファイルに保存し、scene_idのシーンのレイヤー設定と、グループ化されたオブジェクトを読む (保存できなければfalse)
// groups: (レイヤー, 開始フレーム) → グループの番号
bool read_project_file(int scene_id, std::map<int, LayerSettings>& out, std::map<std::pair<int, int>, int>& groups) {
	wchar_t dir[MAX_PATH];
	DWORD length = GetTempPathW(MAX_PATH, dir);
	if (length == 0 || length >= MAX_PATH) return false;
	std::wstring path = std::wstring(dir) + L"EditHistory_" + std::to_wstring(GetCurrentProcessId()) + L".aup2";
	if (!edit_handle->save_project_file(path.c_str())) return false;
	std::string data;
	{
		std::ifstream file(path, std::ios::binary);
		data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
	}
	DeleteFileW(path.c_str());

	// [scene.N]のscene=でシーンを見分け、そのシーンの[layer.N]と[オブジェクトの番号]を読む。
	// [layer.N]のNは並び順の番号の可能性があるので、レイヤー番号は中のlayer=から取る。
	// オブジェクトの[N]にはlayer=・frame=開始,(中間点,)終了・group=が書かれる
	out.clear();
	groups.clear();
	bool in_scene_header = false, in_scene = false, in_layer = false, in_object = false;
	int layer = -1, start = -1, group = 0;
	LayerSettings settings;
	auto commit = [&]() {
		if (in_layer && layer >= 0 && !settings.empty()) out[layer] = settings;
		if (in_object && layer >= 0 && start >= 0 && group) groups[{ layer, start }] = group;
		in_layer = in_object = false;
		layer = start = -1;
		group = 0;
		settings.clear();
	};
	size_t pos = 0;
	while (pos < data.size()) {
		size_t end = data.find('\n', pos);
		if (end == std::string::npos) end = data.size();
		std::string line = data.substr(pos, end - pos);
		pos = end + 1;
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (line.empty()) continue;
		if (line[0] == '[') {
			commit();
			in_scene_header = line.rfind("[scene.", 0) == 0;
			if (in_scene_header) in_scene = false;
			in_layer = in_scene && line.rfind("[layer.", 0) == 0;
			in_object = in_scene && line.size() > 2 && line.back() == ']' &&
				line.find_first_not_of("0123456789", 1) == line.size() - 1;
			continue;
		}
		size_t eq = line.find('=');
		if (eq == std::string::npos) continue;
		std::string key = line.substr(0, eq), value = line.substr(eq + 1);
		if (in_scene_header && key == "scene") in_scene = atoi(value.c_str()) == scene_id;
		else if ((in_layer || in_object) && key == "layer") layer = atoi(value.c_str());
		else if (in_layer) settings[key] = value;
		else if (in_object && key == "frame") start = atoi(value.c_str());
		else if (in_object && key == "group") group = atoi(value.c_str());
	}
	commit();
	return true;
}

// レイヤー設定とグループ化を読み直し、変わったものをrに足す (記録しない設定の時・保存できない時は何もしない)
void refresh_project_file(SceneHistory& sh, Refresh& r) {
	if (!g_track_layer_settings) return;
	std::map<int, LayerSettings> now;
	std::map<std::pair<int, int>, int> groups;
	if (!read_project_file(g_scene_id, now, groups)) return;
	for (auto& [handle, o] : sh.state.objects) {
		auto it = groups.find({ o.lf.layer, o.lf.start });
		int group = it != groups.end() ? it->second : 0;
		if (group != o.group) r.groups.push_back({ handle, o.group, group });
		o.group = group;
	}
	std::set<int> layers;
	for (auto& [layer, settings] : sh.state.layer_settings) layers.insert(layer);
	for (auto& [layer, settings] : now) layers.insert(layer);
	for (int layer : layers) {
		auto a = sh.state.layer_settings.find(layer);
		auto b = now.find(layer);
		LayerSettings before = a != sh.state.layer_settings.end() ? a->second : LayerSettings();
		LayerSettings after = b != now.end() ? b->second : LayerSettings();
		if (before != after) r.settings.push_back({ layer, before, after });
	}
	sh.state.layer_settings = std::move(now);
}

// レイヤー設定の項目名の表示
std::wstring layer_setting_name(const std::string& key) {
	if (key == "x") return L"X";
	if (key == "y") return L"Y";
	if (key == "z") return L"Z";
	if (key == "volume") return L"音量";
	if (key == "balance") return L"左右";
	return utf8_to_wide(key);
}

// レイヤー設定の値の表示 (書かれていなければ標準の値。数値は末尾の0を省く)
std::wstring layer_setting_value(const LayerSettings& settings, const std::string& key) {
	auto it = settings.find(key);
	if (it == settings.end()) return L"標準";
	std::string value = it->second;
	char* end = nullptr;
	strtod(value.c_str(), &end);
	if (!value.empty() && end && *end == '\0' && value.find('.') != std::string::npos) {
		while (value.back() == '0') value.pop_back();
		if (value.back() == '.') value.pop_back();
	}
	return shorten(utf8_to_wide(value));
}

// エイリアスから選択状態(focus=)の行を除く。プロジェクトファイルでは[Object]セクションに選択中を表すfocus=1が入るため、
// エイリアスにも入っていると選択が移っただけで変更と数えられ、状態のハッシュもずれてしまう
std::string strip_focus(const std::string& alias) {
	std::string out;
	out.reserve(alias.size());
	size_t pos = 0;
	while (pos < alias.size()) {
		size_t end = alias.find('\n', pos);
		if (end == std::string::npos) end = alias.size();
		if (alias.compare(pos, 6, "focus=") != 0) out.append(alias, pos, std::min(end + 1, alias.size()) - pos);
		pos = end + 1;
	}
	return out;
}

// 状態を読み取ってキャッシュを更新し、変わったものを返す。
// エイリアスの取得は1個約0.8msと重いので、full以外では次のオブジェクトだけ取り直す:
//	位置が変わった・新しく現れたオブジェクト / 選択中のオブジェクト / candidatesに含まれるオブジェクト
Refresh refresh_state(SceneHistory& sh, const std::set<OBJECT_HANDLE>& candidates, bool full) {
	struct Param {
		SceneHistory* sh;
		const std::set<OBJECT_HANDLE>* candidates;
		bool full;
		EDIT_INFO info;
		Refresh result;
	} param = { &sh, &candidates, full, {}, {} };
	edit_handle->get_edit_info(&param.info, sizeof(param.info));

	param.result.ok = edit_handle->call_read_section_param(&param, [](void* p, EDIT_SECTION* edit) {
		auto* param = (Param*)p;
		State& state = param->sh->state;
		Refresh& result = param->result;

		std::set<OBJECT_HANDLE> selected;
		if (OBJECT_HANDLE focus = edit->get_focus_object()) selected.insert(focus);
		int num = edit->get_selected_object_num();
		for (int i = 0; i < num; i++) selected.insert(edit->get_selected_object(i));

		// 全オブジェクトの位置 (軽い)
		std::map<OBJECT_HANDLE, OBJECT_LAYER_FRAME> positions;
		for (int layer = 0; layer <= param->info.layer_max; layer++) {
			int frame = 0;
			while (OBJECT_HANDLE obj = edit->find_object(layer, frame)) {
				OBJECT_LAYER_FRAME lf = edit->get_object_layer_frame(obj);
				positions[obj] = lf;
				if (lf.end + 1 <= frame) break;
				frame = lf.end + 1;
			}
		}

		for (auto& [h, lf] : positions) {
			auto it = state.objects.find(h);
			bool is_new = it == state.objects.end();
			bool moved = !is_new && !same_lf(it->second.lf, lf);
			bool need_alias = param->full || is_new || moved || selected.count(h) || param->candidates->count(h);
			if (!need_alias) continue;
			ObjState now;
			now.lf = lf;
			if (!is_new) now.group = it->second.group;	// グループはプロジェクトファイルからしか読めないので引き継ぐ
			LPCSTR alias = edit->get_object_alias(h);
			now.alias = strip_focus(alias ? alias : "");
			now.alias_hash = hash_bytes(now.alias.data(), now.alias.size());
			if (is_new) {
				ObjChange c;
				c.handle = h;
				c.added = true;
				c.after = now;
				result.objects.push_back(c);
			} else if (moved || it->second.alias_hash != now.alias_hash) {
				ObjChange c;
				c.handle = h;
				c.moved = moved;
				c.content = it->second.alias_hash != now.alias_hash;
				c.before = it->second;
				c.after = now;
				result.objects.push_back(c);
			}
			state.objects[h] = now;
		}
		for (auto it = state.objects.begin(); it != state.objects.end();) {
			if (!positions.count(it->first)) {
				ObjChange c;
				c.handle = it->first;
				c.removed = true;
				c.before = it->second;
				result.objects.push_back(c);
				it = state.objects.erase(it);
			} else {
				++it;
			}
		}

		// レイヤー: 名前とロックは実体が無くても標準の値が返るので広く読み、
		// 表示・非表示は実体の無いレイヤーで非表示と返るので、オブジェクトのある範囲と表示範囲だけ読む
		int narrow_max = std::max(param->info.layer_max, param->info.display_layer_start + param->info.display_layer_num - 1);
		int wide_max = narrow_max;
		if (!state.layers.empty()) wide_max = std::max(wide_max, state.layers.rbegin()->first);
		for (int layer = 0; layer <= wide_max; layer++) {
			LayerState now;
			LPCWSTR name = edit->get_layer_name(layer);
			if (name) now.name = name;
			now.lock = edit->get_layer_lock(layer);
			auto it = state.layers.find(layer);
			LayerState before = it != state.layers.end() ? it->second : LayerState();
			now.enable = layer <= narrow_max ? edit->get_layer_enable(layer) : before.enable;
			if (now != before) result.layers.push_back({ layer, before, now });
			if (now.is_default()) state.layers.erase(layer);
			else state.layers[layer] = now;
		}
	});
	// プロジェクトファイルは保存して読むので重い。全体を読み直す時・レイヤーの状態が変わった時・オブジェクトが増えた時だけ読む
	// (名前等が[layer.N]にも書かれる場合や、グループ化されたオブジェクトの貼り付け等で、写しが古いまま残らないようにする)
	bool added = false;
	for (auto& c : param.result.objects) if (c.added) added = true;
	if (param.result.ok && (full || added || !param.result.layers.empty())) refresh_project_file(sh, param.result);
	return param.result;
}


// 1つのオブジェクトの1つの数値の設定項目だけが変わったか (数値欄へのキーボード入力の途中の通知を見分けるため)
bool single_numeric_change(const Refresh& r, OBJECT_HANDLE& handle, std::string& effect, std::string& key,
	std::string& before, std::string& after) {
	if (r.objects.size() != 1 || !r.layers.empty()) return false;
	const ObjChange& c = r.objects[0];
	if (c.added || c.removed || c.moved || !c.content) return false;
	auto a = parse_alias(c.before.alias);
	auto b = parse_alias(c.after.alias);
	if (a.size() != b.size()) return false;
	int count = 0;
	std::string section;
	for (size_t i = 0; i < b.size(); i++) {
		if (a[i].name != b[i].name || a[i].items.size() != b[i].items.size()) return false;
		for (size_t j = 0; j < b[i].items.size(); j++) {
			if (a[i].items[j].first != b[i].items[j].first) return false;
			if (a[i].items[j].second == b[i].items[j].second) continue;
			if (b[i].name == "Object") return false;
			count++;
			section = b[i].name;
			effect = section_value(b[i], "effect.name");
			key = b[i].items[j].first;
			before = a[i].items[j].second;
			after = b[i].items[j].second;
		}
	}
	if (count != 1 || is_text_item(effect, key)) return false;	// 文字列の項目は本体が1文字ずつUndoを積むので対象外
	// 表示用の名前にする (同じエフェクトが複数ある時は「ぼかし[2]」になり、入力の続きかの判定でも区別される)
	effect = effect_display_names(b)[section];
	TrackValue tb = parse_track(before), ta = parse_track(after);
	if (tb.track || ta.track) {
		if (!tb.track || !ta.track || tb.move != ta.move || tb.param != ta.param || tb.extra != ta.extra || tb.expression != ta.expression ||
			tb.values.size() != ta.values.size()) return false;
		int diff = 0;
		for (size_t j = 0; j < ta.values.size(); j++) if (tb.values[j] != ta.values[j]) diff++;
		if (diff != 1) return false;
	} else if (!is_numeric_value(before) || !is_numeric_value(after)) {
		return false;
	}
	handle = c.handle;
	return true;
}

// 変わったものから、ラベル・対象オブジェクト・テキスト入力かどうかを作る
// フレーム番号を時刻の文字列にする (60秒未満は「1.23秒」、以上は「1:05.20」)
int g_rate = 60, g_scale = 1;	// ラベルを作る時点のシーンのフレームレート

std::wstring time_text(int frame) {
	double sec = g_rate > 0 ? (double)frame * g_scale / g_rate : 0;
	wchar_t buf[32];
	if (sec < 60) swprintf_s(buf, L"%.2f秒", sec);
	else swprintf_s(buf, L"%d:%05.2f", (int)(sec / 60), sec - (int)(sec / 60) * 60);
	return buf;
}

// エイリアスの[Object]セクションのframe (開始,中間点…,終了) を、オブジェクトの開始フレーム基準の絶対フレームにして返す
std::vector<int> section_frames(const ObjState& o) {
	std::vector<int> frames;
	for (auto& s : parse_alias(o.alias)) {
		if (s.name != "Object") continue;
		std::string v = section_value(s, "frame");
		size_t pos = 0;
		while (pos < v.size()) {
			size_t comma = v.find(',', pos);
			if (comma == std::string::npos) comma = v.size();
			frames.push_back(atoi(v.substr(pos, comma - pos).c_str()));
			pos = comma + 1;
		}
	}
	if (!frames.empty()) {
		int base = frames[0];
		for (auto& f : frames) f = f - base + o.lf.start;
	}
	return frames;
}

// 中間点の変化を説明する (変化が無ければ空)
std::wstring describe_section_change(const ObjState& before, const ObjState& after) {
	auto a = section_frames(before);
	auto b = section_frames(after);
	if (a.size() < 2 || b.size() < 2) return L"";
	std::vector<int> ma(a.begin() + 1, a.end() - 1), mb(b.begin() + 1, b.end() - 1);
	if (mb.size() > ma.size()) {
		for (int f : mb) if (std::find(ma.begin(), ma.end(), f) == ma.end()) return L"中間点を追加 (" + time_text(f) + L")";
		return L"中間点を追加";
	}
	if (mb.size() < ma.size()) {
		for (int f : ma) if (std::find(mb.begin(), mb.end(), f) == mb.end()) return L"中間点を削除 (" + time_text(f) + L")";
		return L"中間点を削除";
	}
	// 位置の変化(移動・長さの変更)で全体がずれた場合は中間点の移動とはみなさない
	int shift = after.lf.start - before.lf.start;
	for (size_t i = 0; i < ma.size(); i++) {
		if (mb[i] != ma[i] && mb[i] != ma[i] + shift) {
			return L"中間点を移動 (" + time_text(ma[i]) + L"→" + time_text(mb[i]) + L")";
		}
	}
	return L"";
}

// 位置の変化(移動・レイヤー移動・長さの変更)を説明する (変化が無ければ空)
std::wstring describe_position_change(const ObjState& before, const ObjState& after) {
	const OBJECT_LAYER_FRAME& a = before.lf;
	const OBJECT_LAYER_FRAME& b = after.lf;
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

// 1つのオブジェクトの変化を説明する。text_item: 変わったのが1つの文字列の項目だけなら「エフェクト名\x1f項目名」
std::wstring describe_object_change(const ObjChange& c, std::string& text_item) {
	text_item.clear();
	if (c.added) return object_title(c.after) + L" を追加 (" + time_text(c.after.lf.start) + L")";
	if (c.removed) return object_title(c.before) + L" を削除";
	std::wstring title = object_title(c.before);
	std::wstring position = describe_position_change(c.before, c.after);
	std::wstring section = describe_section_change(c.before, c.after);
	if (!section.empty()) {
		// 中間点の追加・削除では各設定項目の値の並びも変わるので、設定の変更としては出さない
		return title + L" " + section + (position.empty() ? L"" : L" / " + position);
	}
	std::wstring content;
	if (c.content) {
		std::string item;
		content = describe_content_change(c.before, c.after, item);
		if (content == L"を変更") content.clear();	// [Object]セクション(位置)だけの変化
		if (position.empty()) text_item = item;
	}
	if (!position.empty() && !content.empty()) return title + L" " + position + L" / " + content;
	if (!position.empty()) return title + L" " + position;
	if (!content.empty()) return title + L" " + content;
	return title + L" を変更";
}

// 設定項目がグループ(X,Y,Z等)に属していれば、グループの最初の項目名を返す (属していなければ項目名)。結果は覚えておく
std::string item_group_head(const std::string& effect, const std::string& key) {
	static std::map<std::pair<std::string, std::string>, std::string> cache;
	auto cache_key = std::make_pair(effect, key);
	auto it = cache.find(cache_key);
	if (it != cache.end()) return it->second;
	std::string head = key;
	if (edit_handle && !effect.empty()) {
		LPCWSTR names[32] = {};
		int n = edit_handle->get_effect_item_group_names(utf8_to_wide(effect).c_str(), utf8_to_wide(key).c_str(), names, 32, nullptr);
		if (n > 0 && names[0]) {
			int size = WideCharToMultiByte(CP_UTF8, 0, names[0], -1, nullptr, 0, nullptr, nullptr);
			if (size > 1) {
				std::string text(size - 1, '\0');
				WideCharToMultiByte(CP_UTF8, 0, names[0], -1, text.data(), size, nullptr, nullptr);
				head = text;
			}
		}
	}
	cache[cache_key] = head;
	return head;
}

// 同じパラメータの連続した変更を表示上まとめるための識別子 (まとめない変更なら空)
//   位置だけの変更: 「\x1c移動」(長さが変わらない) /「\x1c長さ」
//   数値項目の値だけの変更 (移動方法・参照式等は変わらない): 「\x1dセクション名\x1f項目名\x1e…」
std::string value_group_key(const ObjChange& c) {
	if (c.added || c.removed) return "";
	if (c.moved) {
		if (c.content) {
			std::string item;
			if (describe_content_change(c.before, c.after, item) != L"を変更") return "";	// 位置以外も変わった
		}
		bool same_length = (c.before.lf.end - c.before.lf.start) == (c.after.lf.end - c.after.lf.start);
		return same_length ? "\x1c移動" : "\x1c長さ";
	}
	if (!c.content) return "";
	auto a = parse_alias(c.before.alias);
	auto b = parse_alias(c.after.alias);
	if (a.size() != b.size()) return "";
	std::string key;
	std::set<std::string> groups;	// 変わった項目 (セクション名\x1fグループの最初の項目名)
	for (size_t i = 0; i < b.size(); i++) {
		if (a[i].name != b[i].name) return "";
		std::string effect = section_value(b[i], "effect.name");
		if (effect != section_value(a[i], "effect.name")) return "";
		if (a[i].items.size() != b[i].items.size()) return "";
		for (size_t j = 0; j < b[i].items.size(); j++) {
			const auto& [k, v] = b[i].items[j];
			if (a[i].items[j].first != k) return "";
			const std::string& old_value = a[i].items[j].second;
			if (old_value == v) continue;
			if (b[i].name == "Object") return "";	// クリッピング・名前等
			if (is_text_item(effect, k)) return "";
			TrackValue ta = parse_track(old_value), tb = parse_track(v);
			if (ta.track != tb.track) return "";
			if (ta.track) {
				if (ta.move != tb.move || ta.param != tb.param || ta.extra != tb.extra || ta.expression != tb.expression ||
					ta.values.size() != tb.values.size()) return "";
			} else if (!is_numeric_value(old_value) || !is_numeric_value(v)) {
				return "";
			}
			// グループ(X,Y,Z等)の項目はグループで数える (「X,Yの変更」と「Xだけの変更」を同じパラメータとしてまとめる)
			groups.insert(b[i].name + "\x1f" + item_group_head(effect, k));
		}
	}
	for (auto& g : groups) key += g + "\x1e";
	return key.empty() ? "" : "\x1d" + key;
}

// 変わったものから、ラベル・対象オブジェクト・テキスト入力かどうかを作る
void make_entry_info(const Refresh& r, Entry& e) {
	EDIT_INFO info = {};
	edit_handle->get_edit_info(&info, sizeof(info));
	if (info.rate > 0) { g_rate = info.rate; g_scale = info.scale; }

	std::vector<std::wstring> texts;
	for (auto& c : r.objects) e.objects.push_back(c.handle);

	// 分割: 同じレイヤーで、変化の前の区間 [s, e] が、変化の後の区間 [s, m-1] [m, e] の2つに分かれた
	// (本体が「元のオブジェクトを短くして後ろを追加する」形でも「削除して2つ作り直す」形でも見分けられるようにする)。
	// 複数のオブジェクトを選択して一度に分割した場合は、変化の前の区間の全てがそれぞれ2つに分かれている
	if (r.layers.empty()) {
		std::vector<const ObjState*> before, after;
		for (auto& c : r.objects) {
			if (c.removed) before.push_back(&c.before);
			else if (c.added) after.push_back(&c.after);
			else if (c.moved) { before.push_back(&c.before); after.push_back(&c.after); }
		}
		if (!before.empty() && after.size() == before.size() * 2) {
			std::vector<bool> used(after.size(), false);
			std::vector<int> points;	// 分割した位置 (後ろの区間の開始)
			for (const ObjState* b : before) {
				int first = -1, second = -1;
				for (size_t i = 0; i < after.size() && first < 0; i++) {
					const OBJECT_LAYER_FRAME& a = after[i]->lf;
					if (!used[i] && a.layer == b->lf.layer && a.start == b->lf.start && a.end < b->lf.end) first = (int)i;
				}
				if (first < 0) break;
				for (size_t i = 0; i < after.size() && second < 0; i++) {
					const OBJECT_LAYER_FRAME& a = after[i]->lf;
					if (!used[i] && (int)i != first && a.layer == b->lf.layer && a.start == after[first]->lf.end + 1 &&
						a.end == b->lf.end) second = (int)i;
				}
				if (second < 0) break;
				used[first] = used[second] = true;
				points.push_back(after[second]->lf.start);
			}
			if (points.size() == before.size()) {
				bool same_point = std::all_of(points.begin(), points.end(), [&](int f) { return f == points[0]; });
				std::wstring at = same_point ? L" (" + time_text(points[0]) + L")" : L"";
				if (before.size() == 1) e.label = object_title(*before[0]) + L" を分割" + at;
				else e.label = std::to_wstring(before.size()) + L"個のオブジェクトを分割" + at;
				return;
			}
		}
	}

	// 複数のオブジェクトをまとめて移動・長さ変更: 全オブジェクトの変化が位置だけで、全て移動か全て長さの変更なら1件にする
	if (r.layers.empty() && r.objects.size() >= 2) {
		std::vector<const ObjChange*> list;
		bool ok = true;
		for (auto& c : r.objects) {
			if (c.added || c.removed || !c.moved) { ok = false; break; }
			if (c.content) {
				std::string item;
				if (describe_content_change(c.before, c.after, item) != L"を変更") { ok = false; break; }	// 位置以外も変わった
			}
			list.push_back(&c);
		}
		if (ok) {
			std::sort(list.begin(), list.end(), [](const ObjChange* x, const ObjChange* y) {
				return x->before.lf.layer != y->before.lf.layer ? x->before.lf.layer < y->before.lf.layer : x->before.lf.start < y->before.lf.start;
			});
			bool any_layer = false, any_start = false, any_end = false, all_same_length = true;
			std::vector<int> layer_a, layer_b, start_a, start_b, end_a, end_b;
			for (auto* c : list) {
				const OBJECT_LAYER_FRAME& a = c->before.lf;
				const OBJECT_LAYER_FRAME& b = c->after.lf;
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
			std::wstring count = std::to_wstring(list.size()) + L"個のオブジェクト";
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
			if (!what.empty()) {
				std::wstring detail;
				for (auto& d : details) if (!d.empty()) detail += (detail.empty() ? L"" : L" / ") + d;
				e.label = count + what + (detail.empty() ? L"" : L" (" + detail + L")");
				return;
			}
		}
	}

	for (auto& c : r.objects) {
		std::string text_item;
		texts.push_back(describe_object_change(c, text_item));
		if (!text_item.empty() && r.objects.size() == 1 && r.layers.empty()) {
			e.text_object = c.handle;
			e.text_item = text_item;
		}
		if (r.objects.size() == 1 && r.layers.empty() && !c.added && !c.removed) {
			e.has_change = true;
			e.change_before = c.before;
			e.change_after = c.after;
			if (e.text_item.empty()) {
				std::string key = value_group_key(c);
				if (!key.empty()) {
					e.text_object = c.handle;
					e.text_item = key;
				}
			}
		}
	}
	for (auto& c : r.layers) {
		std::wstring what;
		if (c.before.name != c.after.name) what = L"名前「" + c.after.name + L"」";
		else if (c.before.enable != c.after.enable) what = c.after.enable ? L"表示" : L"非表示";
		else what = c.after.lock ? L"ロック" : L"ロック解除";
		texts.push_back(L"レイヤー" + std::to_wstring(c.layer + 1) + L" " + what);
	}
	if (!r.groups.empty()) {
		// 新しく現れたオブジェクトのグループは、オブジェクトの追加として説明しているので数えない
		int grouped = 0, ungrouped = 0;
		for (auto& g : r.groups) {
			bool added = false;
			for (auto& c : r.objects) if (c.handle == g.handle && c.added) added = true;
			if (added) continue;
			(g.after ? grouped : ungrouped)++;
		}
		if (grouped) texts.push_back(std::to_wstring(grouped) + L"個のオブジェクトをグループ化");
		else if (ungrouped) texts.push_back(std::to_wstring(ungrouped) + L"個のオブジェクトのグループを解除");
	}
	for (auto& c : r.settings) {
		// 名前・表示・ロックの変更と同じレイヤーなら、そちらで説明しているので出さない
		bool described = false;
		for (auto& l : r.layers) if (l.layer == c.layer) described = true;
		if (described) continue;
		std::set<std::string> keys;
		for (auto& [key, value] : c.before) keys.insert(key);
		for (auto& [key, value] : c.after) keys.insert(key);
		std::wstring items;
		std::string changed_key;
		int count = 0;
		for (auto& key : keys) {
			auto a = c.before.find(key);
			auto b = c.after.find(key);
			bool has_a = a != c.before.end(), has_b = b != c.after.end();
			if (has_a == has_b && (!has_a || a->second == b->second)) continue;
			items += (items.empty() ? L"" : L", ") + layer_setting_name(key) + L" " +
				layer_setting_value(c.before, key) + L"→" + layer_setting_value(c.after, key);
			changed_key = key;
			count++;
		}
		if (items.empty()) continue;
		std::wstring prefix = L"レイヤー" + std::to_wstring(c.layer + 1) + L" ";
		texts.push_back(prefix + items);
		// 同じレイヤーの同じ項目の変更が続く時は表示上まとめる (レイヤー番号をハンドルの代わりにする。項目の頭の\x1bで区別する)
		if (count == 1 && r.objects.empty() && r.layers.empty() && r.settings.size() == 1) {
			e.text_object = (OBJECT_HANDLE)(intptr_t)(c.layer + 1);
			e.text_item = "\x1b" + changed_key;
			e.setting_prefix = prefix + layer_setting_name(changed_key) + L" ";
			e.setting_before = layer_setting_value(c.before, changed_key);
			e.setting_after = layer_setting_value(c.after, changed_key);
		}
	}
	if (texts.empty()) {
		e.label = L"(内容を特定できない編集)";
		return;
	}
	e.label = texts[0];
	if (texts.size() > 1) e.label += L" 他" + std::to_wstring(texts.size() - 1) + L"件";
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

// メニューの項目名 (表示言語ごと。日本語・英語・韓国語・中国語)。先に並べた名前を優先して探す。
// グループ化・グループ解除は、タイムラインの右クリックのメニューの名前も含む (日本語・韓国語はメインメニューと名前が違うので、
// メインメニューの名前を先に並べる。メインメニューで見つからないとショートカットキーの割り当てを読めない)
const wchar_t* const UNDO_NAMES[] = { L"元に戻す", L"Undo", L"되돌리다", L"撤销", nullptr };
const wchar_t* const REDO_NAMES[] = { L"やり直し", L"やり直す", L"Redo", L"다시 하다", L"重做", nullptr };
const wchar_t* const GROUP_NAMES[] = { L"オブジェクトのグループ化", L"객체를 그룹화", L"グループ化", L"Group Object", L"그룹화", L"组合对象", nullptr };
const wchar_t* const UNGROUP_NAMES[] = { L"オブジェクトのグループ解除", L"객체의 그룹 해제", L"グループ解除", L"Ungroup Object", L"그룹 해제", L"取消组合对象", nullptr };

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

// 表示名がnamesのどれかで始まるか
bool is_menu_name_any(const std::wstring& text, const wchar_t* const* names) {
	for (; *names; names++) if (is_menu_name(text, *names)) return true;
	return false;
}

// メニューを再帰的に探し、表示名がnameの項目のIDを返す (見つからなければ0)
// IDはプラグインのメニュー登録数によってずれるため、固定値ではなく名前で探す
// parent: 見つかった項目を含むポップアップメニュー / parent_pos: そのポップアップの親メニュー内での位置
UINT find_menu_item(HMENU menu, const wchar_t* name, HMENU* parent, int* parent_pos, int menu_pos = -1) {
	int count = GetMenuItemCount(menu);
	for (int i = 0; i < count; i++) {
		MENUITEMINFOW mii = { sizeof(mii) };
		mii.fMask = MIIM_ID | MIIM_SUBMENU;
		if (!GetMenuItemInfoW(menu, i, TRUE, &mii)) continue;
		if (mii.hSubMenu) {
			if (UINT id = find_menu_item(mii.hSubMenu, name, parent, parent_pos, i)) return id;
		} else if (is_menu_name(get_menu_text(menu, i), name)) {
			if (parent) *parent = menu_pos >= 0 ? menu : nullptr;
			if (parent_pos) *parent_pos = menu_pos;
			return mii.wID;
		}
	}
	return 0;
}

// namesのどれかの項目を探す (先に並べた名前を優先する)。matched: 一致した名前
UINT find_menu_item_any(HMENU menu, const wchar_t* const* names, HMENU* parent, int* parent_pos, const wchar_t** matched = nullptr) {
	for (; *names; names++) {
		if (UINT id = find_menu_item(menu, *names, parent, parent_pos)) {
			if (matched) *matched = *names;
			return id;
		}
	}
	return 0;
}

UINT find_menu_id(const wchar_t* const* names) {
	HMENU menu = GetMenu(edit_handle->get_host_app_window());
	return menu ? find_menu_item_any(menu, names, nullptr, nullptr) : 0;
}

bool g_sending_initmenu = false;	// 自分が送ったWM_INITMENUPOPUPをサブクラスで区別するため

// メインメニューの項目の有効状態 (1:有効 / 0:無効 / -1:見つからない)
// 本体はメニューを開いた時にしか有効・無効を更新しないため、WM_INITMENUPOPUPを送って更新させてから読む
int get_menu_enabled(const wchar_t* const* names) {
	HWND main = edit_handle->get_host_app_window();
	HMENU menu = GetMenu(main);
	HMENU parent = nullptr;
	int parent_pos = 0;
	UINT id = menu ? find_menu_item_any(menu, names, &parent, &parent_pos) : 0;
	if (!id) return -1;
	if (parent) {
		g_sending_initmenu = true;
		SendMessageW(main, WM_INITMENUPOPUP, (WPARAM)parent, MAKELPARAM(parent_pos, FALSE));
		g_sending_initmenu = false;
	}
	return (GetMenuState(menu, id, MF_BYCOMMAND) & (MF_GRAYED | MF_DISABLED)) ? 0 : 1;
}

//=======================================================================
//	Undo/Redoコマンドの観測
//	ショートカットキー: メインスレッドのキーボードフック(割り当てはメニューの項目名から読み取る)
//	メニューからの実行: メインウィンドウのサブクラス化でWM_COMMANDを見る
//=======================================================================

enum Command { CMD_NONE, CMD_UNDO, CMD_REDO };

struct Shortcut {
	bool valid = false, ctrl = false, shift = false, alt = false;
	UINT vk = 0;
};
Shortcut g_undo_key, g_redo_key;
UINT g_undo_id = 0, g_redo_id = 0;
// オブジェクトのグループ化・グループ解除 (本体のUndoは積まれるが通知が来ないので、コマンドを観測してプロジェクトファイルを読む)
Shortcut g_group_key, g_ungroup_key;
UINT g_group_id = 0, g_ungroup_id = 0;
HHOOK g_keyboard_hook = nullptr;
HHOOK g_mouse_hook = nullptr;
HHOOK g_callwnd_hook = nullptr;
bool g_subclassed = false;
std::deque<std::pair<Command, ULONGLONG>> g_observed;	// 観測したコマンドと時刻

// ショートカットキー表記のキー名を仮想キーコードにする (対応していない名前は0)
UINT key_name_to_vk(const std::wstring& name) {
	if (name.empty()) return 0;
	if (name.size() == 1) {
		wchar_t c = (wchar_t)towupper(name[0]);
		if ((c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9')) return c;
		return 0;
	}
	if ((name[0] == L'F' || name[0] == L'f') && name.size() <= 3) {
		int n = _wtoi(name.c_str() + 1);
		if (n >= 1 && n <= 24) return VK_F1 + n - 1;
	}
	struct { const wchar_t* name; UINT vk; } table[] = {
		{ L"Delete", VK_DELETE }, { L"Insert", VK_INSERT }, { L"Space", VK_SPACE }, { L"Enter", VK_RETURN },
		{ L"Tab", VK_TAB }, { L"Backspace", VK_BACK }, { L"Home", VK_HOME }, { L"End", VK_END },
		{ L"PageUp", VK_PRIOR }, { L"PageDown", VK_NEXT }, { L"Esc", VK_ESCAPE },
	};
	for (auto& t : table) if (name == t.name) return t.vk;
	return 0;
}

// メニューの項目名(「元に戻す\tCtrl+Z」)の後ろにあるショートカットキー表記を読み取る
Shortcut read_shortcut(const wchar_t* const* names) {
	Shortcut sc;
	HMENU menu = GetMenu(edit_handle->get_host_app_window());
	const wchar_t* name = nullptr;
	UINT id = menu ? find_menu_item_any(menu, names, nullptr, nullptr, &name) : 0;
	if (!id) return sc;
	wchar_t buf[256] = {};
	GetMenuStringW(menu, id, buf, _countof(buf), MF_BYCOMMAND);
	std::wstring text = strip_access_key(buf);
	// ショートカットキー表記はタブの後ろ。タブが無ければ名前とアクセスキー(「(U)」等)の後ろ
	size_t pos = text.find(L'\t');
	if (pos == std::wstring::npos) {
		pos = std::min(wcslen(name), text.size());
		if (pos < text.size() && text[pos] == L'(') {
			size_t close = text.find(L')', pos);
			if (close != std::wstring::npos) pos = close + 1;
		}
	}
	while (pos < text.size() && (text[pos] == L' ' || text[pos] == L'\t')) pos++;
	std::wstring rest = text.substr(pos);
	while (!rest.empty() && rest.back() == L' ') rest.pop_back();
	if (rest.empty()) return sc;
	while (true) {
		size_t plus = rest.find(L'+');
		if (plus == std::wstring::npos || plus == rest.size() - 1) break;
		std::wstring mod = rest.substr(0, plus);
		if (mod == L"Ctrl") sc.ctrl = true;
		else if (mod == L"Shift") sc.shift = true;
		else if (mod == L"Alt") sc.alt = true;
		else return sc;
		rest = rest.substr(plus + 1);
	}
	sc.vk = key_name_to_vk(rest);
	sc.valid = sc.vk != 0;
	return sc;
}

void read_shortcuts() {
	g_undo_id = find_menu_id(UNDO_NAMES);
	g_redo_id = find_menu_id(REDO_NAMES);
	g_undo_key = read_shortcut(UNDO_NAMES);
	g_redo_key = read_shortcut(REDO_NAMES);
	g_group_id = find_menu_id(GROUP_NAMES);
	g_ungroup_id = find_menu_id(UNGROUP_NAMES);
	g_group_key = read_shortcut(GROUP_NAMES);
	g_ungroup_key = read_shortcut(UNGROUP_NAMES);
}

extern HWND g_hwnd;

void observe_command(Command cmd) {
	g_observed.push_back({ cmd, GetTickCount64() });
	// グループ化のUndo/Redoは通知が来ないので、来なかった時にプロジェクトファイルを読んで確かめる
	if (g_track_layer_settings) SetTimer(g_hwnd, TIMER_ID_UNNOTIFIED, UNNOTIFIED_WAIT_MS, nullptr);
}

// グループ化・グループ解除のコマンドを観測した (本体が処理し終えてから確かめる)
void observe_group_command() {
	if (g_track_layer_settings) PostMessageW(g_hwnd, WM_APP_GROUP_COMMAND, 0, 0);
}

bool is_jump_active();

// 通知が来た時に、直前に観測したコマンドを1つ取り出す (古いものは捨てる)
// クリックで戻している途中は、自分で送ったコマンドなので時間では捨てない
// (重い編集では本体の処理に時間がかかり、通知がCOMMAND_EXPIRE_MSより遅れて「移動中に編集された」と誤判定されたため)
Command take_observed_command() {
	ULONGLONG now = GetTickCount64();
	while (!is_jump_active() && !g_observed.empty() && now - g_observed.front().second > COMMAND_EXPIRE_MS) g_observed.pop_front();
	if (g_observed.empty()) return CMD_NONE;
	Command cmd = g_observed.front().first;
	g_observed.pop_front();
	return cmd;
}

// 数値欄へのキーボード入力のまとまり。Enter・Tab・Esc・マウスの押下で区切る (本体ではEscもEnterと同じく入力中の値で確定する)
int g_input_session = 0;		// 入力中のまとまりの番号
bool g_input_open = false;
int g_closed_session = -1;		// 直前に区切ったまとまりの番号 (区切った直後の確定の通知をまとめるため)
ULONGLONG g_input_close_tick = 0;
ULONGLONG g_last_input_key_tick = 0;

// 数値欄への入力がまだ確定していない項目か (入力のまとまりが開いているか、区切った直後)
// 確定するまで本体のUndo履歴には積まれていないので、本体のメニューの状態と写しが一時的に食い違う
bool is_input_pending(const Entry& e) {
	if (!e.input_object) return false;
	if (g_input_open && e.input_session == g_input_session) return true;
	return e.input_session == g_closed_session && GetTickCount64() - g_input_close_tick < INPUT_CLOSE_GRACE_MS;
}

void close_input_session() {
	if (!g_input_open) return;
	g_closed_session = g_input_session;
	g_input_close_tick = GetTickCount64();
	g_input_session++;
	g_input_open = false;
}

bool is_input_key(WPARAM vk) {
	return (vk >= '0' && vk <= '9') || (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) || vk == VK_OEM_PERIOD || vk == VK_DECIMAL ||
		vk == VK_OEM_MINUS || vk == VK_SUBTRACT || vk == VK_BACK || vk == VK_DELETE;
}

bool match_shortcut(const Shortcut& sc, WPARAM vk, bool ctrl, bool shift, bool alt) {
	return sc.valid && vk == sc.vk && ctrl == sc.ctrl && shift == sc.shift && alt == sc.alt;
}

LRESULT CALLBACK keyboard_hook_proc(int code, WPARAM wparam, LPARAM lparam) {
	// キーを押した時(キーリピートを含む)だけを見る。bit31が1なら離した時
	if (code == HC_ACTION && !(lparam & 0x80000000)) {
		bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0, alt = GetKeyState(VK_MENU) < 0;
		if (match_shortcut(g_undo_key, wparam, ctrl, shift, alt)) observe_command(CMD_UNDO);
		else if (match_shortcut(g_redo_key, wparam, ctrl, shift, alt)) observe_command(CMD_REDO);
		else if (match_shortcut(g_group_key, wparam, ctrl, shift, alt) || match_shortcut(g_ungroup_key, wparam, ctrl, shift, alt)) observe_group_command();
		else if (!ctrl && !alt && is_input_key(wparam)) {
			g_input_open = true;
			g_last_input_key_tick = GetTickCount64();
		} else if (wparam == VK_RETURN || wparam == VK_TAB || wparam == VK_ESCAPE) {
			close_input_session();
		}
	}
	return CallNextHookEx(g_keyboard_hook, code, wparam, lparam);
}

// タイムラインの右クリックのメニュー等からのグループ化・グループ解除を観測する。
// これらのメニューもメインウィンドウが持ち主だが、選んだ項目のWM_COMMANDは送られない(メニューが閉じた後に本体が実行する)ので、
// メニューの中で最後に選ばれていた項目の名前をWM_MENUSELECTで覚えておき、メニューが閉じた時にそれがグループ化・グループ解除なら確かめる
// (選ばずに閉じた場合も確かめることになるが、変化が無ければ何も足さない)。
// メインメニューの「オブジェクトのグループ化」「オブジェクトのグループ解除」はWM_COMMANDで観測しているので除く。無効な項目も除く
std::wstring g_menu_selected;

LRESULT CALLBACK callwnd_hook_proc(int code, WPARAM wparam, LPARAM lparam) {
	if (code == HC_ACTION && g_track_layer_settings) {
		auto* cwp = (CWPSTRUCT*)lparam;
		if (cwp->message == WM_MENUSELECT) {
			UINT item = LOWORD(cwp->wParam), flags = HIWORD(cwp->wParam);
			HMENU menu = (HMENU)cwp->lParam;
			if (flags == 0xFFFF && !menu) {
				// メニューが閉じた
				if (is_menu_name_any(g_menu_selected, GROUP_NAMES) || is_menu_name_any(g_menu_selected, UNGROUP_NAMES)) {
					observe_group_command();
				}
				g_menu_selected.clear();
			} else if (menu) {
				wchar_t text[256] = {};
				bool skip = (flags & (MF_POPUP | MF_GRAYED | MF_DISABLED)) || item == g_group_id || item == g_ungroup_id;
				if (!skip) GetMenuStringW(menu, item, text, 256, MF_BYCOMMAND);
				g_menu_selected = text;
			}
		}
	}
	return CallNextHookEx(g_callwnd_hook, code, wparam, lparam);
}

LRESULT CALLBACK main_subclass_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR) {
	if (message == WM_COMMAND && lparam == 0) {
		UINT id = LOWORD(wparam);
		if (id && id == g_undo_id) observe_command(CMD_UNDO);
		else if (id && id == g_redo_id) observe_command(CMD_REDO);
		else if (id && (id == g_group_id || id == g_ungroup_id)) observe_group_command();
	} else if (message == WM_INITMENUPOPUP && !g_sending_initmenu) {
		// ユーザーがメニューを開いた時にショートカットの割り当てを読み直す (割り当ての変更に追従する)
		LRESULT result = DefSubclassProc(hwnd, message, wparam, lparam);
		read_shortcuts();
		return result;
	}
	return DefSubclassProc(hwnd, message, wparam, lparam);
}

//=======================================================================
//	マウスの押下・解放 (ドラッグ1回を1件に区切る)
//=======================================================================

bool g_button_down = false;
bool g_pending_update = false;	// 押下中に来た通知を、解放で処理する

void process_update();

// 押されているボタン (左・右・中)。カメラ制御ではプレビューの右ボタンドラッグで視点を変えられ、本体はそれも1ドラッグ1件のUndoにする
enum { BUTTON_LEFT = 1, BUTTON_RIGHT = 2, BUTTON_MIDDLE = 4 };
int g_buttons = 0;

void on_button_down(int button) {
	// 前の押下の分がまだなら、新しい押下の変更が入る前に処理する
	if (g_buttons == 0 && g_pending_update) {
		g_pending_update = false;
		process_update();
	}
	g_buttons |= button;
	g_button_down = true;
}

void on_button_up(int button) {
	if (!(g_buttons & button)) return;
	g_buttons &= ~button;
	if (g_buttons == 0) {
		g_button_down = false;
		// 本体が解放を処理し終えてから処理する (投げたメッセージは次の入力より先に処理される)
		if (g_pending_update) PostMessageW(g_hwnd, WM_APP_RELEASED, 0, 0);
	}
}

LRESULT CALLBACK mouse_hook_proc(int code, WPARAM wparam, LPARAM lparam) {
	if (code == HC_ACTION) {
		switch (wparam) {
		case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: case WM_NCLBUTTONDOWN: case WM_NCLBUTTONDBLCLK:
			close_input_session();	// クリックで数値欄への入力は確定する
			on_button_down(BUTTON_LEFT);
			break;
		case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: case WM_NCRBUTTONDOWN: case WM_NCRBUTTONDBLCLK:
			on_button_down(BUTTON_RIGHT);
			break;
		case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: case WM_NCMBUTTONDOWN: case WM_NCMBUTTONDBLCLK:
			on_button_down(BUTTON_MIDDLE);
			break;
		case WM_LBUTTONUP: case WM_NCLBUTTONUP:
			on_button_up(BUTTON_LEFT);
			break;
		case WM_RBUTTONUP: case WM_NCRBUTTONUP:
			on_button_up(BUTTON_RIGHT);
			break;
		case WM_MBUTTONUP: case WM_NCMBUTTONUP:
			on_button_up(BUTTON_MIDDLE);
			break;
		}
	}
	return CallNextHookEx(g_mouse_hook, code, wparam, lparam);
}

//=======================================================================
//	判定 (通知ごとに写しを更新する)
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
	bool unnotified_checked = false;	// 送ったコマンドに通知が来ないのを確かめたか
} g_jump;

bool is_jump_active() { return g_jump.active; }

void reset_history(SceneHistory& sh, const std::wstring& reason) {
	sh.entries.clear();
	Entry start;
	start.hash = state_hash(sh.state);
	start.label = L"開始時点";
	sh.entries.push_back(start);
	sh.pos = 0;
	if (!reason.empty()) log_line(L"[EditHistory] 履歴を取り直しました (" + reason + L")");
}

// 現在のシーンの写しを用意する (無ければ全オブジェクトを読んで開始時点を作る)
void ensure_scene() {
	EDIT_INFO info = {};
	edit_handle->get_edit_info(&info, sizeof(info));
	if (info.scene_id != g_scene_id) g_scene_id = info.scene_id;
	SceneHistory& sh = current_history();
	if (!sh.initialized) {
		sh.state = State();
		refresh_state(sh, {}, true);
		reset_history(sh, L"");
		sh.initialized = true;
	}
}

// entries[from..to)を消す時、クリックで戻す目標の番号を補正する。目標が消える場合はlandedを目標にする
void adjust_jump_target(int from, int to, int landed) {
	if (!g_jump.active || to <= from) return;
	if (g_jump.target >= from && g_jump.target < to) g_jump.target = landed;
	else if (g_jump.target >= to) g_jump.target -= to - from;
}

// entries[from..to)の範囲の対象オブジェクトを集める
void collect_candidates(const SceneHistory& sh, int from, int to, std::set<OBJECT_HANDLE>& out) {
	from = std::max(from, 0);
	to = std::min(to, (int)sh.entries.size());
	for (int i = from; i < to; i++) for (auto h : sh.entries[i].objects) out.insert(h);
}

// 現在位置からdir方向(-1:前 / +1:後ろ)に、ハッシュが一致する最も近い項目を探す (無ければ-1)
int find_nearest(const SceneHistory& sh, uint64_t hash, int dir) {
	for (int i = sh.pos + dir; i >= 0 && i < (int)sh.entries.size(); i += dir) {
		if (sh.entries[i].hash == hash) return i;
	}
	return -1;
}

// まとめた項目のラベルを付け直す。「…項目 A→B」の形で同じ項目の変更どうしなら、最初の変更前の値→最後の変更後の値にする
void merge_label(const Entry& first, Entry& last) {
	if (first.has_change && last.has_change) last.change_before = first.change_before;
	size_t fa = first.label.rfind(L'→'), la = last.label.rfind(L'→');
	if (fa == std::wstring::npos || la == std::wstring::npos) return;
	size_t fs = first.label.rfind(L' ', fa), ls = last.label.rfind(L' ', la);
	if (fs == std::wstring::npos || ls == std::wstring::npos) return;
	if (first.label.compare(0, fs, last.label, 0, ls) != 0) return;
	last.label = last.label.substr(0, ls + 1) + first.label.substr(fs + 1, fa - fs - 1) + last.label.substr(la);
	if (!first.input_before.empty()) last.input_before = first.input_before;
}

// Undoでpos→targetへ移った時、間の項目(本体のUndo履歴に無い途中の状態)をまとめる
// entries[target+1..pos]が本体では1件だったので、entries[pos]を残して間を消す
void merge_skipped_undo(SceneHistory& sh, int target) {
	if (sh.pos - target > 1) {
		Entry& last = sh.entries[sh.pos];
		merge_label(sh.entries[target + 1], last);
		for (int i = target + 1; i < sh.pos; i++) {
			for (auto h : sh.entries[i].objects) last.objects.push_back(h);
		}
		adjust_jump_target(target + 1, sh.pos, target);
		sh.entries.erase(sh.entries.begin() + target + 1, sh.entries.begin() + sh.pos);
	}
	sh.pos = target;
}

// Redoでpos→targetへ移った時、間の項目をまとめる (entries[target]を残して間を消す)
void merge_skipped_redo(SceneHistory& sh, int target) {
	if (target - sh.pos > 1) {
		Entry& last = sh.entries[target];
		merge_label(sh.entries[sh.pos + 1], last);
		for (int i = sh.pos + 1; i < target; i++) {
			for (auto h : sh.entries[i].objects) last.objects.push_back(h);
		}
		adjust_jump_target(sh.pos + 1, target, sh.pos + 1);
		sh.entries.erase(sh.entries.begin() + sh.pos + 1, sh.entries.begin() + target);
	}
	sh.pos = sh.pos + 1;
}

// 行き先を探す。見つからなければ全オブジェクトを読み直してもう一度探す
int find_destination(SceneHistory& sh, int dir, uint64_t& hash) {
	int found = find_nearest(sh, hash, dir);
	if (found >= 0) return found;
	refresh_state(sh, {}, true);
	hash = state_hash(sh.state);
	return find_nearest(sh, hash, dir);
}

void add_edit(SceneHistory& sh, const Refresh& r, uint64_t hash) {
	sh.entries.resize(sh.pos + 1);	// やり直せる分を捨てる
	Entry e;
	e.hash = hash;
	make_entry_info(r, e);
	sh.entries.push_back(e);
	sh.pos = (int)sh.entries.size() - 1;
	// 本体のUndoの上限を超えた分は先頭(開始時点の次)から捨てる
	while ((int)sh.entries.size() > HISTORY_MAX + 1) {
		sh.entries.erase(sh.entries.begin());
		sh.entries[0].label = L"開始時点";
		sh.pos--;
	}
}

void abort_jump(const std::wstring& reason) {
	if (!g_jump.active) return;
	g_jump.active = false;
	KillTimer(g_hwnd, TIMER_ID_JUMP_TIMEOUT);
	log_line(L"[EditHistory] 移動を中止しました (" + reason + L")");
}

void send_next_jump_command();

// Escが押されたか (AviUtl2が前面の時だけ)。押し続けていなくても、前回調べてから押されていれば真
bool is_escape_pressed() {
	SHORT state = GetAsyncKeyState(VK_ESCAPE);
	if (!(state & 0x8001)) return false;
	DWORD process = 0;
	GetWindowThreadProcessId(GetForegroundWindow(), &process);
	return process == GetCurrentProcessId();
}

// 通知を受けて写しを更新する
void process_update() {
	if (!g_initialized) return;
	int old_scene = g_scene_id;
	ensure_scene();
	SceneHistory& sh = current_history();
	if (old_scene != g_scene_id) {
		abort_jump(L"シーンが切り替わった");
		// シーンが切り替わった: そのシーンの写しの現在位置と一致するか確かめる
		refresh_state(sh, {}, false);
		if (state_hash(sh.state) != sh.entries[sh.pos].hash) {
			refresh_state(sh, {}, true);
			if (state_hash(sh.state) != sh.entries[sh.pos].hash) reset_history(sh, L"シーンに戻った時に状態が一致しない");
		}
		update_list();
		return;
	}

	// 入力が確定したら、預けていたやり直せる分は要らない (本体でも確定時にやり直せる分が消える)
	if (!sh.entries[sh.pos].stashed_future.empty() && !is_input_pending(sh.entries[sh.pos])) {
		sh.entries[sh.pos].stashed_future.clear();
	}

	Command cmd = take_observed_command();
	std::set<OBJECT_HANDLE> candidates;
	if (cmd == CMD_UNDO) collect_candidates(sh, 0, sh.pos + 1, candidates);
	if (cmd == CMD_REDO) collect_candidates(sh, sh.pos + 1, (int)sh.entries.size(), candidates);
	Refresh r = refresh_state(sh, candidates, false);
	if (!r.ok) return;
	uint64_t hash = state_hash(sh.state);
	int undo_enabled = get_menu_enabled(UNDO_NAMES);
	int redo_enabled = get_menu_enabled(REDO_NAMES);

	if (cmd == CMD_UNDO || cmd == CMD_REDO) {
		int dir = cmd == CMD_UNDO ? -1 : 1;
		int found = find_destination(sh, dir, hash);
		if (found >= 0) {
			if (cmd == CMD_UNDO) merge_skipped_undo(sh, found);
			else merge_skipped_redo(sh, found);
		} else if (cmd == CMD_UNDO && hash != sh.entries[sh.pos].hash) {
			// 開始時点より前の状態へのUndo: 先頭に足す
			Entry e;
			e.hash = hash;
			e.label = L"開始時点";
			sh.entries[0].label = L"(開始時点より前の編集)";
			sh.entries.insert(sh.entries.begin(), e);
			sh.pos = 0;
		} else if (hash != sh.entries[sh.pos].hash) {
			reset_history(sh, cmd == CMD_UNDO ? L"元に戻した先が見つからない" : L"やり直した先が見つからない");
		}
	} else {
		// 数値欄へのキーボード入力の途中か (キー入力の直後に1つの数値項目だけが変わった)
		// 入力は確定するまで本体の履歴に積まれないので、本体の「やり直し」が有効のままでも新しい編集として扱う
		OBJECT_HANDLE handle = nullptr;
		std::string effect, key, before, after;
		ULONGLONG now = GetTickCount64();
		bool typing = now - g_last_input_key_tick < INPUT_KEY_WINDOW_MS &&
			single_numeric_change(r, handle, effect, key, before, after);
		if (typing) {
			Entry& top = sh.entries[sh.pos];
			bool same_session = (g_input_open && top.input_session == g_input_session) ||
				(top.input_session == g_closed_session && now - g_input_close_tick < INPUT_CLOSE_GRACE_MS);
			if (sh.pos == (int)sh.entries.size() - 1 && top.input_object == handle &&
				top.input_effect == effect && top.input_key == key && same_session) {
				// 同じ入力の続き: 同じ項目を上書きする (本体のUndoは確定した値の1件だけ)
				top.hash = hash;
				if (top.has_change && r.objects.size() == 1) top.change_after = r.objects[0].after;
				auto it = sh.state.objects.find(handle);
				std::wstring title = it != sh.state.objects.end() ? object_title(it->second) + L" " : L"";
				top.label = title + (effect.empty() ? L"" : utf8_to_wide(effect) + L" ") + utf8_to_wide(key) +
					describe_value_change(top.input_before, after);
				// 入力した結果が入力前と同じ状態に戻った場合、本体の履歴には何も積まれないので項目を消し、預けたやり直せる分を戻す
				if (sh.pos > 0 && sh.entries[sh.pos - 1].hash == hash) {
					std::vector<Entry> future = std::move(top.stashed_future);
					sh.entries.pop_back();
					sh.pos--;
					for (auto& e : future) sh.entries.push_back(std::move(e));
				}
			} else {
				std::vector<Entry> future(sh.entries.begin() + sh.pos + 1, sh.entries.end());
				add_edit(sh, r, hash);
				Entry& e = sh.entries[sh.pos];
				e.input_object = handle;
				e.input_effect = effect;
				e.input_key = key;
				e.input_before = before;
				e.input_session = g_input_open ? g_input_session : g_closed_session;
				e.stashed_future = std::move(future);
			}
		} else if (redo_enabled == 0) {
			// やり直しが無効 = 新しい編集 (または最新の状態までのRedoを取りこぼした)
			if (r.empty() && hash == sh.entries[sh.pos].hash) {
				// 選択外のオブジェクトの編集(プレビューでのマウス操作・オブジェクトリストからの変更等)かもしれないので読み直して探す。
				// 設定で有効なら全オブジェクト、無効なら再生位置にあるオブジェクトだけ (1レイヤーに1個なので数が限られる)。
				// レイヤー設定の編集もここに来るので、見つからなければレイヤー設定を読む (こちらも設定で有効な時だけ)
				if (g_track_other_objects) {
					r = refresh_state(sh, {}, true);
				} else {
					EDIT_INFO info = {};
					edit_handle->get_edit_info(&info, sizeof(info));
					std::set<OBJECT_HANDLE> at_cursor;
					for (auto& [h, o] : sh.state.objects) {
						if (o.lf.start <= info.frame && info.frame <= o.lf.end) at_cursor.insert(h);
					}
					if (!at_cursor.empty()) r = refresh_state(sh, at_cursor, false);
					if (r.empty()) refresh_project_file(sh, r);
				}
				hash = state_hash(sh.state);
			}
			if (hash == sh.entries[sh.pos].hash) {
				// 内容が変わっていない (1回の編集で通知が2件来た2件目など)
			} else if (sh.pos + 1 == (int)sh.entries.size() - 1 && sh.entries[sh.pos + 1].hash == hash) {
				sh.pos++;
			} else {
				add_edit(sh, r, hash);
			}
		} else if (redo_enabled == 1) {
			// やり直しが有効なのにコマンドを観測していない: 取りこぼしたUndo/Redoとみなし、近い方へ
			if (hash != sh.entries[sh.pos].hash) {
				int back = find_nearest(sh, hash, -1);
				int forward = find_nearest(sh, hash, 1);
				if (back < 0 && forward < 0) {
					refresh_state(sh, {}, true);
					hash = state_hash(sh.state);
					back = find_nearest(sh, hash, -1);
					forward = find_nearest(sh, hash, 1);
				}
				if (back >= 0 && (forward < 0 || sh.pos - back <= forward - sh.pos)) merge_skipped_undo(sh, back);
				else if (forward >= 0) merge_skipped_redo(sh, forward);
				else reset_history(sh, L"Undo/Redoの行き先が見つからない");
			}
		}
	}

	// クリックで戻している途中なら、進み具合を確かめる (先頭を詰める前の位置で判断する)
	bool send_next = false;
	if (g_jump.active) {
		if (sh.pos == g_jump.target) {
			g_jump.active = false;
			KillTimer(g_hwnd, TIMER_ID_JUMP_TIMEOUT);
		} else if (cmd == CMD_NONE) {
			abort_jump(L"移動中に編集された");
		} else if ((g_jump.target < g_jump.last_pos) != (sh.pos < g_jump.last_pos) || sh.pos == g_jump.last_pos) {
			abort_jump(L"想定と違う位置に移動した");
		} else {
			send_next = true;
		}
	}

	// 本体の履歴の先頭なら、写しの前を詰める (Undoの上限に達した場合など)
	// 数値欄への入力が確定していない間は本体のUndo履歴にまだ積まれていないので詰めない
	if (undo_enabled == 0 && sh.pos > 0 && !is_input_pending(sh.entries[sh.pos])) {
		adjust_jump_target(0, sh.pos, 0);
		sh.entries.erase(sh.entries.begin(), sh.entries.begin() + sh.pos);
		sh.entries[0].label = L"開始時点";
		sh.pos = 0;
		if (send_next && g_jump.target == 0) {
			send_next = false;
			g_jump.active = false;
			KillTimer(g_hwnd, TIMER_ID_JUMP_TIMEOUT);
		}
	}
	// Escで中止 (戻している間は入力が処理されずキーボードフックにも届かないので、キーの状態を直接見る)
	if (send_next && is_escape_pressed()) {
		send_next = false;
		abort_jump(L"Escで中止した");
	}
	// 次のコマンドはすぐに送る。投げたメッセージは入力・再描画より先に処理されるので、戻し終わるまで
	// ユーザーの入力は処理されず、戻し終わった後に順に処理される (途中で編集されて移動が中止されることが無い)。
	// 本体の画面・一覧も更新されないので、戻している途中の帯はsend_next_jump_command()で直接描く
	if (send_next) send_next_jump_command();
	update_list();
}

// 観測したUndo/Redoに通知が来ないまま時間が経った: グループ化のUndo/Redoかもしれないので、プロジェクトファイルを読んで確かめる。
// グループかレイヤー設定が変わっていれば、観測したコマンドの行き先を探す (変わっていなければ、通知が遅れているだけとみなして待つ)
void check_unnotified_command() {
	if (!g_track_layer_settings || !g_initialized || g_observed.empty()) return;
	SceneHistory& sh = current_history();
	Refresh r;
	refresh_project_file(sh, r);
	if (r.empty()) return;
	process_update();
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
	bool undo = g_jump.target < sh.pos;
	const wchar_t* name = undo ? L"元に戻す" : L"やり直し";	// ログに出す名前
	const wchar_t* const* names = undo ? UNDO_NAMES : REDO_NAMES;
	UINT id = find_menu_id(names);
	if (!id || get_menu_enabled(names) != 1) {
		abort_jump(std::wstring(L"「") + name + L"」が使えない");
		return;
	}
	if (++g_jump.sent > g_jump.max_send) {
		abort_jump(L"送った回数が上限を超えた");
		return;
	}
	g_jump.last_pos = sh.pos;
	g_jump.sent_tick = GetTickCount64();
	g_jump.unnotified_checked = false;
	SetTimer(g_hwnd, TIMER_ID_JUMP_TIMEOUT, JUMP_TIMEOUT_MS, nullptr);
	// メニューのWM_COMMANDはサブクラスで観測され、通知と組になってUndo/Redoとして判定される
	PostMessageW(edit_handle->get_host_app_window(), WM_COMMAND, MAKEWPARAM(id, 0), 0);
	// 通知が来るまでの間も投げたメッセージを絶やさない (絶えると、その隙にユーザーの入力が処理されて編集が割り込み、
	// 自分のUndo/Redoのコマンドと組にされて「行き先が見つからない」になった)
	if (!g_jump.keeping) {
		g_jump.keeping = true;
		PostMessageW(g_hwnd, WM_APP_JUMP_KEEP, 0, 0);
	}
}

void start_jump(int target) {
	SceneHistory& sh = current_history();
	if (g_jump.active || target == sh.pos || target < 0 || target >= (int)sh.entries.size()) return;
	g_jump.active = true;
	g_jump.target = target;
	g_jump.sent = 0;
	// 写しに無い途中の状態が挟まっている可能性があるので余裕を持たせる
	g_jump.max_send = std::abs(target - sh.pos) * 2 + 5;
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

// 一覧の1行 (同じオブジェクトへの連続したテキスト入力は1行にまとめる)
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
		if (!g_rows.empty() && e.text_object && i > 0) {
			const Row& prev = g_rows.back();
			const Entry& p = sh.entries[prev.last];
			// 現在位置をまたいでまとめると、やり直せる分との境目が分からなくなるのでまとめない
			bool crosses = prev.last <= sh.pos && i > sh.pos;
			if (p.text_object == e.text_object && p.text_item == e.text_item && !crosses) {
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

void draw_item(const DRAWITEMSTRUCT* dis) {
	if (dis->itemID == (UINT)-1 || dis->itemID >= g_rows.size()) return;
	const Row& row = g_rows[dis->itemID];
	SceneHistory& sh = current_history();
	bool current = row.first <= sh.pos && sh.pos <= row.last;
	bool future = row.first > sh.pos;
	COLORREF back = current ? color_from_config("GroupingSelect", RGB(0x48, 0x48, 0x48)) : color_from_config("Background", RGB(0x20, 0x20, 0x20));
	COLORREF text = future ? color_from_config("TextDisable", RGB(0x90, 0x90, 0x90)) : color_from_config("Text", RGB(0xff, 0xff, 0xff));
	HBRUSH brush = CreateSolidBrush(back);
	FillRect(dis->hDC, &dis->rcItem, brush);
	DeleteObject(brush);

	// まとめた行は、最後の項目のラベル (入力後の内容) を出す。
	// 同じパラメータ・位置の連続した変更をまとめた行は、最初の変更前→最後の変更後の変化として説明し直す
	std::wstring label = sh.entries[row.last].label;
	const Entry& first_entry = sh.entries[row.first];
	const Entry& last_entry = sh.entries[row.last];
	if (row.last > row.first && first_entry.has_change && last_entry.has_change && !last_entry.text_item.empty() &&
		(last_entry.text_item[0] == '\x1c' || last_entry.text_item[0] == '\x1d')) {
		ObjChange c;
		c.handle = last_entry.text_object;
		c.before = first_entry.change_before;
		c.after = last_entry.change_after;
		c.moved = !same_lf(c.before.lf, c.after.lf);
		c.content = c.before.alias_hash != c.after.alias_hash;
		std::string item;
		label = describe_object_change(c, item);
	}
	if (row.last > row.first && !last_entry.text_item.empty() && last_entry.text_item[0] == '\x1b') {
		label = last_entry.setting_prefix + first_entry.setting_before + L"→" + last_entry.setting_after;
	}
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
	std::wstring number_text = std::to_wstring(number);
	DrawTextW(dis->hDC, number_text.c_str(), -1, &rc_number, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
	RECT rc = dis->rcItem;
	rc.left = rc_number.right + 10;
	rc.right -= 4;
	DrawTextW(dis->hDC, label.c_str(), -1, &rc, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
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

void load_options() {
	std::wstring path = ini_path();
	if (path.empty()) return;
	g_track_layer_settings = GetPrivateProfileIntW(L"EditHistory", L"LayerSettings", 1, path.c_str()) != 0;
	g_track_other_objects = GetPrivateProfileIntW(L"EditHistory", L"OtherObjects", 1, path.c_str()) != 0;
}

void save_options() {
	std::wstring path = ini_path();
	if (path.empty()) return;
	WritePrivateProfileStringW(L"EditHistory", L"LayerSettings", g_track_layer_settings ? L"1" : L"0", path.c_str());
	WritePrivateProfileStringW(L"EditHistory", L"OtherObjects", g_track_other_objects ? L"1" : L"0", path.c_str());
}

// 一覧の右クリックのメニュー
#define IDM_LAYER_SETTINGS	1
#define IDM_OTHER_OBJECTS	2

void show_context_menu(HWND hwnd, LPARAM lparam) {
	POINT pt = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
	if (lparam == -1) {	// キーボードから開いた時は一覧の左上に出す
		pt = { 0, 0 };
		ClientToScreen(hwnd, &pt);
	}
	HMENU menu = CreatePopupMenu();
	if (!menu) return;
	UINT grayed = g_jump.active || !g_initialized ? MF_GRAYED : 0;
	AppendMenuW(menu, MF_STRING | (g_track_other_objects ? MF_CHECKED : 0) | grayed, IDM_OTHER_OBJECTS,
		L"選択中・再生位置以外のオブジェクトの変更も記録する (オブジェクトが多いと重くなります)");
	AppendMenuW(menu, MF_STRING | (g_track_layer_settings ? MF_CHECKED : 0) | grayed, IDM_LAYER_SETTINGS,
		L"レイヤー設定・グループ化の変更も記録する (試験的)");
	int command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
	DestroyMenu(menu);
	if (command == IDM_OTHER_OBJECTS) {
		// 状態のハッシュの中身は変わらないので、履歴は取り直さない
		g_track_other_objects = !g_track_other_objects;
		save_options();
	}
	if (command == IDM_LAYER_SETTINGS) {
		g_track_layer_settings = !g_track_layer_settings;
		save_options();
		// 記録する内容(状態のハッシュ)が変わり、今までの履歴と照らし合わせられなくなるので取り直す
		g_scenes.clear();
		g_scene_id = -1;
		ensure_scene();
		update_list();
		log_line(L"[EditHistory] 履歴を取り直しました (レイヤー設定の記録を切り替えた)");
	}
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
	if (message == WM_CONTEXTMENU) {
		show_context_menu(hwnd, lparam);
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
		if (!g_initialized) {
			HWND main = edit_handle->get_host_app_window();
			if (!main || !GetMenu(main)) {
				PostMessageW(hwnd, WM_APP_INIT, 0, 0);	// メインウィンドウがまだ無ければ後でもう一度
				return 0;
			}
			read_shortcuts();
			if (!g_undo_id || !g_redo_id) log_line(L"[EditHistory] メニューに「元に戻す」「やり直し」が見つかりません");
			g_keyboard_hook = SetWindowsHookExW(WH_KEYBOARD, keyboard_hook_proc, nullptr, GetCurrentThreadId());
			g_mouse_hook = SetWindowsHookExW(WH_MOUSE, mouse_hook_proc, nullptr, GetCurrentThreadId());
			g_callwnd_hook = SetWindowsHookExW(WH_CALLWNDPROC, callwnd_hook_proc, nullptr, GetCurrentThreadId());
			g_subclassed = SetWindowSubclass(main, main_subclass_proc, 1, 0) != FALSE;
			g_initialized = true;
			ensure_scene();
			update_list();
		}
		return 0;

	case WM_APP_OBJECT_UPDATED:
		if (!g_initialized) return 0;
		// 解放を取りこぼした時(右クリックのメニュー等)に押下中のままにならないよう、どのボタンも押されていなければ解除する。
		// 物理的な状態(GetAsyncKeyState)ではなく、このスレッドが処理した入力での状態(GetKeyState)で見る。
		// クリックで戻している間に溜まった入力が後でまとめて処理される時、物理的にはとっくに離していても
		// 本体はまだドラッグの途中なので、物理的な状態で解除すると確定前の通知を処理して履歴を取り直してしまった
		if (g_button_down && GetKeyState(VK_LBUTTON) >= 0 && GetKeyState(VK_RBUTTON) >= 0 && GetKeyState(VK_MBUTTON) >= 0) {
			g_buttons = 0;
			g_button_down = false;
		}
		if (g_button_down && !g_jump.active) {
			// 押下中はまとめて、解放で処理する (一覧のクリックで戻している途中は、押下中でもすぐ処理する)
			g_pending_update = true;
		} else {
			process_update();
		}
		return 0;

	case WM_APP_JUMP_KEEP: {
		// クリックで戻している間、自分宛てに投げ続けてユーザーの入力を戻し終わるまで後回しにする
		// (タイマーも処理されなくなるので、Escと応答なしの確認、ぐるぐるを回すのもここで行う)
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
		// 送ったUndo/Redoに通知が来ない: グループ化のUndo/Redoかもしれないので確かめる (タイマーはここでは処理されない)
		if (!g_jump.unnotified_checked && now - g_jump.sent_tick > UNNOTIFIED_WAIT_MS) {
			g_jump.unnotified_checked = true;
			check_unnotified_command();
			if (!g_jump.active) {
				g_jump.keeping = false;
				return 0;
			}
		}
		// 本体の通知が投げたメッセージの処理を待っている等で来ない場合に備え、長く来なければ投げるのをやめる
		// (次にコマンドを送る時にまた始める。応答なしの判定はタイマーで行われる)
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

	case WM_APP_GROUP_COMMAND:
		// グループ化・グループ解除: 通知が来ないので、本体が処理し終えた後に通知が来たのと同じように確かめる
		// (変化が見つからない時はプロジェクトファイルを読むので、グループの変化が項目になる)
		if (g_initialized && !g_jump.active) process_update();
		return 0;

	case WM_APP_RELEASED:
		if (g_pending_update && !g_button_down) {
			g_pending_update = false;
			process_update();
		}
		return 0;

	case WM_APP_SCENE_CHANGED:
		if (g_initialized) process_update();
		return 0;

	case WM_APP_PROJECT_LOADED:
		// プロジェクトの読み込み・新規作成で本体のUndo履歴は空になるので、全シーンの写しを捨てる
		abort_jump(L"プロジェクトが読み込まれた");
		g_scenes.clear();
		g_scene_id = -1;
		g_observed.clear();
		g_pending_update = false;
		if (g_initialized) {
			ensure_scene();
			update_list();
		} else {
			PostMessageW(hwnd, WM_APP_INIT, 0, 0);
		}
		return 0;

	case WM_TIMER:
		if (wparam == TIMER_ID_UNNOTIFIED) {
			KillTimer(hwnd, TIMER_ID_UNNOTIFIED);
			if (!g_jump.active) check_unnotified_command();
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
	if (g_keyboard_hook) UnhookWindowsHookEx(g_keyboard_hook);
	if (g_mouse_hook) UnhookWindowsHookEx(g_mouse_hook);
	if (g_callwnd_hook) UnhookWindowsHookEx(g_callwnd_hook);
	if (g_subclassed && edit_handle) RemoveWindowSubclass(edit_handle->get_host_app_window(), main_subclass_proc, 1);
	if (g_list) RemoveWindowSubclass(g_list, list_subclass_proc, 1);
	if (g_font) DeleteObject(g_font);
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
	load_options();
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
	PostMessageW(g_hwnd, WM_APP_INIT, 0, 0);
}

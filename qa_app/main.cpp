#include <iostream>
#include <string>
#include <fstream>
#include <sstream>
#include <vector>
#include <map>
#include <thread>
#include <atomic>
#include <mutex>
#include <functional>
#include <algorithm>
#include <cstring>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
typedef SOCKET socket_t;
#define INVALID_SOCK INVALID_SOCKET
#define CLOSE_SOCKET closesocket

// 托盘图标相关
#define WM_TRAYICON (WM_USER + 1)
#define ID_TRAYICON 1
#define IDM_OPEN_BROWSER 2001
#define IDM_EXIT 2002
static NOTIFYICONDATAA g_nid;
static HWND g_hwnd = NULL;
static std::atomic<bool> g_running(true);
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <signal.h>
typedef int socket_t;
#define INVALID_SOCK -1
#define CLOSE_SOCKET close
#endif

#include <liboai.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// 安全序列化：将非法 UTF-8 字节替换为 U+FFFD，避免 dump() 因不可信输入
// （畸形请求体、含原始字节的异常消息）抛出 type_error.316 导致进程终止。
static std::string safeDump(const json& j) {
    return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

// ============ 配置管理 ============
struct Config {
    std::string api_url = "https://api.openai.com/v1";
    std::string api_key;
    std::string model = "gpt-3.5-turbo";
    std::string vision_model = "gpt-4o";  // 视觉模型，用于图片识别
    std::string embedding_model = "text-embedding-3-small";
    bool memory_enabled = true;  // 记忆检索（RAG）开关
};

static Config g_config;
static std::mutex g_config_mutex;

// 对话消息
struct Message {
    std::string role;
    std::string content;
    std::string image_base64;
    std::string image_mime;
    int prompt_tokens = 0;
    int completion_tokens = 0;
    int total_tokens = 0;
};

// 会话
struct Session {
    std::string id;
    std::string title = "新对话";
    long long created_at = 0;
    std::vector<Message> messages;
};

static std::map<std::string, Session> g_sessions;
static std::mutex g_sessions_mutex;
static std::atomic<bool> g_stop_requested{false};
static std::string g_system_prompt = "你是一个友好、专业的中文AI助手。请用详细、有条理的语言回答用户的问题，必要时提供示例代码或具体案例来辅助说明。如果问题涉及多个方面，请分点阐述。";

bool loadConfig(const std::string& path, Config& config) {
    std::ifstream file(path);
    if (!file.is_open()) return false;
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        size_t pos = line.find('=');
        if (pos == std::string::npos) continue;
        std::string key = line.substr(0, pos);
        std::string value = line.substr(pos + 1);
        while (!key.empty() && key.back() == ' ') key.pop_back();
        while (!value.empty() && value.front() == ' ') value = value.substr(1);
        if (key == "api_url") config.api_url = value;
        else if (key == "api_key") config.api_key = value;
        else if (key == "model") config.model = value;
        else if (key == "vision_model") config.vision_model = value;
        else if (key == "embedding_model") config.embedding_model = value;
        else if (key == "memory_enabled") config.memory_enabled = (value == "1" || value == "true");
    }
    return true;
}

void saveConfig(const std::string& path, const Config& config) {
    std::ofstream file(path);
    if (!file.is_open()) return;
    file << "# LLM API 配置文件\n";
    file << "# 常见服务商:\n";
    file << "# OpenAI:   url=https://api.openai.com/v1, model=gpt-3.5-turbo\n";
    file << "# 通义千问: url=https://dashscope.aliyuncs.com/compatible-mode/v1, model=qwen-turbo\n";
    file << "# Kimi:    url=https://api.moonshot.cn/v1, model=moonshot-v1-8k\n";
    file << "# DeepSeek: url=https://api.deepseek.com/v1, model=deepseek-chat\n\n";
    file << "api_url=" << config.api_url << "\n";
    file << "api_key=" << config.api_key << "\n";
    file << "model=" << config.model << "\n";
    file << "vision_model=" << config.vision_model << "\n";
    file << "embedding_model=" << config.embedding_model << "\n";
    file << "memory_enabled=" << (config.memory_enabled ? "1" : "0") << "\n";
}

// ============ 会话持久化 ============
long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string genId() {
    static std::atomic<int> counter{0};
    return std::to_string(nowMs()) + "_" + std::to_string(counter++);
}

// UTF-8 安全截断，避免多字节字符被截断产生乱码
std::string utf8Truncate(const std::string& s, size_t maxChars) {
    size_t count = 0, i = 0;
    while (i < s.size() && count < maxChars) {
        unsigned char c = (unsigned char)s[i];
        size_t step = (c < 0x80) ? 1 : ((c >> 5) == 6 ? 2 : ((c >> 4) == 14 ? 3 : 4));
        if (i + step > s.size()) break;
        i += step;
        count++;
    }
    return s.substr(0, i);
}

// URL 解码（%XX 与 +）
std::string urlDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int hi = hex(s[i+1]), lo = hex(s[i+2]);
            if (hi >= 0 && lo >= 0) { out += (char)(hi * 16 + lo); i += 2; continue; }
        }
        out += (s[i] == '+') ? ' ' : s[i];
    }
    return out;
}

// 从带查询串的路径中提取参数，如 /api/search?q=xxx
std::string getQueryParam(const std::string& path, const std::string& key) {
    size_t qpos = path.find('?');
    if (qpos == std::string::npos) return "";
    std::string qs = path.substr(qpos + 1);
    size_t start = 0;
    while (start < qs.size()) {
        size_t amp = qs.find('&', start);
        std::string pair = qs.substr(start, amp == std::string::npos ? std::string::npos : amp - start);
        size_t eq = pair.find('=');
        if (eq != std::string::npos && pair.substr(0, eq) == key)
            return urlDecode(pair.substr(eq + 1));
        if (amp == std::string::npos) break;
        start = amp + 1;
    }
    return "";
}

// ASCII 大小写不敏感子串查找（UTF-8 多字节字节不受影响）
size_t findNoCase(const std::string& hay, const std::string& needle) {
    if (needle.empty() || hay.size() < needle.size()) return std::string::npos;
    auto low = [](char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; };
    for (size_t i = 0; i + needle.size() <= hay.size(); i++) {
        size_t j = 0;
        while (j < needle.size() && low(hay[i+j]) == low(needle[j])) j++;
        if (j == needle.size()) return i;
    }
    return std::string::npos;
}

// 生成匹配位置周围的 UTF-8 安全片段（前后各 radius 字节，回退到字符边界）
std::string makeSnippet(const std::string& text, size_t pos, size_t len, size_t radius = 60) {
    auto boundaryBack = [&](size_t p) {
        while (p > 0 && ((unsigned char)text[p] & 0xC0) == 0x80) p--;
        return p;
    };
    auto boundaryFwd = [&](size_t p) {
        while (p < text.size() && ((unsigned char)text[p] & 0xC0) == 0x80) p++;
        return p;
    };
    size_t begin = boundaryBack(pos > radius ? pos - radius : 0);
    size_t end = boundaryFwd(std::min(text.size(), pos + len + radius));
    // 片段内部换行替换为空格，便于侧边栏单行展示
    std::string snip = text.substr(begin, end - begin);
    for (auto& c : snip) if (c == '\n' || c == '\r') c = ' ';
    return (begin > 0 ? "…" : "") + snip + (end < text.size() ? "…" : "");
}

void saveSessions() {
    // 调用者需持有 g_sessions_mutex
    json root = json::array();
    for (auto& [id, s] : g_sessions) {
        json js;
        js["id"] = s.id;
        js["title"] = s.title;
        js["created_at"] = s.created_at;
        json msgs = json::array();
        for (auto& m : s.messages) {
            json jm;
            jm["role"] = m.role;
            jm["content"] = m.content;
            jm["image_mime"] = m.image_mime;
            // 不持久化大体积 base64，仅记录是否有图
            jm["has_image"] = !m.image_base64.empty();
            jm["total_tokens"] = m.total_tokens;
            msgs.push_back(jm);
        }
        js["messages"] = msgs;
        root.push_back(js);
    }
    // 原子写：先写临时文件，完整落盘后再替换正式文件，
    // 避免写入中途崩溃导致 sessions.json 损坏
    try {
        const std::string tmpPath = "sessions.json.tmp";
        {
            std::ofstream file(tmpPath, std::ios::trunc);
            if (!file.is_open()) return;
            file << root.dump(1);
            file.flush();
            if (!file.good()) return; // 写入失败则保留旧文件
        }
#ifdef _WIN32
        // MoveFileEx + REPLACE_EXISTING：替换操作本身原子，无 remove/rename 间隙
        MoveFileExA(tmpPath.c_str(), "sessions.json", MOVEFILE_REPLACE_EXISTING);
#else
        std::remove("sessions.json");
        std::rename(tmpPath.c_str(), "sessions.json");
#endif
    } catch (...) {}
}

void loadSessions() {
    std::ifstream file("sessions.json");
    if (!file.is_open()) return;
    try {
        json root;
        file >> root;
        if (!root.is_array()) return;
        for (auto& js : root) {
            Session s;
            s.id = js.value("id", "");
            s.title = js.value("title", "新对话");
            s.created_at = js.value("created_at", 0LL);
            if (js.contains("messages") && js["messages"].is_array()) {
                for (auto& jm : js["messages"]) {
                    Message m;
                    m.role = jm.value("role", "user");
                    m.content = jm.value("content", "");
                    m.image_mime = jm.value("image_mime", "");
                    m.total_tokens = jm.value("total_tokens", 0);
                    s.messages.push_back(m);
                }
            }
            if (!s.id.empty()) g_sessions[s.id] = s;
        }
    } catch (...) {}
}

// ============ 记忆索引（RAG 检索层） ============
// 以「一问一答」为一个 chunk；向量 L2 归一化后存储，余弦相似度退化为点积。
// 索引使用独立互斥量，检索/写入不阻塞会话操作（最小临界区原则）。
struct Chunk {
    std::string session_id;
    int msg_index = 0;      // 该轮 user 消息在 messages 中的下标
    std::string text;       // 块文本（检索结果展示用）
    std::vector<float> vec; // 已归一化
};

static std::vector<Chunk> g_index;
static std::mutex g_index_mutex;

// 向量通道降级：连续失败进入纯关键词模式，冷却后自动重试
static std::atomic<long long> g_embed_disabled_until{0};
static std::atomic<int> g_embed_fail_count{0};

void l2Normalize(std::vector<float>& v) {
    double sum = 0;
    for (float x : v) sum += (double)x * x;
    float norm = (float)std::sqrt(sum);
    if (norm > 1e-9f) for (float& x : v) x /= norm;
}

float dotProduct(const std::vector<float>& a, const std::vector<float>& b) {
    size_t n = std::min(a.size(), b.size());
    float s = 0;
    for (size_t i = 0; i < n; i++) s += a[i] * b[i];
    return s;
}

// 调用 embedding API（锁外执行，仅取配置时短暂持锁）
bool requestEmbedding(const std::string& text, std::vector<float>& out) {
    long long disabledUntil = g_embed_disabled_until.load();
    if (disabledUntil != 0 && nowMs() < disabledUntil) return false;

    std::string apiUrl, apiKey, model;
    {
        std::lock_guard<std::mutex> lock(g_config_mutex);
        apiUrl = g_config.api_url;
        apiKey = g_config.api_key;
        model = g_config.embedding_model;
    }
    if (model.empty()) return false;

    try {
        liboai::OpenAI openai(apiUrl);
        if (!openai.auth.SetKey(apiKey)) return false;
        liboai::Response res = openai.Embedding->create(model, text);
        json j = json::parse(res.content);        if (!j.contains("data") || !j["data"].is_array() || j["data"].empty()
            || !j["data"][0].contains("embedding")) {
            throw std::runtime_error("embedding 响应格式异常");
        }
        out.clear();
        for (auto& v : j["data"][0]["embedding"]) out.push_back(v.get<float>());
        if (out.empty()) throw std::runtime_error("embedding 向量为空");
        l2Normalize(out);
        g_embed_fail_count = 0;
        g_embed_disabled_until = 0;
        return true;
    } catch (...) {
        // 连续 3 次失败：关闭向量通道 60 秒，检索退化为纯关键词
        if (g_embed_fail_count.fetch_add(1) + 1 >= 3) {
            g_embed_disabled_until = nowMs() + 60000;
            g_embed_fail_count = 0;
        }
        return false;
    }
}

// 会话消息 → 待索引块文本列表（user 起始的一问一答为一块，超长截断）
void buildTurnTexts(const std::vector<Message>& msgs, std::vector<std::pair<int, std::string>>& out) {
    for (size_t i = 0; i < msgs.size(); i++) {
        if (msgs[i].role != "user") continue;
        std::string text = "用户: " + msgs[i].content;
        if (i + 1 < msgs.size() && msgs[i + 1].role == "assistant")
            text += "\n助手: " + msgs[i + 1].content;
        if (text.size() > 3000) text = utf8Truncate(text, 1000); // 粗截断，控制 embedding 成本
        out.push_back({(int)i, text});
    }
}

bool saveVectorIndex() {
    // 调用者需持有 g_index_mutex；原子写，同 sessions.json
    json root = json::array();
    for (auto& c : g_index) {
        json e;
        e["session_id"] = c.session_id;
        e["msg_index"] = c.msg_index;
        e["vec"] = c.vec;
        root.push_back(e);
    }
    try {
        const std::string tmpPath = "vectors.json.tmp";
        {
            std::ofstream file(tmpPath, std::ios::trunc);
            if (!file.is_open()) return false;
            file << root.dump();
            file.flush();
            if (!file.good()) return false;
        }
#ifdef _WIN32
        return MoveFileExA(tmpPath.c_str(), "vectors.json", MOVEFILE_REPLACE_EXISTING) != 0;
#else
        std::remove("vectors.json");
        return std::rename(tmpPath.c_str(), "vectors.json") == 0;
#endif
    } catch (...) { return false; }
}

void loadVectorIndex() {
    std::ifstream file("vectors.json");
    if (!file.is_open()) return;
    try {
        json root;
        file >> root;
        if (!root.is_array()) return;
        std::lock_guard<std::mutex> lock(g_index_mutex);
        for (auto& e : root) {
            Chunk c;
            c.session_id = e.value("session_id", "");
            c.msg_index = e.value("msg_index", 0);
            if (e.contains("vec") && e["vec"].is_array())
                for (auto& v : e["vec"]) c.vec.push_back(v.get<float>());
            if (!c.session_id.empty() && !c.vec.empty()) g_index.push_back(std::move(c));
        }
    } catch (...) {}
}

// 索引单个会话：跳过已有向量的轮次，新轮次逐条调 embedding API
void indexSessionNow(const std::string& sessionId) {
    std::vector<std::pair<int, std::string>> turns;
    {
        std::lock_guard<std::mutex> lock(g_sessions_mutex);
        auto it = g_sessions.find(sessionId);
        if (it == g_sessions.end()) return;
        buildTurnTexts(it->second.messages, turns);
    }
    // 锁内仅快照待索引轮次，阻塞的 embedding 网络调用放到锁外
    std::vector<std::pair<int, std::string>> todo;
    {
        std::lock_guard<std::mutex> lock(g_index_mutex);
        for (auto& [idx, text] : turns) {
            bool exists = false;
            for (auto& c : g_index)
                if (c.session_id == sessionId && c.msg_index == idx) { exists = true; break; }
            if (!exists) todo.push_back({idx, text});
        }
    }
    // 锁外逐条调 embedding（可能耗时数秒），不阻塞检索与其他会话
    std::vector<Chunk> newChunks;
    for (auto& [idx, text] : todo) {
        std::vector<float> vec;
        if (!requestEmbedding(text, vec)) return; // API 不可用，剩余轮次留待下次
        Chunk c; c.session_id = sessionId; c.msg_index = idx;
        c.text = text; c.vec = std::move(vec);
        newChunks.push_back(std::move(c));
    }
    // 锁内仅做追加与落盘
    if (!newChunks.empty()) {
        std::lock_guard<std::mutex> lock(g_index_mutex);
        for (auto& c : newChunks) g_index.push_back(std::move(c));
        saveVectorIndex();
    }
}

// 后台索引队列：单工作线程串行消费，聊天主流程只入队不等待
static std::deque<std::string> g_index_queue;
static std::mutex g_index_queue_mutex;
static std::condition_variable g_index_cv;
static std::atomic<bool> g_index_worker_run{false};

void indexSessionAsync(const std::string& sessionId) {
    {
        std::lock_guard<std::mutex> lock(g_config_mutex);
        if (!g_config.memory_enabled) return;
    }
    {
        std::lock_guard<std::mutex> lock(g_index_queue_mutex);
        g_index_queue.push_back(sessionId);
    }
    g_index_cv.notify_one();
}

void startIndexWorker() {
    if (g_index_worker_run.exchange(true)) return;
    std::thread([]() {
        while (g_index_worker_run.load()) {
            std::string sessionId;
            {
                std::unique_lock<std::mutex> lock(g_index_queue_mutex);
                g_index_cv.wait_for(lock, std::chrono::seconds(1),
                    [] { return !g_index_queue.empty() || !g_index_worker_run.load(); });
                if (!g_index_worker_run.load() && g_index_queue.empty()) break;
                if (g_index_queue.empty()) continue;
                sessionId = g_index_queue.front();
                g_index_queue.pop_front();
            }
            try { indexSessionNow(sessionId); } catch (...) {}
        }
    }).detach();
}

void scheduleIndexRebuildAll() {
    std::vector<std::string> ids;
    {
        std::lock_guard<std::mutex> lock(g_sessions_mutex);
        for (auto& [id, s] : g_sessions) ids.push_back(id);
    }
    {
        std::lock_guard<std::mutex> lock(g_index_queue_mutex);
        for (auto& id : ids) g_index_queue.push_back(id);
    }
    g_index_cv.notify_all();
}

void removeSessionVectors(const std::string& sessionId) {
    std::lock_guard<std::mutex> lock(g_index_mutex);
    g_index.erase(std::remove_if(g_index.begin(), g_index.end(),
        [&](const Chunk& c) { return c.session_id == sessionId; }), g_index.end());
    saveVectorIndex();
}

struct SearchHit {
    std::string session_id;
    int msg_index = 0;
    float score = 0;
    std::string text;
};

struct SearchResult {
    std::vector<SearchHit> hits;
    bool vector_used = false;  // 向量通道是否生效（false = 降级为纯关键词）
    int vector_candidates = 0; // 向量通道候选数
};

// 混合检索：向量语义召回 + 关键词精确召回，RRF 融合
// score = Σ 1/(60 + rank)，两通道分数量纲不可比，用排名倒数融合
SearchResult hybridSearch(const std::string& query, const std::string& excludeSession, int topK) {
    SearchResult out;
    std::vector<Chunk> snapshot;
    {
        std::lock_guard<std::mutex> lock(g_index_mutex);
        for (auto& c : g_index)
            if (c.session_id != excludeSession) snapshot.push_back(c);
    }
    // 块文本不持久化，从会话现取（保证与最新编辑一致）
    {
        std::lock_guard<std::mutex> lock(g_sessions_mutex);
        for (auto& c : snapshot) {
            auto it = g_sessions.find(c.session_id);
            if (it == g_sessions.end()) { c.text.clear(); continue; }
            auto& msgs = it->second.messages;
            if (c.msg_index >= 0 && c.msg_index < (int)msgs.size()) {
                std::vector<std::pair<int, std::string>> turns;
                std::vector<Message> slice(msgs.begin() + c.msg_index, msgs.end());
                buildTurnTexts(slice, turns);
                if (!turns.empty()) c.text = turns[0].second;
            }
        }
    }

    // 向量通道（快照上的纯计算，无锁）
    std::vector<size_t> vecRank;
    std::vector<float> qvec;
    if (requestEmbedding(query, qvec)) {
        std::vector<std::pair<float, size_t>> scored;
        for (size_t i = 0; i < snapshot.size(); i++) {
            if (snapshot[i].vec.empty() || snapshot[i].text.empty()) continue;
            scored.push_back({dotProduct(qvec, snapshot[i].vec), i});
        }
        size_t keep = std::min(scored.size(), (size_t)topK * 3);
        std::partial_sort(scored.begin(), scored.begin() + keep, scored.end(),
            [](auto& a, auto& b) { return a.first > b.first; });
        // 相似度阈值过滤：text-embedding-3-small 下实测相关查询 cos≥0.42、
        // 无关查询 cos≤0.30，取 0.40 分离带，避免弱相关噪声进入上下文
        for (size_t i = 0; i < keep && scored[i].first >= 0.40f; i++)
            vecRank.push_back(scored[i].second);
    }

    // 关键词通道（复用全文搜索的大小写不敏感子串匹配）
    std::vector<size_t> kwRank;
    for (size_t i = 0; i < snapshot.size(); i++) {
        if (!snapshot[i].text.empty() && findNoCase(snapshot[i].text, query) != std::string::npos)
            kwRank.push_back(i);
        if (kwRank.size() >= (size_t)topK * 3) break;
    }

    // RRF 融合
    std::map<size_t, float> fused;
    for (size_t r = 0; r < vecRank.size(); r++) fused[vecRank[r]] += 1.0f / (60.0f + (float)r);
    for (size_t r = 0; r < kwRank.size(); r++) fused[kwRank[r]] += 1.0f / (60.0f + (float)r);

    // 显式构造，避免 map(pair<K,V>) → vector(pair<V,K>) 的迭代器区间转换错乱
    std::vector<std::pair<float, size_t>> ranked;
    ranked.reserve(fused.size());
    for (auto& [idx, sc] : fused) ranked.emplace_back(sc, idx);
    size_t keep = std::min(ranked.size(), (size_t)topK);
    std::partial_sort(ranked.begin(), ranked.begin() + keep, ranked.end(),
        [](auto& a, auto& b) { return a.first > b.first; });

    out.vector_used = !vecRank.empty();
    out.vector_candidates = (int)vecRank.size();
    for (size_t i = 0; i < keep; i++) {
        SearchHit h;
        h.session_id = snapshot[ranked[i].second].session_id;
        h.msg_index = snapshot[ranked[i].second].msg_index;
        h.score = ranked[i].first;
        h.text = snapshot[ranked[i].second].text;
        out.hits.push_back(std::move(h));
    }
    return out;
}

// ============ HTTP 工具 ============
struct HttpRequest {
    std::string method;
    std::string path;
    std::map<std::string, std::string> headers;
    std::string body;
};

struct HttpResponse {
    int status = 200;
    std::map<std::string, std::string> headers;
    std::string body;
};

std::string readFileContent(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return "";
    std::stringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

HttpRequest parseHttpRequest(const std::string& raw) {
    HttpRequest req;
    std::istringstream stream(raw);
    std::string line;
    
    if (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream ls(line);
        ls >> req.method >> req.path;
    }
    
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        size_t pos = line.find(':');
        if (pos != std::string::npos) {
            std::string key = line.substr(0, pos);
            std::string val = line.substr(pos + 1);
            while (!val.empty() && val.front() == ' ') val = val.substr(1);
            std::string lkey = key;
            std::transform(lkey.begin(), lkey.end(), lkey.begin(), ::tolower);
            req.headers[lkey] = val;
        }
    }
    
    std::string remaining;
    while (std::getline(stream, line)) {
        remaining += line + "\n";
    }
    if (!remaining.empty() && remaining.back() == '\n') remaining.pop_back();
    req.body = remaining;
    
    auto it = req.headers.find("content-length");
    if (it != req.headers.end()) {
        int contentLen = std::stoi(it->second);
        size_t headerEnd = raw.find("\r\n\r\n");
        if (headerEnd != std::string::npos) {
            size_t bodyStart = headerEnd + 4;
            if (bodyStart + contentLen <= raw.size()) {
                req.body = raw.substr(bodyStart, contentLen);
            } else if (bodyStart < raw.size()) {
                req.body = raw.substr(bodyStart);
            }
        }
    }
    
    return req;
}

void sendAll(socket_t sock, const std::string& data) {
    size_t offset = 0;
    while (offset < data.size()) {
        size_t chunk = std::min((size_t)4096, data.size() - offset);
        send(sock, data.c_str() + offset, chunk, 0);
        offset += chunk;
    }
}

void sendSseHeaders(socket_t sock) {
    std::string headers = 
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream; charset=utf-8\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: keep-alive\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n";
    sendAll(sock, headers);
}

void sendSseEvent(socket_t sock, const std::string& data) {
    std::string event = "data: " + data + "\n\n";
    sendAll(sock, event);
}

// ============ 工具调用（MCP 风格 tool-calling） ============
// 用 OpenAI tool-calling 的 JSON Schema 声明可被模型调用的工具。
// 当前暴露一个工具 search_memory：对历史会话记忆做混合语义检索（agentic RAG）。
// 检索本身即“工具”，由模型自主决定何时调用，而非在每轮请求前无条件预取。
static json buildToolsSchema() {
    json searchMemory = {
        {"type", "function"},
        {"function", {
            {"name", "search_memory"},
            {"description", "在用户过往的历史对话记忆中检索与当前问题相关的信息。当问题涉及之前聊过的内容、用户偏好、历史决定，或需要跨会话回忆时调用。"},
            {"parameters", {
                {"type", "object"},
                {"properties", {
                    {"query", {{"type", "string"}, {"description", "检索用的关键词或自然语言问题"}}},
                    {"top_k", {{"type", "integer"}, {"description", "返回的最相关条数，默认5，最多10"}}}
                }},
                {"required", json::array({"query"})}
            }}
        }}
    };
    return json::array({searchMemory});
}

struct ToolExecResult {
    std::string content;      // 回填给模型的 tool 消息内容
    int count = 0;            // 命中条数（前端展示用）
    bool vector_used = false; // 向量通道是否生效
    std::string query;        // 实际使用的检索词
};

// 执行 search_memory：稳健解析参数 → hybridSearch → 组织为模型可读文本
static ToolExecResult runSearchMemoryTool(const json& args, const std::string& sessionId) {
    ToolExecResult r;
    if (args.contains("query") && args["query"].is_string())
        r.query = args["query"].get<std::string>();

    int topK = 5;
    if (args.contains("top_k")) {
        if (args["top_k"].is_number()) topK = args["top_k"].get<int>();
        else if (args["top_k"].is_string()) { try { topK = std::stoi(args["top_k"].get<std::string>()); } catch (...) {} }
    }
    if (topK < 1) topK = 1;
    if (topK > 10) topK = 10;

    SearchResult sr = hybridSearch(r.query, sessionId, topK);
    r.count = (int)sr.hits.size();
    r.vector_used = sr.vector_used;

    if (sr.hits.empty()) {
        r.content = "未在历史记忆中找到与该查询相关的内容。";
        return r;
    }
    std::string out = "从历史对话记忆中检索到 " + std::to_string(sr.hits.size()) + " 条相关内容：\n";
    int i = 1;
    for (auto& h : sr.hits)
        out += "[" + std::to_string(i++) + "] " + h.text + "\n";
    r.content = out;
    return r;
}

// ============ API 处理 ============
struct StreamResult {
    std::string full_content;
    int prompt_tokens = 0;
    int completion_tokens = 0;
    int total_tokens = 0;
    std::string error;
};

StreamResult callLLMStream(const std::vector<Message>& history, const std::string& userMsg,
                           const std::string& imageBase64, const std::string& imageMime,
                           bool webSearch, bool appendUserMsg, const std::string& sessionId, socket_t sock) {
    StreamResult result;
    
    // 先复制配置，然后立即释放锁
    std::string apiUrl, apiKey, model;
    bool toolsEnabled;
    {
        std::lock_guard<std::mutex> lock(g_config_mutex);
        apiUrl = g_config.api_url;
        apiKey = g_config.api_key;
        // 有图片时使用视觉模型
        model = (!imageBase64.empty() && !g_config.vision_model.empty()) 
                ? g_config.vision_model : g_config.model;
        // 记忆检索工具：仅在开启记忆且非图片场景启用（视觉模型通常不支持工具调用）
        toolsEnabled = g_config.memory_enabled && imageBase64.empty();
    }
    
    // agentic 循环：最多 MAX_TOOL_ROUNDS 轮工具调用，其后强制生成文本答案
    const int MAX_TOOL_ROUNDS = 4;
    
    try {
        liboai::OpenAI openai(apiUrl);
        if (!openai.auth.SetKey(apiKey)) {
            result.error = "设置 API Key 失败";
            return result;
        }
        
        liboai::Conversation conversation;
        std::string sysPrompt = g_system_prompt;
        if (webSearch) {
            sysPrompt += "\n（当前已开启联网搜索，请结合最新网络信息回答，并注明信息时效性。）";
        }
        if (toolsEnabled) {
            sysPrompt += "\n（你可以调用 search_memory 工具检索用户的历史对话记忆；当问题可能涉及以往聊过的内容、用户偏好或历史决定时，应先检索再作答。）";
        }
        (void)conversation.SetSystemData(sysPrompt);
        
        // 添加历史消息（传入的会话历史，不依赖全局）
        {
            auto& convJson = const_cast<json&>(conversation.GetJSON());
            auto& msgs = convJson["messages"];
            for (auto& msg : history) {
                json m;
                m["role"] = msg.role;
                m["content"] = msg.content;
                msgs.push_back(m);
            }
        }
        
        // 添加当前用户消息（重新生成时历史已含用户消息，不再追加）
        if (appendUserMsg) {
            (void)conversation.AddUserData(userMsg);
        }
        
        // 如果有图片，修改 conversation JSON 支持多模态
        if (!imageBase64.empty()) {
            auto& convJson = const_cast<json&>(conversation.GetJSON());
            auto& messages = convJson["messages"];
            for (int i = (int)messages.size() - 1; i >= 0; i--) {
                if (messages[i]["role"] == "user") {
                    std::string textContent = messages[i]["content"].get<std::string>();
                    messages[i]["content"] = json::array({
                        {{"type", "text"}, {"text", textContent}},
                        {{"type", "image_url"}, {"image_url", {{"url", "data:" + imageMime + ";base64," + imageBase64}}}}
                    });
                    break;
                }
            }
        }
        
        // ============ agentic 循环 ============
        // 每轮：流式请求模型 → 解析内容增量与 tool_calls 增量 →
        //   若模型请求调用工具，则执行工具、把 assistant(tool_calls) 与 tool 结果
        //   追加进会话，再次请求；否则本轮内容即最终答案，退出循环。
        json tools = toolsEnabled ? buildToolsSchema() : json();
        std::string finalContent;      // 最终回答（用于写回会话历史）
        std::string lastResponseContent; // 最后一次响应体（兜底解析用）

        for (int round = 0; round <= MAX_TOOL_ROUNDS; round++) {
            if (g_stop_requested.load()) break;
            bool allowTools = toolsEnabled && round < MAX_TOOL_ROUNDS;

            // 每轮独立的增量累积状态
            std::string roundContent;
            std::string sseBuffer;
            std::map<int, json> toolCalls;  // index -> {id, name, arguments}，arguments 为分片拼接的字符串
            int roundPrompt = 0, roundCompletion = 0, roundTotal = 0;

            auto streamCallback = [&](std::string data, intptr_t, liboai::Conversation&) -> bool {
                if (g_stop_requested.load()) {
                    json sseData; sseData["stopped"] = true;
                    sendSseEvent(sock, safeDump(sseData));
                    return false; // 中止接收
                }
                sseBuffer += data;
                size_t pos;
                while ((pos = sseBuffer.find('\n')) != std::string::npos) {
                    std::string line = sseBuffer.substr(0, pos);
                    sseBuffer.erase(0, pos + 1);
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (line.empty()) continue;
                    if (line.find("[DONE]") != std::string::npos) return true;

                    std::string jsonStr;
                    if (line.size() > 6 && line.compare(0, 6, "data: ") == 0) jsonStr = line.substr(6);
                    else if (line.size() > 5 && line.compare(0, 5, "data:") == 0) jsonStr = line.substr(5);
                    else continue; // 不是 SSE 数据行，跳过

                    try {
                        auto j = json::parse(jsonStr);
                        if (j.contains("choices") && j["choices"].is_array() && !j["choices"].empty()) {
                            auto& choice = j["choices"][0];
                            if (choice.contains("delta") && choice["delta"].is_object()) {
                                auto& delta = choice["delta"];
                                // 文本内容增量
                                if (delta.contains("content") && delta["content"].is_string()) {
                                    std::string d = delta["content"].get<std::string>();
                                    if (!d.empty()) {
                                        roundContent += d;
                                        json sseData; sseData["delta"] = d;
                                        sendSseEvent(sock, safeDump(sseData));
                                    }
                                }
                                // 工具调用增量：id/name 通常首片给出，arguments 分多片下发，按 index 聚合
                                if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
                                    for (auto& tc : delta["tool_calls"]) {
                                        int idx = (tc.contains("index") && tc["index"].is_number()) ? tc["index"].get<int>() : 0;
                                        auto it = toolCalls.find(idx);
                                        if (it == toolCalls.end())
                                            it = toolCalls.emplace(idx, json{{"id", ""}, {"name", ""}, {"arguments", ""}}).first;
                                        json& acc = it->second;
                                        if (tc.contains("id") && tc["id"].is_string())
                                            acc["id"] = tc["id"].get<std::string>();
                                        if (tc.contains("function") && tc["function"].is_object()) {
                                            auto& fn = tc["function"];
                                            if (fn.contains("name") && fn["name"].is_string())
                                                acc["name"] = fn["name"].get<std::string>();
                                            if (fn.contains("arguments") && fn["arguments"].is_string())
                                                acc["arguments"] = acc["arguments"].get<std::string>() + fn["arguments"].get<std::string>();
                                        }
                                    }
                                }
                            }
                        }
                        if (j.contains("usage") && j["usage"].is_object()) {
                            roundPrompt = j["usage"].value("prompt_tokens", 0);
                            roundCompletion = j["usage"].value("completion_tokens", 0);
                            roundTotal = j["usage"].value("total_tokens", 0);
                        }
                    } catch (...) {
                        // 单行 JSON 解析失败，跳过该行
                    }
                }
                return true; // 继续接收数据
            };

            liboai::Response response = openai.ChatCompletion->create(
                model,
                conversation,
                std::nullopt,       // function_call
                0.7f,               // temperature
                std::nullopt,       // top_p
                std::nullopt,       // n
                streamCallback,     // stream
                std::nullopt,       // stop
                std::nullopt,       // max_tokens
                std::nullopt,       // presence_penalty
                std::nullopt,       // frequency_penalty
                std::nullopt,       // logit_bias
                std::nullopt,       // user
                allowTools ? std::optional<json>(tools) : std::optional<json>(std::nullopt)  // tools
            );
            lastResponseContent = response.content;

            // token 统计：本轮若流式未带 usage，尝试从响应体解析；多轮求和
            if (roundTotal == 0) {
                try {
                    auto respJson = json::parse(response.content);
                    if (respJson.contains("usage")) {
                        roundPrompt = respJson["usage"].value("prompt_tokens", 0);
                        roundCompletion = respJson["usage"].value("completion_tokens", 0);
                        roundTotal = respJson["usage"].value("total_tokens", 0);
                    }
                } catch (...) {}
            }
            result.prompt_tokens += roundPrompt;
            result.completion_tokens += roundCompletion;
            result.total_tokens += roundTotal;

            if (g_stop_requested.load()) { finalContent = roundContent; break; }

            // 本轮没有工具调用 → roundContent 即最终答案
            if (toolCalls.empty()) { finalContent = roundContent; break; }

            // 有工具调用：先把 assistant(tool_calls) 消息追加进会话
            auto& msgs = const_cast<json&>(conversation.GetJSON())["messages"];
            std::vector<int> idxs;
            idxs.reserve(toolCalls.size());
            for (auto& [k, v] : toolCalls) idxs.push_back(k);
            std::sort(idxs.begin(), idxs.end());

            json tcArr = json::array();
            for (int idx : idxs) {
                json& acc = toolCalls[idx];
                std::string callId = acc["id"].get<std::string>();
                if (callId.empty()) callId = "call_" + std::to_string(idx) + "_" + std::to_string(nowMs());
                tcArr.push_back({
                    {"id", callId},
                    {"type", "function"},
                    {"function", {{"name", acc["name"].get<std::string>()}, {"arguments", acc["arguments"].get<std::string>()}}}
                });
            }
            json assistantMsg;
            assistantMsg["role"] = "assistant";
            assistantMsg["content"] = roundContent;  // 调用工具时通常为空
            assistantMsg["tool_calls"] = tcArr;
            msgs.push_back(assistantMsg);

            // 逐个执行工具，推送 SSE 事件，并追加 tool 结果消息
            for (auto& tc : tcArr) {
                std::string callId = tc["id"].get<std::string>();
                std::string fnName = tc["function"]["name"].get<std::string>();
                std::string fnArgs = tc["function"]["arguments"].get<std::string>();

                json argsJson = json::object();
                try { argsJson = json::parse(fnArgs); } catch (...) { argsJson = json::object(); }

                {   // tool_start
                    json ev;
                    ev["tool_event"] = "start";
                    ev["tool"] = fnName;
                    ev["args"] = argsJson;
                    sendSseEvent(sock, safeDump(ev));
                }

                std::string toolContent;
                int hitCount = 0; bool vecUsed = false; std::string query;
                if (fnName == "search_memory") {
                    ToolExecResult tr = runSearchMemoryTool(argsJson, sessionId);
                    toolContent = tr.content; hitCount = tr.count; vecUsed = tr.vector_used; query = tr.query;
                } else {
                    toolContent = "未知工具: " + fnName;
                }

                {   // tool_end
                    json ev;
                    ev["tool_event"] = "end";
                    ev["tool"] = fnName;
                    ev["query"] = query;
                    ev["count"] = hitCount;
                    ev["vector_used"] = vecUsed;
                    sendSseEvent(sock, safeDump(ev));
                }

                json toolMsg;
                toolMsg["role"] = "tool";
                toolMsg["tool_call_id"] = callId;
                toolMsg["content"] = toolContent;
                msgs.push_back(toolMsg);
            }

            finalContent = roundContent;  // 保底：若后续未产出文本，用本轮内容
            // 进入下一轮，把工具结果交回模型
        }

        result.full_content = finalContent;

        // 兜底：某些网关不流式返回内容，尝试从最后一次响应体解析
        if (result.full_content.empty() && result.error.empty() && !lastResponseContent.empty()) {
            try {
                auto respJson = json::parse(lastResponseContent);
                if (respJson.contains("choices") && respJson["choices"].is_array() && !respJson["choices"].empty()) {
                    auto& msg = respJson["choices"][0]["message"];
                    if (msg.contains("content") && msg["content"].is_string()) {
                        result.full_content = msg["content"].get<std::string>();
                        json sseData; sseData["delta"] = result.full_content;
                        sendSseEvent(sock, safeDump(sseData));
                    }
                }
            } catch (...) {}
        }
        
    } catch (const liboai::exception::OpenAIException& e) {
        result.error = std::string("[API 错误] ") + e.what();
    } catch (const std::exception& e) {
        result.error = std::string("[异常] ") + e.what();
    }
    
    return result;
}

// ============ HTTP 服务器 ============
class HttpServer {
public:
    HttpServer(int port, const std::string& webDir) 
        : port_(port), webDir_(webDir), running_(false) {}
    
    bool start() {
#ifdef _WIN32
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            std::cerr << "WSAStartup 失败" << std::endl;
            return false;
        }
#endif
        serverSock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (serverSock_ == INVALID_SOCK) {
            std::cerr << "创建 socket 失败" << std::endl;
            return false;
        }
        
        int opt = 1;
        setsockopt(serverSock_, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
        
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port_);
        
        if (bind(serverSock_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            std::cerr << "绑定端口 " << port_ << " 失败" << std::endl;
            CLOSE_SOCKET(serverSock_);
            return false;
        }
        
        if (listen(serverSock_, 10) < 0) {
            std::cerr << "监听失败" << std::endl;
            CLOSE_SOCKET(serverSock_);
            return false;
        }
        
        running_ = true;
        serverThread_ = std::thread([this]() { acceptLoop(); });
        return true;
    }
    
    void stop() {
        running_ = false;
        CLOSE_SOCKET(serverSock_);
        if (serverThread_.joinable()) serverThread_.join();
#ifdef _WIN32
        WSACleanup();
#endif
    }
    
private:
    void acceptLoop() {
        while (running_) {
            struct sockaddr_in clientAddr;
            socklen_t clientLen = sizeof(clientAddr);
            
            struct timeval tv;
            tv.tv_sec = 1;
            tv.tv_usec = 0;
            setsockopt(serverSock_, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
            
            socket_t clientSock = accept(serverSock_, (struct sockaddr*)&clientAddr, &clientLen);
            if (clientSock == INVALID_SOCK) continue;
            
            std::thread([this, clientSock]() {
                handleClient(clientSock);
            }).detach();
        }
    }
    
    void handleClient(socket_t sock) {
        std::string rawData;
        char buffer[8192];
        
        size_t headerEnd = std::string::npos;
        while (running_) {
            struct timeval tv;
            tv.tv_sec = 5;
            tv.tv_usec = 0;
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
            
            int n = recv(sock, buffer, sizeof(buffer) - 1, 0);
            if (n <= 0) break;
            buffer[n] = '\0';
            rawData += buffer;
            
            headerEnd = rawData.find("\r\n\r\n");
            if (headerEnd != std::string::npos) break;
        }
        
        if (headerEnd == std::string::npos) {
            CLOSE_SOCKET(sock);
            return;
        }
        
        HttpRequest tempReq = parseHttpRequest(rawData);
        auto it = tempReq.headers.find("content-length");
        if (it != tempReq.headers.end()) {
            int contentLen = std::stoi(it->second);
            size_t bodyStart = headerEnd + 4;
            size_t bodyReceived = rawData.size() - bodyStart;
            
            while ((int)bodyReceived < contentLen && running_) {
                struct timeval tv;
                tv.tv_sec = 10;
                tv.tv_usec = 0;
                setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
                
                int n = recv(sock, buffer, sizeof(buffer) - 1, 0);
                if (n <= 0) break;
                buffer[n] = '\0';
                rawData += buffer;
                bodyReceived += n;
            }
        }
        
        HttpRequest req = parseHttpRequest(rawData);
        
        // 判断是否需要流式响应
        if (req.method == "POST" && req.path == "/api/chat") {
            handleStreamingChat(sock, req);
        } else {
            HttpResponse resp;
            try {
                resp = routeRequest(req);
            } catch (const std::exception& e) {
                // 兜底：任何异常都不应逃逸到线程顶层（否则 std::terminate 整个进程）
                resp = makeJsonResponse(500, safeDump(json{{"error", std::string("服务器错误: ") + e.what()}}));
            } catch (...) {
                resp = makeJsonResponse(500, "{\"error\":\"服务器内部错误\"}");
            }
            std::string responseStr = 
                "HTTP/1.1 " + std::to_string(resp.status) + " " + getStatusText(resp.status) + "\r\n";
            for (auto& [k, v] : resp.headers) {
                responseStr += k + ": " + v + "\r\n";
            }
            responseStr += "\r\n" + resp.body;
            sendAll(sock, responseStr);
            CLOSE_SOCKET(sock);
        }
    }
    
    void handleStreamingChat(socket_t sock, const HttpRequest& req) {
        sendSseHeaders(sock);
        
        try {
            json j = json::parse(req.body);
            std::string userMsg = j.value("message", "");
            std::string sessionId = j.value("session_id", "");
            std::string action = j.value("action", "send");
            int editIndex = j.value("edit_index", -1);
            bool webSearch = j.value("web_search", false);
            std::string imageBase64, imageMime;
            
            if (j.contains("image") && j["image"].is_object()) {
                imageBase64 = j["image"].value("data", "");
                imageMime = j["image"].value("mime_type", "image/jpeg");
            }
            
            if (sessionId.empty()) {
                json errData; errData["error"] = "缺少 session_id";
                sendSseEvent(sock, safeDump(errData));
                sendSseEvent(sock, "[DONE]");
                CLOSE_SOCKET(sock); return;
            }
            
            // 准备历史与会话状态（锁内只做快速准备，不调 LLM）
            std::vector<Message> history;
            bool appendUserMsg = true;
            bool staleIndex = false;
            {
                std::lock_guard<std::mutex> lock(g_sessions_mutex);
                auto it = g_sessions.find(sessionId);
                if (it == g_sessions.end()) {
                    Session s; s.id = sessionId; s.created_at = nowMs();
                    g_sessions[sessionId] = s;
                    it = g_sessions.find(sessionId);
                }
                Session& sess = it->second;
                
                if (action == "regenerate") {
                    while (!sess.messages.empty() && sess.messages.back().role == "assistant")
                        sess.messages.pop_back();
                    history = sess.messages; // 历史末尾已是用户消息
                    appendUserMsg = false;
                } else if (action == "edit" && editIndex >= 0) {
                    if (editIndex <= (int)sess.messages.size()) sess.messages.resize(editIndex);
                    else sess.messages.clear();
                    history = sess.messages;
                    appendUserMsg = true;
                } else { // send
                    history = sess.messages;
                    appendUserMsg = true;
                }
                
                // 消息被截断后，尾部轮次的向量可能过期，整体重建该会话索引
                if ((action == "regenerate" || action == "edit") && !sess.messages.empty())
                    staleIndex = true;
                
                // 首条消息自动生成标题
                if (sess.title == "新对话" && !userMsg.empty()) {
                    sess.title = utf8Truncate(userMsg, 20);
                }
            }
            
            if (action != "regenerate" && userMsg.empty() && imageBase64.empty()) {
                json errData; errData["error"] = "没有用户消息";
                sendSseEvent(sock, safeDump(errData));
                sendSseEvent(sock, "[DONE]");
                CLOSE_SOCKET(sock); return;
            }
            
            // 复位停止标志
            g_stop_requested = false;
            
            // 调用 LLM（不持锁，避免阻塞其他请求）
            StreamResult result = callLLMStream(history, userMsg, imageBase64, imageMime, webSearch, appendUserMsg, sessionId, sock);
            
            if (!result.error.empty()) {
                json errData; errData["error"] = result.error;
                sendSseEvent(sock, safeDump(errData));
            } else {
                if (result.total_tokens > 0) {
                    json usageData;
                    usageData["usage"] = {
                        {"prompt_tokens", result.prompt_tokens},
                        {"completion_tokens", result.completion_tokens},
                        {"total_tokens", result.total_tokens}
                    };
                    sendSseEvent(sock, safeDump(usageData));
                }
                
                // 写回会话并持久化
                {
                    std::lock_guard<std::mutex> lock(g_sessions_mutex);
                    auto it = g_sessions.find(sessionId);
                    if (it != g_sessions.end()) {
                        Session& sess = it->second;
                        if (action != "regenerate") {
                            Message um; um.role = "user"; um.content = userMsg;
                            um.image_base64 = imageBase64; um.image_mime = imageMime;
                            sess.messages.push_back(um);
                        }
                        Message am; am.role = "assistant"; am.content = result.full_content;
                        am.prompt_tokens = result.prompt_tokens;
                        am.completion_tokens = result.completion_tokens;
                        am.total_tokens = result.total_tokens;
                        sess.messages.push_back(am);
                        saveSessions();
                    }
                }
                
                // 更新记忆索引（异步，不阻塞 SSE 收尾）
                if (staleIndex) removeSessionVectors(sessionId);
                indexSessionAsync(sessionId);
            }
            
        } catch (const std::exception& e) {
            json errData; errData["error"] = std::string("服务器错误: ") + e.what();
            sendSseEvent(sock, safeDump(errData));
        }
        
        // 发送结束标记
        sendAll(sock, "data: [DONE]\n\n");
        CLOSE_SOCKET(sock);
    }
    
    std::string getStatusText(int status) {
        switch (status) {
            case 200: return "OK";
            case 400: return "Bad Request";
            case 404: return "Not Found";
            case 500: return "Internal Server Error";
            default: return "OK";
        }
    }
    
    HttpResponse routeRequest(const HttpRequest& req) {
        if (req.method == "OPTIONS") {
            HttpResponse resp;
            resp.status = 204;
            resp.headers["Access-Control-Allow-Origin"] = "*";
            resp.headers["Access-Control-Allow-Methods"] = "GET, POST, DELETE, OPTIONS";
            resp.headers["Access-Control-Allow-Headers"] = "Content-Type";
            return resp;
        }
        
        if (req.method == "GET" && (req.path == "/" || req.path == "/index.html")) {
            return serveStaticFile(webDir_ + "/index.html", "text/html; charset=utf-8");
        }
        
        if (req.method == "GET" && req.path == "/api/config") {
            std::lock_guard<std::mutex> lock(g_config_mutex);
            json j;
            j["url"] = g_config.api_url;
            j["key"] = g_config.api_key;
            j["model"] = g_config.model;
            j["vision_model"] = g_config.vision_model;
            j["embedding_model"] = g_config.embedding_model;
            j["memory_enabled"] = g_config.memory_enabled;
            return makeJsonResponse(200, j.dump());
        }
        
        if (req.method == "POST" && req.path == "/api/config") {
            try {
                json j = json::parse(req.body);
                std::lock_guard<std::mutex> lock(g_config_mutex);
                if (j.contains("url") && j["url"].is_string()) 
                    g_config.api_url = j["url"].get<std::string>();
                if (j.contains("key") && j["key"].is_string()) 
                    g_config.api_key = j["key"].get<std::string>();
                if (j.contains("model") && j["model"].is_string()) 
                    g_config.model = j["model"].get<std::string>();
                if (j.contains("vision_model") && j["vision_model"].is_string()) 
                    g_config.vision_model = j["vision_model"].get<std::string>();
                if (j.contains("embedding_model") && j["embedding_model"].is_string()) 
                    g_config.embedding_model = j["embedding_model"].get<std::string>();
                if (j.contains("memory_enabled") && j["memory_enabled"].is_boolean()) 
                    g_config.memory_enabled = j["memory_enabled"].get<bool>();
                saveConfig("config.txt", g_config);
                return makeJsonResponse(200, "{\"ok\":true}");
            } catch (const std::exception& e) {
                return makeJsonResponse(400, std::string("{\"error\":\"") + e.what() + "\"}");
            }
        }
        
        if (req.method == "POST" && req.path == "/api/clear") {
            std::string sessionId;
            try { json j = json::parse(req.body); sessionId = j.value("session_id", ""); } catch (...) {}
            {
                std::lock_guard<std::mutex> lock(g_sessions_mutex);
                auto it = g_sessions.find(sessionId);
                if (it != g_sessions.end()) {
                    it->second.messages.clear();
                    saveSessions();
                }
            }
            removeSessionVectors(sessionId);
            return makeJsonResponse(200, "{\"ok\":true}");
        }
        
        // 停止当前生成
        if (req.method == "POST" && req.path == "/api/stop") {
            g_stop_requested = true;
            return makeJsonResponse(200, "{\"ok\":true}");
        }
        
        // 记忆检索调试接口：GET /api/debug/search?q=xxx&top_k=5
        if (req.method == "GET" && req.path.rfind("/api/debug/search", 0) == 0) {
            std::string q = getQueryParam(req.path, "q");
            int topK = 5;
            try { topK = std::stoi(getQueryParam(req.path, "top_k")); } catch (...) {}
            if (topK <= 0 || topK > 20) topK = 5;
            json j;
            j["query"] = q;
            j["results"] = json::array();
            size_t indexSize = 0;
            {
                std::lock_guard<std::mutex> lock(g_index_mutex);
                indexSize = g_index.size();
            }
            j["index_size"] = (int)indexSize;
            if (!q.empty()) {
                auto sr = hybridSearch(q, "", topK);
                j["vector_used"] = sr.vector_used;
                j["vector_candidates"] = sr.vector_candidates;
                for (auto& h : sr.hits) {
                    json e;
                    e["session_id"] = h.session_id;
                    e["msg_index"] = h.msg_index;
                    e["score"] = std::to_string(h.score);
                    {
                        std::lock_guard<std::mutex> lock(g_sessions_mutex);
                        auto it = g_sessions.find(h.session_id);
                        e["title"] = (it != g_sessions.end()) ? it->second.title : "";
                    }
                    e["text"] = utf8Truncate(h.text, 200);
                    j["results"].push_back(e);
                }
            }
            return makeJsonResponse(200, j.dump());
        }
        
        // 全文搜索：按会话分组，一个会话内多处命中合并为一条结果
        if (req.method == "GET" && req.path.rfind("/api/search", 0) == 0) {
            std::string q = getQueryParam(req.path, "q");
            json results = json::array();
            if (!q.empty()) {
                std::lock_guard<std::mutex> lock(g_sessions_mutex);
                for (auto& [id, s] : g_sessions) {
                    if (results.size() >= 30) break;
                    json matches = json::array();
                    int matchCount = 0;
                    // 标题匹配
                    size_t tpos = findNoCase(s.title, q);
                    if (tpos != std::string::npos) {
                        json m;
                        m["msg_index"] = -1; m["role"] = "title";
                        m["snippet"] = makeSnippet(s.title, tpos, q.size(), 40);
                        matches.push_back(m);
                        matchCount++;
                    }
                    // 消息内容匹配（片段最多展示5条，计数统计全部）
                    for (size_t i = 0; i < s.messages.size(); i++) {
                        size_t pos = findNoCase(s.messages[i].content, q);
                        if (pos == std::string::npos) continue;
                        matchCount++;
                        if (matches.size() < 6) {
                            json m;
                            m["msg_index"] = (int)i; m["role"] = s.messages[i].role;
                            m["snippet"] = makeSnippet(s.messages[i].content, pos, q.size());
                            matches.push_back(m);
                        }
                    }
                    if (matchCount == 0) continue;
                    json e;
                    e["session_id"] = id; e["title"] = s.title;
                    e["created_at"] = s.created_at;
                    e["match_count"] = matchCount;
                    e["matches"] = matches;
                    results.push_back(e);
                }
                // 按会话创建时间倒序
                std::vector<json> v(results.begin(), results.end());
                std::sort(v.begin(), v.end(), [](const json& a, const json& b) {
                    return a.value("created_at", 0LL) > b.value("created_at", 0LL);
                });
                results = json::array();
                for (auto& e : v) results.push_back(e);
            }
            json j; j["query"] = q; j["results"] = results;
            return makeJsonResponse(200, j.dump());
        }
        
        // 会话列表（按创建时间倒序）
        if (req.method == "GET" && req.path == "/api/sessions") {
            std::lock_guard<std::mutex> lock(g_sessions_mutex);
            std::vector<Session*> list;
            for (auto& [id, s] : g_sessions) list.push_back(&s);
            std::sort(list.begin(), list.end(), [](Session* a, Session* b) {
                return a->created_at > b->created_at;
            });
            json arr = json::array();
            for (auto* s : list) {
                json e;
                e["id"] = s->id;
                e["title"] = s->title;
                e["created_at"] = s->created_at;
                e["message_count"] = s->messages.size();
                arr.push_back(e);
            }
            json j; j["sessions"] = arr;
            return makeJsonResponse(200, j.dump());
        }
        
        // 新建会话
        if (req.method == "POST" && req.path == "/api/sessions") {
            Session s;
            s.id = genId();
            s.created_at = nowMs();
            {
                std::lock_guard<std::mutex> lock(g_sessions_mutex);
                g_sessions[s.id] = s;
                saveSessions();
            }
            json j; j["id"] = s.id;
            return makeJsonResponse(200, j.dump());
        }
        
        // 单个会话：GET 获取消息 / DELETE 删除
        if (req.path.rfind("/api/sessions/", 0) == 0) {
            std::string id = req.path.substr(14);
            if (req.method == "GET") {
                std::lock_guard<std::mutex> lock(g_sessions_mutex);
                auto it = g_sessions.find(id);
                if (it == g_sessions.end()) return makeJsonResponse(404, "{\"error\":\"会话不存在\"}");
                Session& s = it->second;
                json j;
                j["id"] = s.id; j["title"] = s.title; j["created_at"] = s.created_at;
                json msgs = json::array();
                for (auto& m : s.messages) {
                    json mm;
                    mm["role"] = m.role;
                    mm["content"] = m.content;
                    if (!m.image_base64.empty()) {
                        mm["image_data"] = m.image_base64;
                        mm["image_mime"] = m.image_mime;
                    }
                    if (m.total_tokens > 0) mm["total_tokens"] = m.total_tokens;
                    msgs.push_back(mm);
                }
                j["messages"] = msgs;
                return makeJsonResponse(200, j.dump());
            }
            if (req.method == "DELETE") {
                {
                    std::lock_guard<std::mutex> lock(g_sessions_mutex);
                    g_sessions.erase(id);
                    saveSessions();
                }
                removeSessionVectors(id);
                return makeJsonResponse(200, "{\"ok\":true}");
            }
        }
        
        // 返回可用模型列表
        if (req.method == "GET" && req.path == "/api/models") {
            std::string modelsJson = R"({
  "chat": [
    {"id":"gpt-4o","name":"GPT-4o","desc":"OpenAI 旗舰，全能"},
    {"id":"gpt-4o-mini","name":"GPT-4o Mini","desc":"性价比高，速度快"},
    {"id":"gpt-4.1-mini","name":"GPT-4.1 Mini","desc":"最新一代，支持识图"},
    {"id":"gpt-4.1-nano","name":"GPT-4.1 Nano","desc":"最便宜，简单任务"},
    {"id":"gpt-3.5-turbo","name":"GPT-3.5 Turbo","desc":"经典模型，经济实惠"},
    {"id":"claude-sonnet-4-20250514","name":"Claude Sonnet 4","desc":"Anthropic 主力，写作强"},
    {"id":"deepseek-chat","name":"DeepSeek V3","desc":"国产之光，能力强"},
    {"id":"deepseek-v3.2","name":"DeepSeek V3.2","desc":"最新版，全面提升"},
    {"id":"qwen-plus","name":"通义千问 Plus","desc":"阿里旗舰，均衡"},
    {"id":"qwen-max","name":"通义千问 Max","desc":"阿里最强"},
    {"id":"qwen-turbo","name":"通义千问 Turbo","desc":"快速经济"},
    {"id":"MiniMax-M2.5","name":"MiniMax M2.5","desc":"编程办公强"},
    {"id":"doubao-seed-2-0-lite-260428","name":"豆包 Seed 2.0","desc":"字节全能模型"}
  ],
  "reason": [
    {"id":"deepseek-r1","name":"DeepSeek R1","desc":"推理之王"},
    {"id":"deepseek-r1-0528","name":"DeepSeek R1 0528","desc":"R1 最新版"}
  ],
  "vision": [
    {"id":"gpt-4o","name":"GPT-4o","desc":"最佳视觉理解"},
    {"id":"gpt-4o-mini","name":"GPT-4o Mini","desc":"快速识图"},
    {"id":"gpt-4.1-mini","name":"GPT-4.1 Mini","desc":"最新识图"},
    {"id":"qwen-vl-max","name":"通义 VL Max","desc":"阿里视觉最强"},
    {"id":"qwen-vl-plus","name":"通义 VL Plus","desc":"阿里视觉均衡"},
    {"id":"deepseek-v3.2","name":"DeepSeek V3.2","desc":"DeepSeek 识图"}
  ]
})";
            return makeJsonResponse(200, modelsJson);
        }
        
        return makeJsonResponse(404, "{\"error\":\"Not Found\"}");
    }
    
    HttpResponse makeJsonResponse(int status, const std::string& jsonStr) {
        HttpResponse resp;
        resp.status = status;
        resp.headers["Content-Type"] = "application/json; charset=utf-8";
        resp.headers["Access-Control-Allow-Origin"] = "*";
        resp.headers["Access-Control-Allow-Methods"] = "GET, POST, DELETE, OPTIONS";
        resp.headers["Access-Control-Allow-Headers"] = "Content-Type";
        resp.headers["Content-Length"] = std::to_string(jsonStr.size());
        resp.body = jsonStr;
        return resp;
    }
    
    HttpResponse serveStaticFile(const std::string& path, const std::string& contentType) {
        std::string content = readFileContent(path);
        if (content.empty()) {
            return makeJsonResponse(404, "{\"error\":\"File not found\"}");
        }
        HttpResponse resp;
        resp.status = 200;
        resp.headers["Content-Type"] = contentType;
        resp.headers["Access-Control-Allow-Origin"] = "*";
        resp.headers["Content-Length"] = std::to_string(content.size());
        resp.body = content;
        return resp;
    }
    
    int port_;
    std::string webDir_;
    std::atomic<bool> running_;
    socket_t serverSock_;
    std::thread serverThread_;
};

// ============ 全局状态 ============
static int g_port = 0;

void openBrowser(int port) {
    std::string url = "http://localhost:" + std::to_string(port);
#ifdef _WIN32
    ShellExecuteA(NULL, "open", url.c_str(), NULL, NULL, SW_SHOWNORMAL);
#elif __APPLE__
    system(("open " + url).c_str());
#else
    system(("xdg-open " + url).c_str());
#endif
}

// ============ Windows 托盘模式 ============
#ifdef _WIN32

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_TRAYICON:
        if (lParam == WM_RBUTTONUP) {
            POINT pt;
            GetCursorPos(&pt);
            HMENU hMenu = CreatePopupMenu();
            AppendMenuA(hMenu, MF_STRING, IDM_OPEN_BROWSER, "Open Browser");
            AppendMenuA(hMenu, MF_SEPARATOR, 0, NULL);
            AppendMenuA(hMenu, MF_STRING, IDM_EXIT, "Exit");
            SetForegroundWindow(hwnd);
            TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, NULL);
            DestroyMenu(hMenu);
        } else if (lParam == WM_LBUTTONDBLCLK) {
            openBrowser(g_port);
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wParam) == IDM_OPEN_BROWSER) {
            openBrowser(g_port);
        } else if (LOWORD(wParam) == IDM_EXIT) {
            g_running = false;
            Shell_NotifyIconA(NIM_DELETE, &g_nid);
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

int WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    // 加载配置
    loadConfig("config.txt", g_config);
    if (g_config.api_key.empty()) {
        // 没有配置，弹出错误提示后退出
        MessageBoxA(NULL, "config.txt not found or API Key is empty.\nPlease create config.txt first.", "AI Assistant", MB_ICONWARNING);
        return 0;
    }
    
    // 加载历史会话
    loadSessions();
    
    // 加载记忆向量索引并启动后台索引线程，补建缺失向量
    loadVectorIndex();
    startIndexWorker();
    scheduleIndexRebuildAll();
    
    // 查找 web 目录
    std::string webDir = "web";
    {
        std::ifstream testFile(webDir + "/index.html");
        if (!testFile.is_open()) {
            std::ifstream testFile2("../web/index.html");
            if (testFile2.is_open()) {
                webDir = "../web";
            } else {
                MessageBoxA(NULL, "Cannot find web/index.html!", "AI Assistant", MB_ICONERROR);
                return 0;
            }
        }
    }
    
    // 启动 HTTP 服务器
    int ports[] = {8080, 8888, 9090, 18080};
    HttpServer* server = nullptr;
    int usedPort = 0;
    for (int p : ports) {
        auto* s = new HttpServer(p, webDir);
        if (s->start()) {
            server = s;
            usedPort = p;
            break;
        }
        delete s;
    }
    if (!server) {
        MessageBoxA(NULL, "Cannot start server. Ports may be in use.", "AI Assistant", MB_ICONERROR);
        return 0;
    }
    g_port = usedPort;
    
    // 创建隐藏窗口
    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = "AIAssistantTray";
    RegisterClassExA(&wc);
    
    g_hwnd = CreateWindowExA(0, "AIAssistantTray", "AI Assistant", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, hInst, NULL);
    
    // 创建托盘图标
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = ID_TRAYICON;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    snprintf(g_nid.szTip, sizeof(g_nid.szTip), "AI Assistant - Port %d", usedPort);
    Shell_NotifyIconA(NIM_ADD, &g_nid);
    
    // 自动打开浏览器
    openBrowser(usedPort);
    
    // 消息循环
    MSG msg;
    while (g_running) {
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { g_running = false; break; }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (!g_running) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    
    server->stop();
    delete server;
    return 0;
}

#else
// ============ Linux/Mac 控制台模式 ============
int main(int argc, char* argv[]) {
    loadConfig("config.txt", g_config);
    if (g_config.api_key.empty()) {
        std::cerr << "API Key is empty in config.txt!" << std::endl;
        return 1;
    }
    
    // 加载历史会话
    loadSessions();
    
    // 加载记忆向量索引并启动后台索引线程，补建缺失向量
    loadVectorIndex();
    startIndexWorker();
    scheduleIndexRebuildAll();
    
    std::string webDir = "web";
    {
        std::ifstream testFile(webDir + "/index.html");
        if (!testFile.is_open()) {
            std::ifstream testFile2("../web/index.html");
            if (testFile2.is_open()) webDir = "../web";
            else { std::cerr << "Cannot find web/index.html!" << std::endl; return 1; }
        }
    }
    
    int ports[] = {8080, 8888, 9090, 18080};
    HttpServer* server = nullptr;
    int usedPort = 0;
    for (int p : ports) {
        auto* s = new HttpServer(p, webDir);
        if (s->start()) { server = s; usedPort = p; break; }
        delete s;
    }
    if (!server) { std::cerr << "Cannot start server!" << std::endl; return 1; }
    
    std::cout << "Server running at http://localhost:" << usedPort << std::endl;
    openBrowser(usedPort);
    
    while (true) std::this_thread::sleep_for(std::chrono::seconds(1));
    server->stop();
    delete server;
    return 0;
}
#endif

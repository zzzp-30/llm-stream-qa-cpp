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

// ============ 配置管理 ============
struct Config {
    std::string api_url = "https://api.openai.com/v1";
    std::string api_key;
    std::string model = "gpt-3.5-turbo";
    std::string vision_model = "gpt-4o";  // 视觉模型，用于图片识别
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
                           bool webSearch, bool appendUserMsg, socket_t sock) {
    StreamResult result;
    
    // 先复制配置，然后立即释放锁
    std::string apiUrl, apiKey, model;
    {
        std::lock_guard<std::mutex> lock(g_config_mutex);
        apiUrl = g_config.api_url;
        apiKey = g_config.api_key;
        // 有图片时使用视觉模型
        model = (!imageBase64.empty() && !g_config.vision_model.empty()) 
                ? g_config.vision_model : g_config.model;
    }
    
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
        
        // 流式回调 - 正确解析 SSE 数据（带持久缓冲区处理 chunk 截断）
        std::string sseBuffer;
        auto streamCallback = [&result, &sock, &sseBuffer](std::string data, intptr_t, liboai::Conversation& conv) -> bool {
            // 检测停止请求
            if (g_stop_requested.load()) {
                json sseData;
                sseData["stopped"] = true;
                sendSseEvent(sock, sseData.dump());
                return false; // 中止接收
            }
            sseBuffer += data;
            // 处理缓冲区中完整的行
            size_t pos;
            while ((pos = sseBuffer.find('\n')) != std::string::npos) {
                std::string line = sseBuffer.substr(0, pos);
                sseBuffer.erase(0, pos + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) continue;
                if (line.find("[DONE]") != std::string::npos) return true;
                
                std::string jsonStr;
                if (line.size() > 6 && line.substr(0, 6) == "data: ") {
                    jsonStr = line.substr(6);
                } else if (line.size() > 5 && line.substr(0, 5) == "data:") {
                    jsonStr = line.substr(5);
                } else {
                    continue; // 不是 SSE 数据行，跳过
                }
                
                try {
                    auto j = json::parse(jsonStr);
                    if (j.contains("choices") && j["choices"].is_array() && !j["choices"].empty()) {
                        auto& choice = j["choices"][0];
                        std::string delta;
                        if (choice.contains("delta") && choice["delta"].contains("content")) {
                            delta = choice["delta"]["content"].get<std::string>();
                        } else if (choice.contains("text")) {
                            delta = choice["text"].get<std::string>();
                        }
                        
                        if (!delta.empty()) {
                            result.full_content += delta;
                            json sseData;
                            sseData["delta"] = delta;
                            sendSseEvent(sock, sseData.dump());
                        }
                    }
                    if (j.contains("usage")) {
                        result.prompt_tokens = j["usage"].value("prompt_tokens", 0);
                        result.completion_tokens = j["usage"].value("completion_tokens", 0);
                        result.total_tokens = j["usage"].value("total_tokens", 0);
                    }
                } catch (...) {
                    // JSON 解析失败，跳过该行
                }
            }
            return true; // 继续接收数据
        };
        
        // 真实流式调用
        liboai::Response response = openai.ChatCompletion->create(
            model,
            conversation,
            std::nullopt,       // function_call
            0.7f,               // temperature
            std::nullopt,       // top_p
            std::nullopt,       // n
            streamCallback,     // stream
            std::nullopt,       // stop
            std::nullopt        // max_tokens
        );
        
        // 如果流式回调没有拿到 usage，尝试从 response 提取
        if (result.total_tokens == 0) {
            try {
                auto respJson = json::parse(response.content);
                if (respJson.contains("usage")) {
                    result.prompt_tokens = respJson["usage"].value("prompt_tokens", 0);
                    result.completion_tokens = respJson["usage"].value("completion_tokens", 0);
                    result.total_tokens = respJson["usage"].value("total_tokens", 0);
                }
            } catch (...) {}
        }
        
        // 更新对话历史（流式回调已通过 AppendStreamData 更新，这里不再重复 Update）
        // 注意：流式模式下 conversation 已在回调中通过 AppendStreamData 更新
        
        if (result.full_content.empty() && result.error.empty()) {
            // 流式没有产出内容，尝试从 conversation 获取
            std::string lastResp = conversation.GetLastResponse();
            if (!lastResp.empty()) {
                result.full_content = lastResp;
                // 一次性推送
                json sseData;
                sseData["delta"] = result.full_content;
                sendSseEvent(sock, sseData.dump());
            }
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
            HttpResponse resp = routeRequest(req);
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
                sendSseEvent(sock, errData.dump());
                sendSseEvent(sock, "[DONE]");
                CLOSE_SOCKET(sock); return;
            }
            
            // 准备历史与会话状态（锁内只做快速准备，不调 LLM）
            std::vector<Message> history;
            bool appendUserMsg = true;
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
                
                // 首条消息自动生成标题
                if (sess.title == "新对话" && !userMsg.empty()) {
                    sess.title = utf8Truncate(userMsg, 20);
                }
            }
            
            if (action != "regenerate" && userMsg.empty() && imageBase64.empty()) {
                json errData; errData["error"] = "没有用户消息";
                sendSseEvent(sock, errData.dump());
                sendSseEvent(sock, "[DONE]");
                CLOSE_SOCKET(sock); return;
            }
            
            // 复位停止标志
            g_stop_requested = false;
            
            // 调用 LLM（不持锁，避免阻塞其他请求）
            StreamResult result = callLLMStream(history, userMsg, imageBase64, imageMime, webSearch, appendUserMsg, sock);
            
            if (!result.error.empty()) {
                json errData; errData["error"] = result.error;
                sendSseEvent(sock, errData.dump());
            } else {
                if (result.total_tokens > 0) {
                    json usageData;
                    usageData["usage"] = {
                        {"prompt_tokens", result.prompt_tokens},
                        {"completion_tokens", result.completion_tokens},
                        {"total_tokens", result.total_tokens}
                    };
                    sendSseEvent(sock, usageData.dump());
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
            }
            
        } catch (const std::exception& e) {
            json errData; errData["error"] = std::string("服务器错误: ") + e.what();
            sendSseEvent(sock, errData.dump());
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
                saveConfig("config.txt", g_config);
                return makeJsonResponse(200, "{\"ok\":true}");
            } catch (const std::exception& e) {
                return makeJsonResponse(400, std::string("{\"error\":\"") + e.what() + "\"}");
            }
        }
        
        if (req.method == "POST" && req.path == "/api/clear") {
            std::string sessionId;
            try { json j = json::parse(req.body); sessionId = j.value("session_id", ""); } catch (...) {}
            std::lock_guard<std::mutex> lock(g_sessions_mutex);
            auto it = g_sessions.find(sessionId);
            if (it != g_sessions.end()) {
                it->second.messages.clear();
                saveSessions();
            }
            return makeJsonResponse(200, "{\"ok\":true}");
        }
        
        // 停止当前生成
        if (req.method == "POST" && req.path == "/api/stop") {
            g_stop_requested = true;
            return makeJsonResponse(200, "{\"ok\":true}");
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
                std::lock_guard<std::mutex> lock(g_sessions_mutex);
                g_sessions.erase(id);
                saveSessions();
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

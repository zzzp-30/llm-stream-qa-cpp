# Agentic Stream QA — 流式智能体问答系统

基于 C++ 与 [liboai](https://github.com/D7EAD/liboai) 的轻量级流式问答系统：C++ 手写 HTTP 服务器 + SSE 流式推送，支持多厂商模型切换、图片多模态识别、多会话管理、**Agentic RAG 记忆检索**与**联网搜索**（二者均作为工具由模型自主调用），最终打包为免安装的 Windows 桌面应用。

## 功能特性

- **流式输出**：SSE 协议逐字推送，服务端持久缓冲状态机保证跨包分片解析鲁棒
- **多模态识别**：图片输入自动路由至视觉模型（Vision API），前端自动压缩编码
- **多会话管理**：会话隔离、消息编辑、对话重生成、生成中止，JSON 持久化（临时文件 + 原子替换），支持跨重启恢复
- **历史全文搜索**：侧边栏搜索框实时检索所有会话的标题与消息内容，命中关键词高亮，点击结果直接跳转并定位到对应消息
- **Agentic RAG 记忆检索**：把「检索历史记忆」封装为 OpenAI tool-calling 工具 `search_memory`，由模型**自主决定何时调用**；向量语义 + 关键词双通道 RRF 融合检索，后台异步建索引，前端实时展示检索过程卡片（详见下文专章）
- **联网搜索**：把「实时联网检索」封装为工具 `web_search`，模型遇到新闻 / 天气 / 股价 / 赛事等实时性问题时**自主调用**；基于 WinHTTP 抓取必应搜索结果页并解析，无需任何付费搜索 API（详见下文专章）
- **多厂商兼容**：配置驱动，运行时热切换任意 OpenAI 兼容 API（通义 / DeepSeek / Kimi 等）
- **模型测试**：批量实测候选模型可用性，区分限流与真实不可用
- **前端体验**：深色模式、语音输入、拖拽上传、Markdown/PDF 导出、代码行号高亮
- **桌面化**：PE 补丁生成无控制台 GUI 版，支持系统托盘，一键打包分发

## 系统架构

```
浏览器 (web/index.html)
   │ ① POST /api/chat
   ▼
HttpServer（Winsock 手写：HTTP 解析 / 每连接一线程 / SSE 推送）
   │ ② 锁内拷贝会话快照 → 锁外调用
   ▼
callLLMStream（编排层：历史注入 / 多模态构造 / 模型动态路由 /
   │           sseBuffer 分行状态机 / 停止标志检测）
   │ ③ ChatCompletion->create()
   ▼
liboai → libcurl → HTTPS ──→ 大模型 API
   ▲ ④ 数据块流式回调
   │ ⑤ SSE 逐字下发 → 结束后加锁写回 + 原子持久化
```

**应用层三层结构**（`qa_app/main.cpp`）：

| 层 | 组件 | 职责 |
|---|---|---|
| 推送服务层 | `HttpServer` | Winsock HTTP/SSE 协议、路由、静态页服务 |
| 会话状态层 | `Session` / `Message` | 多会话上下文、并发锁、JSON 持久化 |
| 结果抽象层 | `StreamResult` | 流式结果与结构化错误统一封装 |

模型接入层复用开源库 **liboai**（MIT 协议），本项目聚焦其上应用层的设计与实现。为支持工具调用，对 liboai 做了**向后兼容的最小扩展**：`ChatCompletion::create` / `create_async` 追加末位可选参数 `std::optional<nlohmann::json> tools`，仅当传入非空数组时注入请求体，不影响任何现有调用点。

## Agentic 工具调用（记忆检索 + 联网搜索）

区别于「每轮请求前无条件预取上下文」的传统 RAG，本项目把检索做成**模型可自主调用的工具**，形成 agentic 循环：模型判断问题是否需要回忆历史 → 发起 `search_memory` 工具调用 → 后端执行混合检索并回填结果 → 模型基于检索结果作答。

**工具声明（MCP 风格 JSON Schema）**：`search_memory` 暴露 `query`（必填）与 `top_k`（1–10，默认 5）两个参数，随请求以 OpenAI tool-calling 格式下发。

**Agentic 循环**（`callLLMStream`）：最多 `MAX_TOOL_ROUNDS = 4` 轮。每轮用带持久缓冲的状态机解析 SSE，分别累积 `delta.content`（实时逐字下发前端）与 `delta.tool_calls`（按 `index` 聚合分片的 `id` / `name` / `arguments`）。本轮若有工具调用，则把 `assistant(tool_calls)` 与每个 `role:"tool"` 结果消息追加进会话后再次请求；否则本轮内容即最终答案。token 用量跨轮累加。

**混合检索**（`hybridSearch`）：

| 通道 | 召回方式 | 说明 |
|---|---|---|
| 向量语义 | embedding 余弦相似度 | 向量 L2 归一化后点积即余弦；阈值 0.40 过滤弱相关噪声 |
| 关键词 | 大小写不敏感子串匹配 | 精确命中，弥补向量对专有名词的召回不足 |

两通道分数量纲不可比，用 **RRF（Reciprocal Rank Fusion）** 融合：`score = Σ 1/(60 + rank)`，取融合后 Top-K。

**索引与降级**：以「一问一答」为一个 chunk；后台**单工作线程**串行消费索引队列，聊天主流程只入队不等待（阻塞的 embedding 网络调用全程在锁外）；向量索引 `vectors.json` 原子落盘，启动时加载并补建缺失向量。embedding 通道带**熔断降级**：连续 3 次失败则关闭向量通道 60 秒，检索自动退化为纯关键词，冷却后重试。编辑 / 重生成 / 删除 / 清空会话时同步失效对应向量。

**前端可视化**：模型发起工具调用时，AI 气泡顶部以「深度思考」式小字块实时展示检索过程——`tool_event:start` 显示转圈行「🧠 正在记忆检索：{query}」或「🌐 正在联网搜索：{query}」，`tool_event:end` 变为带 ✓ 的「🌐 联网搜索「{query}」· N 条结果」；记忆检索与联网搜索用不同图标区分，历史重绘后会重新注入本轮气泡，保证可见。

**开关**：工具仅在非图片场景启用（视觉模型通常不支持 tool-calling）；`search_memory` 由 `memory_enabled=1` 控制，可在页面「⚙️ API 设置」中开关并配置 embedding 模型；`web_search` 由前端「联网搜索」开关按请求下发，随开随用。

### 联网搜索（web_search）

**触发**：系统提示词引导模型在遇到新闻、天气、股价、体育赛事、科技动态等需要实时数据的问题时优先调用 `web_search`；纯回忆类问题走 `search_memory`，二者互不干扰。

**实现**：后端用 Windows 原生 **WinHTTP** 直接请求 `https://www.bing.com/search?q=...&setlang=zh-CN`（携带浏览器 UA 与 `Accept-Language` 头），取回 HTML 后解析 `<li class="b_algo">` 结果块，抽取标题 / 链接 / 摘要，拼成文本作为 `role:"tool"` 消息回填给模型作答。整个过程不依赖任何收费搜索 API。

> 说明：DuckDuckGo / Google 在国内网络下超时不可达，故选用国内可直连的必应。搜索本身免费、无需 key；唯一成本是搜索结果注入上下文后带来的额外 LLM token（联网轮的 prompt_tokens 通常数千，普通对话仅数百）。

## 项目结构

```
├── liboai/           # 开源接入库（第三方，MIT）
├── qa_app/           # 本应用（全部自研）
│   ├── main.cpp      # 后端：HTTP 服务器 + 会话 + 流式编排
│   ├── web/          # 前端页面
│   ├── build.bat     # 一键编译（MSYS2 MinGW）
│   ├── package.bat   # 一键打包（DLL 依赖 + 分发包）
│   ├── patch_gui.ps1 # PE 子系统补丁（控制台→GUI）
│   └── config.example.txt
└── documentation/    # liboai 官方文档与示例
```

## 构建（Windows）

前置条件：[MSYS2](https://www.msys64.org/)（ucrt64 工具链）、vcpkg（CURL / nlohmann_json / ZLIB）。

```bat
:: 在 MSYS2 ucrt64 环境下
cd qa_app
build.bat
```

> 注意：构建环境需保证 `C:\msys64\ucrt64\bin` 在 PATH 中，否则编译器会因缺少运行时 DLL 静默失败。

## 运行

```bat
cd qa_app\build
qa_app.exe
```

1. 将 `config.example.txt` 复制为 `config.txt` 并与可执行文件放在同一目录，填入你的 API 地址与密钥；
2. 启动后自动打开浏览器（默认 `http://localhost:8080`）；
3. 也可在页面内通过"模型选择器"切换模型，配置会热更新并落盘。

`config.txt` 关键项：

| 键 | 默认值 | 说明 |
|---|---|---|
| `api_url` / `api_key` | — | OpenAI 兼容接口地址与密钥 |
| `model` / `vision_model` | `gpt-3.5-turbo` / `gpt-4o` | 文本模型 / 图片识别用的视觉模型 |
| `embedding_model` | `text-embedding-3-small` | 记忆检索用的 embedding 模型 |
| `memory_enabled` | `1` | 记忆检索（Agentic RAG）总开关，`0` 关闭 |

## 部署到其他电脑（免安装）

目标电脑要求：Windows 10/11 x64，**无需安装任何运行环境**。

1. 将本仓库的 `qa_app/deploy/` 整个文件夹拷贝到目标电脑（可用 U 盘 / 网盘 / 压缩包），它包含：
   - `qa_app.exe` — 主程序（GUI 子系统，双击即启动，无控制台窗口）
   - 全部依赖 DLL（libcurl / nlohmann_json / OpenSSL 等，已随包携带）
   - `web/` — 前端页面
   - `config.txt` — 配置文件（**拷贝前请清空 api_key，让对方填自己的密钥**）
2. 在目标电脑上编辑 `config.txt`，填入 API 地址与密钥；
3. 双击 `qa_app.exe`，浏览器自动打开 `http://localhost:8080`（若未自动打开，手动访问即可）。

> 如需重新生成分发包：在本机运行 `qa_app/package.bat`，会自动收集 exe 与全部依赖 DLL 到 deploy 目录。`sessions.json`（聊天历史）与 `vectors.json`（记忆索引）属运行期用户数据，**不随包分发**，首次运行自动创建。
>
> 局域网访问：服务器监听所有网卡，只需在 Windows 防火墙放行 8080 端口，同一 WiFi 下手机即可访问 `http://<电脑IP>:8080`。

## 主要 API

| 方法 | 路径 | 说明 |
|---|---|---|
| POST | `/api/chat` | 流式对话（SSE），支持 send/regenerate/edit 动作；SSE 除 `delta`/`usage` 外还推送 `tool_event`（记忆检索 / 联网搜索的开始与结束）|
| GET/POST | `/api/sessions` | 会话列表 / 创建会话 |
| GET/DELETE | `/api/sessions/{id}` | 会话详情 / 删除会话（同步清理其向量）|
| POST | `/api/stop` | 中止当前生成 |
| GET | `/api/search?q=关键词` | 全文搜索所有会话标题与消息内容，返回匹配片段与消息定位 |
| GET | `/api/debug/search?q=&top_k=` | 记忆检索调试接口，返回混合检索命中、向量通道是否生效与索引规模 |
| GET/POST | `/api/config` | 读取 / 更新模型配置（含 `embedding_model`、`memory_enabled`）|
| POST | `/api/test-models` | 批量测试模型可用性 |

## 致谢

- [liboai](https://github.com/D7EAD/liboai) — OpenAI API 的 C++ 客户端库（MIT）
- [nlohmann/json](https://github.com/nlohmann/json)、[libcurl](https://curl.se/libcurl/)、[highlight.js](https://highlightjs.org/)

## License

MIT（继承自 liboai）

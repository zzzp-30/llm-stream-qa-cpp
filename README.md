# LLM Stream QA — C++ 大语言模型流式问答系统

基于 C++17 与 [liboai](https://github.com/D7EAD/liboai) 的轻量级流式问答系统：C++ 手写 HTTP 服务器 + SSE 流式推送，支持多厂商模型切换、图片多模态识别与多会话管理，最终打包为免安装的 Windows 桌面应用。

## 功能特性

- **流式输出**：SSE 协议逐字推送，服务端持久缓冲状态机保证跨包分片解析鲁棒
- **多模态识别**：图片输入自动路由至视觉模型（Vision API），前端自动压缩编码
- **多会话管理**：会话隔离、消息编辑、对话重生成、生成中止，JSON 持久化（临时文件 + 原子替换），支持跨重启恢复
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

模型接入层复用开源库 **liboai**（MIT 协议），本项目聚焦其上应用层的设计与实现。

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

## 主要 API

| 方法 | 路径 | 说明 |
|---|---|---|
| POST | `/api/chat` | 流式对话（SSE），支持 send/regenerate/edit 动作 |
| GET/POST | `/api/sessions` | 会话列表 / 创建会话 |
| GET/DELETE | `/api/sessions/{id}` | 会话详情 / 删除会话 |
| POST | `/api/stop` | 中止当前生成 |
| GET/POST | `/api/config` | 读取 / 更新模型配置 |
| POST | `/api/test-models` | 批量测试模型可用性 |

## 致谢

- [liboai](https://github.com/D7EAD/liboai) — OpenAI API 的 C++ 客户端库（MIT）
- [nlohmann/json](https://github.com/nlohmann/json)、[libcurl](https://curl.se/libcurl/)、[highlight.js](https://highlightjs.org/)

## License

MIT（继承自 liboai）

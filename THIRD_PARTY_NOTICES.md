# 来源与许可证

| 组件 | 来源 | 处理 |
|---|---|---|
| QQ bridge、共享 DSH 客户端 | https://github.com/Derpyu520/qq-bridge ，基线 9df6a7e7fc5abcb36793337f778483bd442a3d2d，加本地定制 | MIT，保留原版权声明 |
| 表情解码器 | 本地 wechat-emoticon-parser，版权 CN-Grace 2026 | MIT 原文随 vendor 分发；无缓存、数据库 |
| DSH / SnowLuma / MCP 依赖 | npm package-lock.json | 安装时获取，不打包 node_modules；各自许可适用 |
| 微信 hook 上游 | https://github.com/aixed/wechat_control | 2026-10-18 GitHub 元数据未声明 license；不再分发完整源码、DLL |
| 原生表情扩展（`hook-ext/`） | 本项目作者编写，运行在上一行的上游 hook 之上 | MIT，随仓库分发；**只包含自研部分**，不含上游基座 |
| QQ / 微信 / DSH / SnowLuma 客户端 | 各自供应方 | 外部依赖，不再分发安装包 |
| 展示插画 | 项目所有者提供的“姐姐请吃饭” | 用于本项目展示，不包含在代码 MIT 许可中 |

关于 hook：`hook-ext/` 里的代码需要一份可用的上游 hook 才能编译成 `version.dll`。
上游未声明 license，因此本仓库**只提供自研的那部分扩展**（type47 构造、md5 定位、
PrepareShape 校验、协程执行器），不提供基座源码或已编译 DLL。接入方式见
[`hook-ext/README.md`](hook-ext/README.md)，运行条件见 [`docs/HOOK-DEPENDENCY.md`](docs/HOOK-DEPENDENCY.md)。

本地 TTS 目录包含另一份历史项目，未重复打包其中的桥接源码、配置或音频。QQ 当前语音/TTS 调用接口保留，音色模型、素材与服务由部署者自行配置。

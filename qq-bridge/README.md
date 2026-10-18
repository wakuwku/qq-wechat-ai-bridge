# 大肥鱼 · QQ 连接

基于 [Derpyu520/qq-bridge](https://github.com/Derpyu520/qq-bridge) 的定制整理版，连接 SnowLuma / OneBot v11 与 DSH，保留原作者 MIT 署名。

## 安装

要求 Windows、Node.js >=22.13、已安装并登录的 QQ / SnowLuma、已启动的 DSH。外部软件从其官方来源安装，本包不含客户端或账号。

```powershell
npm ci
Copy-Item config.example.json config.json
node scripts/setup-dsh.mjs
```

`setup-dsh.mjs` 安装 DSH 插件并修改选择的 profile，运行前阅读提示并保留备份，完成后重启 DSH。在 config.json 填写自己的 DSH 地址、模型、SnowLuma 端点与令牌，以及 ownerQQ；只把授权会话加入 allow.private / allow.groups，保持 allowAllWhenEmpty=false。

```powershell
npm start
```

控制台默认本机 3100 端口，按日志访问。不要将控制台或 OneBot 端口映射到公网。roles/小鲸鱼.md 是公开示例，可以自行更换本地角色。

## 能力与范围

- 群聊与私聊会话、角色预设、白名单、限流、图片与表情。
- 受限聊天工具面、用量记录、本地控制台。
- 可选语音/TTS 接口。**AI 发语音默认关闭**，需另配音频库、FFmpeg、TTS 服务。
- voice-gui.mjs / voice-cli.mjs 是上游外置 voice-tool/ 工具，本包不含；基础文字桥接不依赖它们。

旧指南保留为背景，安装以本页为准。

## 测试

```powershell
npm run test:audit
```

工具名契约测试需要 DSH 安装目录；可设置 DSH_INSTALL_MODULES 指向其 node_modules。不具备该环境不能宣称完整通过。FFmpeg 缺失时对应语音用例跳过。真实发送不属于默认发布测试。

state/、config.json、音频和私人角色不应提交。详细边界见根目录 SECURITY.md 和 docs/AUDIT.md。

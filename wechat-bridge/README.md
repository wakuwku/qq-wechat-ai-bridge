# 大肥鱼 · 微信连接

微信消息监听 → DSH 会话 → 文本、图片、收藏表情发送。微信插件随包位于 plugins/wechat-agent-presets/；DSH 客户端在 lib/，不要求安装 QQ 模块。

## 前提

- Windows、Node.js >=22.13、Python >=3.9。
- 自行安装 DSH 与兼容微信环境。原生接口仅针对已验证的 **4.1.10.27**。
- 外部 hook 提供 SendTextMsg、SendImgMsg、NativeExecutor 和 NativeEmoji/Send、SendStatus。普通上游 hook 不一定具有原生表情扩展。
- 本包不包含无明确许可证的 hook 源码或 DLL，**不是一键完整部署包**。
- hook 必须仅监听回环地址；30001 监听所有网卡时启动器拒绝启动。见 [hook 接入条件](../docs/HOOK-DEPENDENCY.md)。

## 安装

```powershell
npm ci
python -m pip install -r requirements.txt
Copy-Item config.example.json config.json
$env:DFY_PYTHON = (Get-Command python).Source
$env:DFY_WECHAT_ACCOUNT_DIR = '<你的微信账户数据目录>'
```

按所用 DSH 版本的插件安装流程，安装并启用 plugins/wechat-agent-presets 这个本地 bundle，保持相对目录结构，加载其 bundle patch 后重启 DSH。必须在 agentPresets.list 中看到 wechat-chat，不能用默认 coding preset 替代。本包不直接覆盖你的 DSH profile。

配置 dsh.baseUrl、ownerWxid 和 whitelist.chats。先加入如下测试项：

```json
{"chatId":"filehelper","name":"文件传输助手"}
```

可选 expiresAt 为毫秒时间戳，非法或过期拒绝发送。群/联系人 ID 从自己的客户端确认。留空白名单拒绝启动。

手动登录兼容微信并加载适当 hook，然后运行：

```powershell
python scripts/send_native_emoji.py probe
python scripts/send_native_emoji.py executor-status
npm start
```

探针不发送。启动器只起桥接子进程，不重启微信/DSH、不改注册表；Ctrl+C 停止。异常退出留下 state/release-launcher.lock 时，先确认无重复实例，再人工移走锁文件。

## 表情与隐私

stickers/ 初始为空，自己添加有权使用的素材与元数据。type47 需要本账号收藏 MD5 映射，仅有图片文件不代表可发原生表情。客户端使用显式 request_id；循环按消息意图生成稳定 ID，不明结果不重发。文字/普通图片尚不具备相同的持久幂等保证。

陌生表情可从本地缓存解码抽帧，通过 DSH 图片输入交给视觉模型；失败时保持未知，不凭哈希猜含义。允许会话的上下文会发送给所选模型服务商。长期记忆默认关闭，开启前告知参与者。

state/ 中的队列、图片和会话都是私人数据，不提交。解码器原 MIT 许可证在 vendor/ 中。微信升级、数据目录或第三方依赖变化均需重验。

## 离线测试

```powershell
python -m unittest discover -s scripts -p 'test_*.py'
node scripts/test_reply_clean.mjs
node scripts/test_sticker_gate.mjs
node scripts/test-mode-memory.mjs
node scripts/test-release-security.mjs
```

源版本曾完成实机验证；本轮不对发布副本进行真实发送，验证边界见审计报告。

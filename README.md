# 大肥鱼 · Dafeiyu

让一个有性格的聊天伙伴，住进 QQ 和微信。

![姐姐请吃饭](docs/assets/dafeiyu-dinner.png)

大肥鱼把 QQ / 微信消息接到本地 DeepSeek Harness（DSH），按会话维护上下文，通过白名单控制回复范围。两条连接分开安装、分开配置，也能在支持的环境里处理图片、表情和语音。

**这是 Windows 上的社区实验项目，不是腾讯或 DeepSeek 官方产品。** 本次整理版提供 DSH 连接；Codex 并列后端仍属后续工作，不宣称已完成生产接入。

[项目展示](https://wakuwku.github.io/dafeiyu/) · [QQ 安装](qq-bridge/README.md) · [微信安装](wechat-bridge/README.md) · [表情库建库](wechat-bridge/tools/sticker/README.md) · [运维与排障](docs/OPERATIONS.md) · [安全与限制](SECURITY.md) · [审计报告](docs/AUDIT.md) · [来源与许可证](THIRD_PARTY_NOTICES.md)

| | QQ | 微信 |
|---|---|---|
| 消息入口 | SnowLuma / OneBot v11 | 本地监听与外部 hook |
| 模型宿主 | DSH | DSH |
| 会话 | 群聊 / 私聊隔离、角色与模式 | 白名单聊天、独立会话映射 |
| 图片与表情 | 多模态消息、表情管理 | 图片解码、陌生表情抽帧、type47 客户端 |
| 语音 | 可选语音发送与 TTS 接口，默认关闭 | 本次未集成语音 |
| 运维 | 本地控制台、用量记录、限流 | 前台启动器、日志、停止控制 |

原生表情源版本曾在作者指定环境完成文件传输助手和群聊验证，不表示所有微信版本可用。微信依赖 **4.1.10.27** 的特定原生实现，升级必须重新验证。发布包不含微信、QQ、SnowLuma、DSH 安装包或 hook DLL。

## 目录

```text
dafeiyu/
├── qq-bridge/       QQ 桥接、控制台、预设、离线测试
├── wechat-bridge/   微信闭环、收发客户端、解码器、专用预设
│   ├── tools/sticker/   表情库建库：导出收藏 → 重建索引 → 校验 md5
│   └── stickers/        表情库（只有结构示例；素材需要自己准备）
├── docs/            展示页、审计、外部 hook 接入说明、运维与排障笔记
├── tools/           发布扫描与打包工具
├── SECURITY.md
└── THIRD_PARTY_NOTICES.md
```

`docs/OPERATIONS.md` 汇总了运行期真正会咬人的坑（DSH 动态端口、预设模板变量失效导致 turn 直接失败、进程被 job object 带走、掉线取证、孤儿进程、静默降级清单），排查故障前建议先看它。

## 下载与开始

[Releases](https://github.com/wakuwku/dafeiyu/releases) 提供 QQ 和微信两个源码 ZIP，依赖按模块 README 安装。也可以克隆后进入对应模块。配置从 `config.example.json` 复制，**默认白名单为空，不回复任何人**。先只允许自己的测试会话，再逐项开放。

私人角色卡、聊天历史、账号、密钥、表情收藏和运行状态不随包发布。展示图是用户提供的项目形象插画，不是聊天效果截图。后续可在 `docs/assets/` 增加脱敏后的实机展示。

## 验证与维护

发布前执行离线回归、依赖漏洞扫描、隐私扫描与 ZIP 核验；不向真实 QQ / 微信发送测试消息。审查范围和遗留问题见审计报告。QQ 主文件过大、微信内部接口不稳定等维护债务仍存在，通过测试不等于不存在缺陷。

代码保留上游 MIT 许可证和署名。展示图片不包含在代码 MIT 授权内。第三方 hook 未发现明确许可证，完整源码和二进制没有再分发。

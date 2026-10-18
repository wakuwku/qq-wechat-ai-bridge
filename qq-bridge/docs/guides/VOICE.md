# QQ 可选语音

AI 发语音默认是关的。启用前先配置 socialV2.voice、允许的本地音频目录、FFmpeg 和对应服务，使用测试会话验证，不能让群友指定任意本机文件路径。

本包的 src/voice-core.js 负责验证与发送相关处理，scripts/send-voice.mjs 提供命令行入口。TTS 服务、音色模型和音频素材均需自行提供，不包含作者语音库。

上游另有图形界面 voice-gui.mjs 与命令行 voice-cli.mjs，放在与 qq-bridge 平级的 voice-tool/，以及发语音-图形界面.cmd 启动器。这些外置工具不在当前发布包；没有它们仍可运行文字桥接。不要把“接口存在”当成音色服务已经配置完成。

FFmpeg 缺失时相关回归会明确跳过。启用后的实机语音效果属于部署者验收范围。

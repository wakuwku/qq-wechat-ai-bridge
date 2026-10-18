# 运行与排障笔记

这些不是 API 文档，是**踩过的坑**。每一条都对应一次真实的静默故障 —— 表面上"还在跑"，实际功能已经降级或停摆。

## 1. DSH 的 web 端口是动态的

实测同一个 DSH 桌面版三次启动拿到三个不同端口：`19387` → `53508` → `51734` → `64322`。

**任何写死端口的配置都会在某次重启后失效**，而且症状是「桥接连不上 / 401」，很容易误判成桥接坏了。

识别 DSH 的可靠指纹（不需要 token）：

```
GET http://127.0.0.1:<port>/  →  401
正文: "dsh web authentication required; reopen the URL printed by dsh web."
```

`scripts/dsh-endpoint.mjs` 就是按这个指纹在本机监听端口里扫，解析结果写回 `config.json` 的
`dsh.baseUrl`，并缓存到 `state/dsh-baseurl.json`。启动前调用一次即可。

## 2. 预设里的模板变量用错，会让每个 turn 立刻失败

DSH `0.2.1` 起，prompt 模板变量只注册了 `provider` 和 `model`。预设里如果还写着
`Your working directory is {{cwd}}.`，表现是：

```
turn/end reason: {"kind":"error","error":{
  "message":"unknown prompt variable \"{{cwd}}\" in section \"deployment:persona-suffix\";
             registered variables: provider, model"}}
```

**注意这不是"少一句提示"，而是整个 turn 直接失败** —— 微信收到消息后一个字都不会回，
而桥接侧只会看到「回复为空」。这种故障极难从表面发现。

排查办法：看 `turn/end` 的 `reason.kind`，如果是 `error` 就把 `reason.error.message` 打出来。
建议在所有「投递 prompt 后等 turn 结束」的地方都显式判断 `reason.kind === 'error'` 并记日志。

改动预设 YAML 后**必须重启 DSH** 才生效（profile 在启动时组装）。改完不重启，症状一模一样。

## 3. 从 DSH 会话里拉起的进程，会被 DSH 重启带走

即使 `spawn(..., { detached: true })` 也一样 —— `detached` 只影响标准输入输出和控制台，
**不改变进程所在的 Windows Job Object**。DSH 重启时关闭 job，整棵进程树一起结束。

实测两次：微信客户端和看护进程都是从 DSH 的会话里启动的，重启 DSH 后双双消失，
而看护启动的三个子服务反而活成孤儿。

要真正脱离，得让进程由 **explorer.exe** 或 **wscript.exe**（从资源管理器/计划任务触发）创建。
经验：

- `explorer.exe "某文件"` 用 ShellExecute 打开，父进程是 explorer → 不在 DSH 的 job 里
- 但 `explorer.exe` 在 `spawnSync` 里**会静默失败**：返回 0，什么都不启动
- `wscript.exe //B script.vbs` 稳定，但如果调用方本身是 DSH 的子进程，结果仍在同一个 job 里

所以可靠的写法是「先 explorer、等不到再 wscript、最后轮询 pid 文件确认真的起来了」。

## 4. 就绪判据不是「进程在不在」

微信停在扫码界面时进程也是在的。这时候起监听会因为拿不到数据库密钥立刻崩、然后被无限重启，
表现成「无限重启循环」。

正确的判据是 **hook 能响应 + 登录 context 非空**：

```
GET /NativeDiag/Chain  →  ctx_object != 0 且 ctx_call_ok == true
```

**还要去抖**：实测有一次该接口瞬时返回 `ctx_object: 0`，而紧接着连查三次都正常。
如果一次读失败就当"掉登录"，监听和接收器会被 SIGTERM 掉再重建，那一小段时间的消息就丢了。
建议要求连续 2 个心跳周期都读不到才认定未登录。

## 5. 掉线取证：怎么区分「崩了」和「被关掉」

`wevtutil` 有两个坑：

1. **开关必须用冒号形式**：`/c:5` 而不是 `/c 5`，否则报 `Too many arguments`
2. `/q:` 只能过滤 System 字段（匹配不到消息正文），所以要把输出**按 `Event[` 切成事件块**再整块匹配进程名。
   逐行匹配会把「Event ID: 1000」和别的程序的「Source: ...」错配在一起 —— 实测第一次就匹配到了
   另一个服务的崩溃事件。

完整的取证组合：

| 证据 | 来源 |
|---|---|
| 崩溃事件 | `wevtutil qe Application /q:*[System[(EventID=1000) or (EventID=1001) or (EventID=1026)]]` `/c:10 /rd:true /f:text` |
| WER 报告 | `%LOCALAPPDATA%\Microsoft\Windows\WER\ReportArchive` 下含进程名的新目录 |
| 内存转储 | `%LOCALAPPDATA%\CrashDumps` 下含进程名的新 `.dmp` |
| 自杀证据 | 应用自己的日志是否正常刷盘、有无退出记录 |

判定只有两种：

- `crash_likely` —— 有崩溃事件 / dump / WER，**确实崩了**
- `no_crash_record` —— 查不到任何崩溃痕迹，**多半是主动退出或被杀**

一个真实案例：微信在 19:22:52 退出，事后查 Application 日志 4 小时零条相关记录、
CrashDumps 最新是两天前的。结论是"不是崩溃"，但**正常退出不留证据，查不出是谁关的** ——
所以这类取证只能证伪，不能定罪。

## 6. 孤儿进程

看护被杀（强杀连 `exit` 钩子都跑不完）时，它启动的子服务不会跟着死，成为孤儿。
新看护起来后"看不见旧孩子"，于是又起一套 → **两份监听、两个闭环**，同一个群会被回两遍。

处理办法：看护启动时读上一次的状态快照，把里面记录的、还活着的子进程 PID 全部清掉再重建。
判断依据是单实例锁已经保证了「此刻没有别的看护在跑」，所以快照里的活进程一定是孤儿。

## 7. 静默降级清单

这些故障**不会报错**，只会让功能悄悄退化。看到现象先怀疑它们：

| 现象 | 可能原因 |
|---|---|
| 表情变成图片发送 | `native-md5.json` 里的 md5 不再唯一命中（用户改了收藏），原生发送失败后回落 |
| 收到消息但一个字都不回 | 预设模板变量失效（见第 2 条），turn 直接 error |
| 回复正常但记忆不增长 | 记忆标记 `[[remember:...]]` 命中率极低，需要定期总结兜底 |
| 同一句话被回两遍 | 两份闭环同时在跑（见第 6 条孤儿） |
| 重启 DSH 后桥接全挂 | 进程被 job object 带走（见第 3 条） |

## 8. 日志时间

日志行首一律用**本地时间**。历史上因为把 UTC 当本地时间，误判过好几次故障发生的先后顺序。
如果要在日志里再打一个绝对时间，用 `toLocaleString('zh-CN', { hour12: false })`，
不要直接输出 `toISOString()`。

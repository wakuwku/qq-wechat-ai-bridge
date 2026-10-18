# hook-ext —— 原生表情扩展（自研部分）

微信 hook 的**上游基座是第三方项目**，本仓库不分发它。这个目录只放**我们在基座上新增的部分**：
原生收藏表情（type47）的构造与发送链路。

## 一、我们用的是什么 hook

| 项 | 值 |
|---|---|
| 上游项目 | <https://github.com/aixed/wechat_control> |
| 微信版本 | **4.1.10.27**（唯一验证过的版本，其他版本函数 RVA 会变） |
| 形态 | `version.dll` 代理（转发 17 个导出），随 `Weixin.exe` 加载，然后在本地起 HTTP 服务 |
| 服务地址 | 默认 `127.0.0.1:30001` |
| 我们改动的注入方式 | 无。注入、DLL 代理、HTTP 框架都沿用上游 |

**上游在 GitHub 上没有声明 license**，所以本仓库既不打包它的源码，也不打包编译好的 DLL。
想要完整能力，你需要自己获得一份可合法使用的上游 hook，并把本目录接进去。

## 二、这个扩展加了什么

上游 hook 能发文本、图片，但**没有原生收藏表情**。这里补的就是这一块：

| 文件 | 行数 | 作用 |
|---|---|---|
| `NativeEmojiPrepare.cpp` | 892 | 主体：定位收藏表情对象、构造 type47 消息、提交到发送队列、回收 |
| `NativeEmojiRequests.h` | 223 | `/NativeEmoji/Send` 的请求校验（收件人形状、request_id、票据） |
| `NativeEmojiRequestRoutes.inl` | 54 | 把 `native_requests::Service` 挂到 HTTP 服务上 |
| `NativeEmojiSend.inl` | 176 | 真正的一次发送：提交、等结果、判定 `sent / not_sent / unknown` |
| `NativeEmojiShape.inl` | 133 | 只做「形状校验」的路径：构造 + 回读 + 释放，**不发送**（给建库校验用） |
| `NativeCoroutineExecutor.inl` | 118 | 在微信主线程的协程上下文里执行阻塞调用，避免直接跨线程 |
| `NativeEmojiProbe.cpp` | 325 | 链条自检（`/NativeDiag/Chain`、`/NativeExecutor/Probe`）：确认取到的对象有效 |

### 关键设计（为什么不是"直接调发包"）

1. **不自己拼二进制包**。走微信自己的消息工厂：拿到 collection 里的表情对象 → 构造 metadata →
   提交到发送队列。这样版本升级时只需要重新定位函数，不用重写协议。
2. **收藏表情靠 md5 定位**，所以发送前必须 `matched == true`（该 md5 在当前账号收藏里唯一命中）。
   这就是 emoji 库需要 `native-md5.json` 的原因。
3. **结果三分**：`sent` / `not_sent` / `unknown`。`unknown` 时**绝不重发** —— 微信的发送没有幂等键，
   重发就是重复消息。
4. **持久 request_id / 票据**：同一次逻辑发送复用同一个 id，进程重启后仍然认得出来。
5. **参数长度按微信分配器算**：`CalcCapacity(len) = ((len + 1 + 0xF) & ~0xF) - 1`
   （20 → 31，32 → 47）。这个公式是从实测的堆行为反推的，写死别的长度会构造失败。

## 三、暴露的 HTTP 端点

| 端点 | 用途 |
|---|---|
| `POST /NativeEmoji/Send` | 正式发送：`{to_wxid, md5, request_id, confirm}` |
| `POST /NativeEmoji/PrepareShape` | **只做准备**：收藏命中 + 对象构造 + 回读 + 释放，不发送 |
| `GET /NativeEmoji/PrepareShapeStatus` | 上面那个的结果 |
| `GET /NativeEmoji/SendStatus` | 发送结果（`sent / not_sent / unknown`） |
| `GET /NativeEmoji/SendOnceStatus`、`POST /NativeEmoji/SendOnce` | 早期一次性诊断接口，保留但**默认关闭** |
| `GET /NativeDiag/Chain` | 链条自检，返回 `ctx_object` 等指针 |
| `POST /NativeExecutor/Probe` | 执行器自检（登录 generation 变化后必须重跑） |
| `GET /NativeEmojiProbe/Diag` 等 | 诊断用，无需发送 |

`/NativeEmoji/SendOnce*` 是历史遗留的一次性通道。为避免误发，`NativeEmojiSend.inl` 里的
`kTestMd5` 在本仓库版本中已置空，因此这条路**永远匹配不上**，等于关闭。要恢复自己填。

## 四、怎么接进上游基座

上游 hook 的源码结构（版本可能略有差异）：

```
wechat_control/
├── src/
│   ├── version_proxy.cpp     DLL 代理入口 / DllMain
│   ├── http_server.cpp       HTTP 服务
│   ├── http_routes.cpp       路由注册 ← 在这里挂我们的
│   ├── wx_send.cpp           发送实现
│   └── ...
└── 3rdparty/                 httplib.h、nlohmann/json.hpp
```

接入步骤：

1. 把这 9 个文件放进上游的 `src/`（或任意 include 得到的目录）。
2. 在 `http_routes.cpp` 的注册函数里调用：

   ```cpp
   #include "NativeEmojiPrepare.h"
   // ...
   RegisterNativeEmojiExtensions(server);   // 见 NativeEmojiPrepare.cpp 末尾
   ```

3. 编译：MSVC（x64），**C++20**，产物仍然是 `version.dll` 代理。
   依赖只有 `httplib.h` 与 `nlohmann/json.hpp`（上游 3rdparty 已带）。
4. 持久化目录：默认 `state\native-emoji-requests`（相对进程当前目录）。
   要换位置就在编译时覆盖：

   ```
   /DNATIVE_EMOJI_STATE_DIR=L"D:\\your\\path"
   ```

## 五、构建与验证的最低要求

```powershell
# 1. 只读自检：不发送，只确认对象能取到、能构造、能释放
curl.exe -s http://127.0.0.1:30001/NativeDiag/Chain
curl.exe -s -X POST http://127.0.0.1:30001/NativeExecutor/Probe -d "{}"

# 2. 形状校验：拿一条真实收藏的 md5 走一遍（不发送）
curl.exe -s -X POST http://127.0.0.1:30001/NativeEmoji/PrepareShape `
  -H "Content-Type: application/json" `
  -d '{\"to_wxid\":\"filehelper\",\"md5\":\"<32位md5>\",\"confirm\":\"prepare_shape_only\"}'
```

`matched / prepared / readback_verified` 三个都是 `true` 才算能发。

**每次微信重新登录，登录 generation 都会变，必须先跑一次 `/NativeExecutor/Probe`**，
否则发送会一律返回 `executor_probe_required`（客户端随后会静默回落到发图片 —— 这正是
`wechat-bridge` 的 `wechat-supervisor` 要来补的事，见 [../docs/OPERATIONS.md](../docs/OPERATIONS.md)）。

## 六、安全边界（重要）

这段代码会给微信进程开一个**本地 HTTP 接口**，能替你发消息。所以：

1. **只绑回环**。上游初始化里如果监听 `0.0.0.0`，必须改成 `127.0.0.1` / `::1`。
   `wechat-bridge` 的启动器会检查 30001 的监听地址，发现外露就拒绝启动。
2. **调用方白名单不保护底层接口**。`wechat-bridge` 的白名单只管"谁能让 AI 回话"，
   绕过它直接 POST `/NativeEmoji/Send` 依然能发 —— 所以这个端口绝不能暴露到局域网。
3. **不支持任意收件人**。`NativeEmojiRequests.h` 只放行三类：`filehelper`、
   纯数字 `@chatroom`、`wxid_*`。改这里等于放开越权发送能力，务必慎重。
4. **不要在没跑过 Probe 的情况下发送**。执行器没就绪时构造对象可能留下未释放的 context，
   实测会导致微信随后崩溃。

## 七、许可

- 本目录下的代码（`Native*` 这些文件）由本项目作者编写，**MIT**，与仓库根 LICENSE 一致。
- 上游 `aixed/wechat_control` **未声明 license**，本仓库不再分发其源码或二进制；
  你需要自行获得。
- 微信客户端本身、`Weixin.exe` 的逆向接口不属于本仓库，使用风险自负。

## 八、已知的坑

| 现象 | 原因 |
|---|---|
| `matched=false` | md5 不在当前账号收藏里，或收藏有重复项导致不唯一 |
| `target_not_unique_native_favorite` | 收藏被增删后同一 md5 命中多条 —— 重新建库并校验 |
| `executor_probe_required` | 没跑 `/NativeExecutor/Probe`，或微信重登录后 generation 变了 |
| 发送后微信崩溃 | context 没配平释放（`ctx_released` 应为 `true`），或跨线程直接调用了主线程接口 |
| `prepared=false` 且无报错 | 字符串容量按 `CalcCapacity` 算错 |

# 表情库工具链

原生表情（type47）**不是有图片文件就能发**。微信原生表情靠 `md5` 定位收藏，桥接发送时要把
`md5` 交给 hook，hook 在收藏列表里唯一定位到那一张，才构造得出消息对象。

所以一个能发原生表情的表情库必须有两份东西：

| 文件 | 作用 |
|---|---|
| `stickers/<md5>.png/gif/jpg/webp` | 图片本体，**文件名就是 md5** |
| `stickers/meta.json` | 桥接内部索引：`sticker id = md5 前 16 位`，带描述/标签 |
| `stickers/native-md5.json` | 发送映射：`sticker id -> { md5, file }` |

`meta.json` 里的 `native: true` + `nativeMd5` 决定这个表情走原生发送；缺失就退化成发图片。

## 三步建库

```powershell
# 0. 先设好账号数据目录（Persist 就在它下面）
$env:DFY_WECHAT_ACCOUNT_DIR = '<微信数据目录>\xwechat_files\wxid_xxx_1234'

# 1. 导出：把当前收藏解密成 <md5>.png 等
python tools/sticker/export_favorites.py --dry-run     # 先看会导出什么
python tools/sticker/export_favorites.py --apply

# 2. 重建索引：按 md5 生成 meta.json + native-md5.json
python tools/sticker/rebuild_library_from_favorites.py --apply

# 3. 校验：拿真实收藏核验每条 md5 是否仍唯一命中（不发送）
python tools/sticker/validate_native_map.py --apply --to filehelper
```

三步都是 `--dry-run` / `--apply` 显式二选一，不带参数会直接报错拒绝执行。

## 关于解密 key

`export_favorites.py` 默认用 `wechatauto` 派生 key：

```
key = IV = md5(f"{seed}{wxid}EMOTICON")[:16]
```

拿不到 `seed`（`db.cfg_dword` 为空）时会返回 `key_derive_failed`。这时用历史流程取一次 key，
再显式传进去：

```powershell
python tools/sticker/export_favorites.py --apply --key-hex <32位hex>
```

**不要把 key 提交到仓库。** 它和账号绑定，能解开你的收藏表情。

## 为什么必须校验（血泪教训）

`native-md5.json` 是某一时刻的收藏快照。用户在微信里增删收藏之后：

- 某条 md5 可能不再唯一命中 → 每次发送都报 `target_not_unique_native_favorite`
- 桥接随后**回落到发图片**，看起来"还能用"，其实已经不是表情了
- 这种降级是静默的，只有看日志才发现

实测过一次：收藏数从 26 掉到 25，一条 md5 就失效了。所以**改过收藏就重跑一遍三个脚本**。

`validate_native_map.py` 走的是 `/NativeEmoji/PrepareShape`：只做「收藏命中 + 对象构造 + 释放」，
**不会真的发消息**。默认探测收件人是 `filehelper`（文件传输助手），不会打扰任何人。

## 索引格式

见 `stickers/meta.example.json` 与 `stickers/native-md5.example.json`。
这两份示例不含任何真实收藏，只是结构说明。

## 已知边界

- 只有 **4.1.10.27** 验证过；微信升级后 `Persist` 布局或加密方式变了就得重做。
- 收藏表情可能是 `wxgf`（HEVC）容器，导出时要解首帧 —— 依赖 `imageio-ffmpeg`。
- 表情库本身是你的私人素材，`.gitignore` 已排除 `stickers/*`，只保留 README 和示例。

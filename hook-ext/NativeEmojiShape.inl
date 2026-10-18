// 不发送的构造验证（GPT 建议的第 2 步）。
// Included after NativeAbi / ConfigureNative / VerifyNative.
//
// 流程：context -> registry -> manager -> 收藏组 -> 按 MD5 命中 metadata
//       -> 用微信自己分配的字符串反查它用的堆（只读）
//       -> 工厂造 type47 -> 写任意长度收件人 -> 逐字节读回
//       -> 对象在 OwnedPool 里被正常释放
//
// 没有 Submit、没有 envelope、没有持久票据：sends_messages 恒为 false。
// 释放那一步才是分配器兼容性的真正证据 —— 如果微信的 operator delete 不接受
// 我们分配的缓冲，微信会在这一步出问题，而不是等到真发消息之后。
namespace native_shape {
constexpr const char kConfirm[] = "prepare_shape_only";
std::mutex mutex;
bool busy = false;
json last = {{"status", "not_requested"}, {"prepared", false}, {"sends_messages", false}};

// 只放行三类收件人：文件传输助手、纯数字 @chatroom、wxid_。
// 权限（是否在白名单）仍由调用方负责，这里只做形状校验。
bool ValidRecipient(const std::string& value) { return native_requests::ValidRecipient(value); }

json Status() { std::lock_guard<std::mutex> lock(mutex); return last; }

json Run(NativeAbi& abi, const std::string& recipient, const char* md5) {
    json result = {{"status", "not_prepared"}, {"prepared", false}, {"sends_messages", false},
        {"delivery_verified", false}, {"to_wxid", recipient},
        {"recipient_length", static_cast<std::uint64_t>(recipient.size())},
        {"md5", md5}, {"expected_capacity", CalcCapacity(recipient.size())}};
    if (!abi.InExecutor()) { result["status"] = "wrong_executor"; return result; }
    bool cleanup = true;
    {
        OwnedPool pool{abi, cleanup, {}};
        try {
            pool.objects.reserve(6);
            auto acquire = [&](Shared& out, auto&& fn) {
                bool ok = false;
                try { ok = fn(); } catch (...) { pool.Adopt(out); throw; }
                pool.Adopt(out); return ok && out.object && out.control;
            };
            auto operation = [&] {
                Shared context{}, registry{}, manager{}, group{}, message{}, match{};
                if (!acquire(context, [&] {return abi.Context(context); })
                    || !acquire(registry, [&] {return abi.Registry(context, registry); })
                    || !acquire(manager, [&] {return abi.Manager(registry, manager); })
                    || !acquire(group, [&] {return abi.FavGroup(manager, group); })) {
                    result["status"] = "native_metadata_chain_failed"; return;
                }
                std::vector<Shared> entries;
                if (!abi.Entries(group, entries)) { result["status"] = "invalid_favorites"; return; }
                result["examined"] = static_cast<std::uint64_t>(entries.size());
                unsigned matches = 0;
                for (auto entry : entries) {
                    Metadata info;
                    if (!abi.Inspect(entry, info)) { result["status"] = "invalid_metadata"; return; }
                    if (info.md5 != md5) continue;
                    ++matches;
                    if (info.type == 3 && info.vtable_rva == kInfoVtable) match = entry;
                }
                if (matches != 1 || !match.object) { result["status"] = "target_not_unique_native_favorite"; return; }
                result["matched"] = true;

                std::string allocator;
                const bool known = abi.LearnAllocator(match, allocator);
                result["allocator_probe"] = allocator;
                result["allocator_known"] = known;
                result["allocator_kind"] = g_allocatorKind.load();
                result["allocator_kind_legend"] = "0=unknown 1=process_default_heap 2=private_heap 3=not_a_heap_block";
                result["heap_count"] = static_cast<std::uint64_t>(g_heapCount.load());
                result["weixin_heap_index"] = g_weixinHeapIndex.load();
                result["sampled_md5_capacity"] = g_sampledStringCapacity.load();

                if (!acquire(message, [&] {return abi.Make47(message); })) { result["status"] = "factory_failed"; return; }
                if (!abi.Configure47To(message, match, recipient.c_str(), recipient.size())) {
                    result["status"] = "configuration_failed"; return;
                }
                result["configured"] = true;
                const bool verified = abi.Verify47To(message, match, recipient.c_str(), recipient.size());
                result["readback_verified"] = verified;
                if (!verified) { result["status"] = "readback_mismatch"; return; }
                result["prepared"] = true;
                result["status"] = "shape_verified_and_destroyed_without_send";
            };
            operation();
        }
        catch (...) { result["prepared"] = false; result["status"] = "shape_exception"; }
    }   // <- 消息对象必须在这里被正常销毁
    result["cleanup_complete"] = cleanup;
    if (!cleanup) { result["prepared"] = false; result["status"] = "cleanup_failed"; }
    result["post_release_alive"] = true;
    return result;
}

json Once(const std::shared_ptr<NativeAbi>& abi, const std::string& recipient, const char* md5) {
    std::string refusal;
    if (!abi->Gate(refusal)) {
        return {{"status", refusal}, {"prepared", false}, {"sends_messages", false}};
    }
    const auto executor = native_executor::Status();
    if (!executor.value("probe_validated", false) || executor.value("in_flight", true)
        || !executor.value("cleanup_ok", false)) {
        return {{"status", "executor_probe_required"}, {"prepared", false}, {"sends_messages", false}};
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (busy) return last;
        busy = true;
        last = {{"status", "queued_do_not_retry"}, {"prepared", false}, {"sends_messages", false}};
    }
    auto promise = std::make_shared<std::promise<json>>();
    auto future = promise->get_future();
    const std::string target = recipient;
    const std::string targetMd5 = md5;
    const bool posted = abi->Dispatch([abi, promise, target, targetMd5] {
        auto value = Run(*abi, target, targetMd5.c_str());
        {
            std::lock_guard<std::mutex> lock(mutex);
            last = value;
            busy = false;
        }
        try { promise->set_value(std::move(value)); } catch (...) {}
    });
    if (!posted) {
        std::lock_guard<std::mutex> lock(mutex);
        last["status"] = "dispatch_refused";
        busy = false;
        return last;
    }
    if (future.wait_for(std::chrono::seconds(20)) == std::future_status::ready) return future.get();
    // 结果不明时锁住重新提交；先查 /NativeEmoji/PrepareShapeStatus。
    return {{"status", "shape_wait_timed_out_do_not_resubmit"},
            {"prepared", false}, {"sends_messages", false}};
}
} // namespace native_shape

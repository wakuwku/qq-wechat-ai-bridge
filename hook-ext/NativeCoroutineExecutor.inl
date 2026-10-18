// Included inside native_emoji_prepare_internal after the native ABI helpers.
namespace native_executor {
struct Observation {
    std::uint64_t request_id=0;
    DWORD caller_tid=0, callback_tid=0;
    std::uintptr_t context=0, scope=0, tls_object=0;
    bool in_flight=false, submitted=false, completed=false, tls_ok=false;
    bool submission_finished=false, cleanup_ok=true;
    std::string status="not_probed";
};
std::mutex mutex;
Observation last;
std::uint64_t sequence=0;
std::uintptr_t verified_context=0, verified_scope=0;

bool ReadTaskTls(std::uintptr_t base, std::uintptr_t* object) noexcept {
    *object=0;
    __try {
        const auto index=*reinterpret_cast<DWORD*>(base+0xA83C2F0);
        if (index>4096) return false;
        const auto slots=reinterpret_cast<std::uintptr_t*>(__readgsqword(0x58));
        if (!slots || !slots[index]) return false;
        const auto weak=*reinterpret_cast<std::uintptr_t*>(slots[index]+0x1F0);
        if (!weak) return false;
        const auto value=reinterpret_cast<const Shared*>(weak);
        if (!value->object || !value->control
            || *reinterpret_cast<volatile LONG*>(value->control+8)<=0) return false;
        *object=value->object;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ReadGeneration(std::uintptr_t base, std::uintptr_t* context, std::uintptr_t* scope) {
    __try {
        const auto app=*reinterpret_cast<std::uintptr_t*>(base+0xA83AB20);
        if (!app) return false;
        *context=*reinterpret_cast<std::uintptr_t*>(app+0x68);
        const auto owner=*reinterpret_cast<std::uintptr_t*>(app+0x310);
        if (!owner || !*context) return false;
        *scope=*reinterpret_cast<std::uintptr_t*>(owner+0x10);
        // CC5780(scope+8) tests the cancellation bit at scope+0xE9.
        return *scope && !(*reinterpret_cast<unsigned char*>(*scope+0xE9)&1);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
json Status() {
    std::lock_guard<std::mutex> lock(mutex);
    return {{"mode","native_co_launch"},{"request_id",last.request_id},
        {"caller_tid",last.caller_tid},{"callback_tid",last.callback_tid},
        {"submitted",last.submitted},{"completed",last.completed},{"in_flight",last.in_flight},
        {"tls_ok",last.tls_ok},{"cleanup_ok",last.cleanup_ok},{"status",last.status},
        {"probe_validated",verified_context!=0 && verified_scope!=0}};
}
bool Post(std::uintptr_t base, std::function<void(bool)> work, bool probe) {
    std::uintptr_t context=0,scope=0;
    if (!ReadGeneration(base,&context,&scope)) return false;
    std::uint64_t id=0;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (last.in_flight) return false; // Do not resubmit an unknown outstanding task.
        if (!last.cleanup_ok) return false;
        if (!probe && (verified_context!=context || verified_scope!=scope)) return false;
        if (probe) { verified_context=0; verified_scope=0; }
        last=Observation{}; last.request_id=id=++sequence; last.caller_tid=GetCurrentThreadId();
        last.context=context; last.scope=scope; last.in_flight=true; last.status="posting";
    }
    Shared owner{},handle{}; SourceInfo source{};
    if (!AppOwner(base,&owner) || !owner.object || !owner.control) {
        std::lock_guard<std::mutex> lock(mutex);
        last.in_flight=false; last.status="owner_unavailable";
        return false;
    }
    std::function<void()> callback=[base,id,context,scope,probe,work=std::move(work)] {
        std::uintptr_t current_context=0,current_scope=0,tls_object=0;
        const bool tls=ReadTaskTls(base,&tls_object);
        const bool valid=tls && ReadGeneration(base,&current_context,&current_scope)
            && current_context==context && current_scope==scope;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (last.request_id!=id) return;
            last.callback_tid=GetCurrentThreadId(); last.tls_object=tls_object; last.tls_ok=tls;
            last.status=valid ? "callback_running" : "native_task_context_unavailable";
            if (probe && valid && last.cleanup_ok
                && (!last.submission_finished || last.submitted)) {
                verified_context=context; verified_scope=scope;
            }
            if (!valid) { verified_context=0; verified_scope=0; }
        }
        bool completed=true;
        try { work(valid); } catch (...) { completed=false; }
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (last.request_id==id) {
                last.completed=true;
                if (last.submission_finished && last.submitted) last.in_flight=false;
                if (valid) last.status=completed ? "callback_completed" : "callback_exception";
            }
        }
    };
    const bool posted=PostNative(base,owner,&handle,&source,&callback);
    // Native coLaunch moves the callable and the active-job list retains the job.
    // Only release a returned job on a normally completed native call.
    bool cleanup=posted ? ReleaseNative(handle) : true;
    cleanup=ReleaseNative(owner) && cleanup;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (last.request_id==id) {
            last.submitted=posted; last.cleanup_ok=cleanup; last.submission_finished=true;
            if (!posted) {
                last.status="submission_exception_outcome_unknown";
                verified_context=0; verified_scope=0;
            }
            else if (!last.completed) last.status="awaiting_callback";
            if (posted && last.completed) last.in_flight=false;
            if (!cleanup) { verified_context=0; verified_scope=0; }
        }
    }
    return posted;
}
} // namespace native_executor

// One explicitly authorized filehelper test. Included after NativeAbi.
namespace native_send {
constexpr char kTestMd5[]="";   // 一次性诊断接口的默认值，正式发送不走这里
constexpr wchar_t kOriginalTicket[]=L"";   // 持久票据路径由调用方给出（见 RequestRoutes）
std::mutex mutex;
bool claimed=false;
json last={{"status","not_requested"},{"submitted",false},{"delivery_verified",false}};

bool Parse(const json& body) {
    return body.is_object() && body.size()==3
        && body.value("to_wxid",std::string{})=="filehelper"
        && body.value("md5",std::string{})==kTestMd5
        && body.value("confirm",std::string{})=="send_one_native_emoji";
}
json Status() { std::lock_guard<std::mutex> lock(mutex); return last; }

// Native std::function storage. These are the original sender's callback
// implementations with empty captures, never pointers to temporary plugin code.
struct Callback { std::uintptr_t words[8]{}; };
struct Parameters { alignas(8) unsigned char bytes[0xE8]{}; };
struct Envelope { std::uintptr_t vtable; Shared* begin; Shared* end; Shared* capacity; std::uint64_t flag; };
static_assert(sizeof(Callback)==0x40 && sizeof(Parameters)==0xE8 && sizeof(Envelope)==0x28);

bool DestroyCallback(void* storage) noexcept {
    __try {
        auto words=static_cast<std::uintptr_t*>(storage);
        const auto impl=words[7];
        if (!impl) return true;
        const auto vt=*reinterpret_cast<std::uintptr_t*>(impl);
        reinterpret_cast<void(__fastcall*)(void*,bool)>(*reinterpret_cast<std::uintptr_t*>(vt+0x20))
            (reinterpret_cast<void*>(impl),impl!=reinterpret_cast<std::uintptr_t>(storage));
        words[7]=0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool MakeParameters(std::uintptr_t base,Parameters* out,Callback* a,Callback* b,Callback* c) {
    Shared empty{};
    __try {
        a->words[0]=base+0x8595F58; a->words[7]=reinterpret_cast<std::uintptr_t>(a);
        b->words[0]=base+0x8595E98; b->words[7]=reinterpret_cast<std::uintptr_t>(b);
        c->words[0]=base+0x8595DD8; c->words[7]=reinterpret_cast<std::uintptr_t>(c);
        const auto scheduler=*reinterpret_cast<std::uintptr_t*>(base+0xA0CE0B0);
        if (!scheduler) return false;
        reinterpret_cast<void(__fastcall*)(Parameters*,Callback*,Callback*,Callback*,Shared*,std::uintptr_t)>(base+0xDF40)
            (out,a,b,c,&empty,scheduler);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool DestroyParameters(Parameters* p) noexcept {
    bool ok=true;
    for (auto offset:{0x90,0x50,0x10}) ok=DestroyCallback(p->bytes+offset) && ok;
    Shared state{}; std::memcpy(&state,p->bytes,sizeof(state));
    // DF40 receives an empty source and initializes its trailing weak pair empty.
    Shared weak{}; std::memcpy(&weak,p->bytes+0xD8,sizeof(weak));
    if (weak.object || weak.control) return false;
    return ReleaseNative(state) && ok;
}
bool Submit(std::uintptr_t base,Envelope* envelope,Parameters* parameters) noexcept {
    __try {
        reinterpret_cast<void(__fastcall*)(Envelope*,Parameters*)>(base+0x1677A30)(envelope,parameters);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ClaimPersistentTicket(const wchar_t* ticket,const char* md5,const char* recipient="filehelper") {
    HANDLE file=CreateFileW(ticket,
        GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (file==INVALID_HANDLE_VALUE) return false;
    const std::string content=std::string(recipient)+";"+md5+";submission may occur;do not retry\r\n";
    DWORD written=0;
    const bool ok=WriteFile(file,content.data(),static_cast<DWORD>(content.size()),&written,nullptr)
        && written==content.size() && FlushFileBuffers(file);
    CloseHandle(file);
    return ok;
}
std::string MessageUuid(Shared message) {
    std::uint64_t size=0,cap=0; std::uintptr_t chars=message.object+0x6A8;
    if (!Read(message.object+0x6B8,size) || !Read(message.object+0x6C0,cap)
        || size==0 || size>64) return {};
    if (cap>=16 && !Read(chars,chars)) return {};
    char out[65]{};
    if (!CopyChecked(out,chars,static_cast<std::size_t>(size))) return {};
    return std::string(out,static_cast<std::size_t>(size));
}
json Run(NativeAbi& abi,const char* target=kTestMd5,const wchar_t* ticket=kOriginalTicket,
         const char* recipient="filehelper") {
    const std::size_t recipientLength=recipient?std::strlen(recipient):0;
    json result={{"status","not_submitted"},{"submitted",false},{"submission_attempted",false},
        {"delivery_verified",false},{"to_wxid",recipient?recipient:""},{"md5",target}};
    if (!abi.InExecutor()) { result["status"]="wrong_executor"; return result; }
    if (!recipient || recipientLength<1 || recipientLength>200) { result["status"]="invalid_recipient_shape"; return result; }
    bool cleanup=true;
    {
        OwnedPool pool{abi,cleanup,{}};
        try {
            pool.objects.reserve(5);
            auto acquire=[&](Shared& out,auto&& fn) {
                bool ok=false;
                try { ok=fn(); } catch (...) { pool.Adopt(out); throw; }
                pool.Adopt(out); return ok && out.object && out.control;
            };
            auto operation=[&] {
                Shared context{},registry{},manager{},group{},message{},match{};
                if (!acquire(context,[&]{return abi.Context(context);})
                    || !acquire(registry,[&]{return abi.Registry(context,registry);})
                    || !acquire(manager,[&]{return abi.Manager(registry,manager);})
                    || !acquire(group,[&]{return abi.FavGroup(manager,group);})) {
                    result["status"]="native_metadata_chain_failed"; return;
                }
                std::vector<Shared> entries;
                if (!abi.Entries(group,entries)) { result["status"]="invalid_favorites"; return; }
                unsigned matches=0;
                for (auto entry:entries) {
                    Metadata info;
                    if (!abi.Inspect(entry,info)) { result["status"]="invalid_metadata"; return; }
                    if (info.md5==target) {
                        ++matches;
                        if (info.type==3 && info.vtable_rva==kInfoVtable) match=entry;
                    }
                }
                if (matches!=1 || !match.object) { result["status"]="target_not_unique_native_favorite"; return; }
                if (!acquire(message,[&]{return abi.Make47(message);})
                    || !abi.Configure47To(message,match,recipient,recipientLength)
                    || !abi.Verify47To(message,match,recipient,recipientLength)) {
                    result["status"]="message_preparation_failed"; return;
                }
                const auto uuid=MessageUuid(message);
                result["message_uuid"]=uuid;
                if (uuid.empty()) { result["status"]="missing_native_uuid"; return; }
                const auto base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"Weixin.dll"));
                Parameters parameters{}; Callback a{},b{},c{};
                if (!MakeParameters(base,&parameters,&a,&b,&c)) {
                    cleanup=false; result["status"]="parameter_construction_failed"; return;
                }
                Envelope envelope{base+0x84EC9C8,&message,&message+1,&message+1,1};
                if (!ClaimPersistentTicket(ticket,target,recipient)) result["status"]="persistent_ticket_unavailable_do_not_retry";
                else {
                    result["submission_attempted"]=true;
                    const bool submitted=Submit(base,&envelope,&parameters);
                    result["submitted"]=submitted;
                    result["status"]=submitted ? "submitted_delivery_unverified" : "submission_exception_outcome_unknown";
                }
                // 1677A30 copies the shared pointers before returning; pool can
                // release our references, leaving the native job's references.
                cleanup=DestroyParameters(&parameters) && cleanup;
                cleanup=DestroyCallback(&a) && cleanup;
                cleanup=DestroyCallback(&b) && cleanup;
                cleanup=DestroyCallback(&c) && cleanup;
            };
            operation();
        } catch (...) { result["status"]="send_test_exception_do_not_retry"; }
    }
    result["cleanup_complete"]=cleanup;
    return result;
}
json Once(const std::shared_ptr<NativeAbi>& abi) {
    std::string refusal;
    if (!abi->Gate(refusal)) return {{"status",refusal},{"submitted",false}};
    const auto executor=native_executor::Status();
    if (!executor.value("probe_validated",false) || executor.value("in_flight",true)
        || !executor.value("cleanup_ok",false)) return {{"status","executor_probe_required"},{"submitted",false}};
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (claimed) return last;
        claimed=true; last["status"]="queued_do_not_retry";
    }
    auto promise=std::make_shared<std::promise<json>>(); auto future=promise->get_future();
    const bool posted=abi->Dispatch([abi,promise] {
        auto value=Run(*abi);
        { std::lock_guard<std::mutex> lock(mutex); last=value; }
        promise->set_value(std::move(value));
    });
    if (!posted) { std::lock_guard<std::mutex> lock(mutex); last["status"]="dispatch_refused"; return last; }
    if (future.wait_for(std::chrono::seconds(15))==std::future_status::ready) return future.get();
    return Status();
}
} // namespace native_send

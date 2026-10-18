// Prepare is read-only. Separate bounded, explicit filehelper send tests are
// registered below; native execution requires version and executor validation.
#include "NativeEmojiPrepare.h"
#include "httplib.h"
#include "json.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef NATIVE_EMOJI_PREPARE_TEST
#include <windows.h>
#include <wincrypt.h>
#include <intrin.h>
#include "NativeEmojiRequests.h"
#endif

namespace native_emoji_prepare_internal {
using json = nlohmann::json;
constexpr std::uintptr_t kInfoVtable = 0x899BB88;
constexpr std::uintptr_t kMessageVtable = 0x84F99C8;
constexpr std::uintptr_t kFactoryRva = 0x1B09F90;

struct Shared {
    std::uintptr_t object = 0;
    std::uintptr_t control = 0;
};
static_assert(sizeof(Shared) == 16, "Native shared_ptr representation must be 16 bytes");
struct Input { std::string recipient; std::string md5; bool readonly = true; };
struct Metadata { std::uintptr_t vtable_rva = 0; std::uint32_t type = 0; std::string md5; };
struct Result {
    std::string status = "refused";
    bool matched = false;
    bool prepared = false;
    bool cleanup_complete = false;
    std::size_t examined = 0;
};

// Deliberately has no Send/SubmitMessage member. All owned native objects remain
// inside the queue callback, and the public result contains no object pointers.
class Abi {
public:
    virtual ~Abi() = default;
    virtual bool Gate(std::string& refusal) = 0;
    virtual bool Dispatch(std::function<void()> work) = 0;
    virtual bool InExecutor() const = 0;
    virtual bool Context(Shared&) = 0;
    virtual bool Registry(Shared context, Shared&) = 0;
    virtual bool Manager(Shared registry, Shared&) = 0;
    virtual bool FavGroup(Shared manager, Shared&) = 0;
    virtual bool Entries(Shared group, std::vector<Shared>& borrowed) = 0;
    virtual bool Inspect(Shared borrowed, Metadata&) = 0;
    virtual bool Make47(Shared&) = 0;
    virtual bool Configure47(Shared message, Shared borrowed_metadata) = 0;
    virtual bool Verify47(Shared message, Shared borrowed_metadata) = 0;
    virtual bool Release(Shared owned) noexcept = 0;
};

bool ValidMd5(const std::string& value) {
    return value.size() == 32 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    });
}
std::string LowerMd5(std::string value) {
    for (auto& c : value) if (c >= 'A' && c <= 'F') c = static_cast<char>(c + ('a' - 'A'));
    return value;
}
bool ParseInput(const json& body, Input& out) {
    if (!body.is_object()) return false;
    for (auto it = body.begin(); it != body.end(); ++it) {
        if (it.key() != "to_wxid" && it.key() != "md5" && it.key() != "readonly") return false;
    }
    if (!body.contains("to_wxid") || !body["to_wxid"].is_string()
        || !body.contains("md5") || !body["md5"].is_string()
        || (body.contains("readonly") && !body["readonly"].is_boolean())) return false;
    out.recipient = body["to_wxid"].get<std::string>();
    out.md5 = LowerMd5(body["md5"].get<std::string>());
    out.readonly = body.value("readonly", true);
    return out.recipient == "filehelper" && ValidMd5(out.md5) && out.readonly;
}
bool Loopback(const std::string& address) { return address == "127.0.0.1" || address == "::1"; }

struct OwnedPool {
    Abi& abi;
    bool& cleanup;
    std::vector<Shared> objects;
    void Adopt(Shared value) { if (value.object || value.control) objects.push_back(value); }
    ~OwnedPool() {
        for (auto it = objects.rbegin(); it != objects.rend(); ++it) {
            if (!abi.Release(*it)) cleanup = false;
        }
    }
};

// Context() 的分段记录（必须在 NativeAbi 之前定义）：
//   1 = InExecutor 拒绝   2 = 原生调用抛异常   3 = 返回空   4 = 成功
std::atomic<int> g_ctxStage{0};
std::atomic<std::uint64_t> g_ctxObj{0};
std::atomic<std::uint64_t> g_ctxCtl{0};
// 下游各环的分段记录（GPT 要求：上游非空不等于下游服务已注册，必须分开观测）
std::atomic<int> g_stageRegistry{0}, g_stageManager{0};
std::atomic<std::uint64_t> g_regObj{0}, g_regCtl{0};
std::atomic<std::uint64_t> g_mgrObj{0}, g_mgrCtl{0};

Result PrepareInExecutor(Abi& abi, const Input& input) {
    Result result;
    if (!abi.InExecutor()) { result.status = "wrong_executor"; return result; }
    bool cleanup = true;
    {
        OwnedPool pool{abi, cleanup, {}};
        try {
            pool.objects.reserve(5);
            auto operation = [&]() {
                Shared context{}, registry{}, manager{}, group{}, message{};
                auto acquire = [&](Shared& value, auto&& call) {
                    bool ok=false;
                    try { ok=call(); }
                    catch (...) { pool.Adopt(value); throw; }
                    pool.Adopt(value);
                    return ok && value.object && value.control;
                };
                if (!acquire(context,[&]{return abi.Context(context);})) { result.status = "context_unavailable"; return; }
                // 分段留痕：GPT 要求 registry / manager / group 各自可观测，
                // 因为上游"非空"不代表下游服务已注册。
                g_stageRegistry.store(0);
                if (!acquire(registry,[&]{return abi.Registry(context,registry);})) { result.status = "registry_unavailable"; return; }
                g_regObj.store(registry.object); g_regCtl.store(registry.control);
                g_stageRegistry.store(1);
                g_stageManager.store(0);
                if (!acquire(manager,[&]{return abi.Manager(registry,manager);})) { result.status = "manager_unavailable"; return; }
                g_mgrObj.store(manager.object); g_mgrCtl.store(manager.control);
                g_stageManager.store(1);
                if (!acquire(group,[&]{return abi.FavGroup(manager,group);})) { result.status = "fav_group_unavailable"; return; }
                std::vector<Shared> entries;
                if (!abi.Entries(group, entries)) { result.status = "invalid_fav_vector"; return; }
                Shared match{};
                std::size_t matching_md5 = 0, eligible = 0;
                for (auto borrowed : entries) {
                    ++result.examined;
                    Metadata metadata;
                    if (!abi.Inspect(borrowed, metadata)) { result.status = "unreadable_metadata"; return; }
                    if (!ValidMd5(metadata.md5) || LowerMd5(metadata.md5) != input.md5) continue;
                    ++matching_md5;
                    if (metadata.vtable_rva == kInfoVtable && metadata.type == 3) { ++eligible; match = borrowed; }
                }
                if (!matching_md5) { result.status = "md5_not_in_favorites"; return; }
                if (matching_md5 != 1) { result.status = "ambiguous_md5"; return; }
                if (eligible != 1) { result.status = "metadata_not_native_fav_type3"; return; }
                result.matched = true;
                if (!acquire(message,[&]{return abi.Make47(message);})) { result.status = "factory_failed"; return; }
                if (!abi.Configure47(message, match)) { result.status = "configuration_failed"; return; }
                if (!abi.Verify47(message, match)) { result.status = "prepared_shape_mismatch"; return; }
                result.prepared = true;
                result.status = "prepared_and_destroyed_without_send";
            };
            operation();
        } catch (...) { result.prepared = false; result.status = "preparation_exception"; }
    } // Native message is destroyed before its borrowed metadata group.
    result.cleanup_complete = cleanup;
    if (!cleanup) { result.prepared = false; result.status = "cleanup_failed"; }
    return result;
}

Result Prepare(const std::shared_ptr<Abi>& abi, Input input,
               std::chrono::milliseconds timeout = std::chrono::milliseconds(10000)) {
    Result result;
    if (!input.readonly || input.recipient != "filehelper" || !ValidMd5(input.md5)) {
        result.status = "invalid_readonly_filehelper_request"; return result;
    }
    input.md5 = LowerMd5(input.md5);
    std::string refusal;
    if (!abi->Gate(refusal)) { result.status = refusal.empty() ? "image_or_abi_gate_refused" : refusal; return result; }
    auto promise = std::make_shared<std::promise<Result>>();
    auto future = promise->get_future();
    // Heap-owned captures remain valid if the HTTP wait times out. A delayed
    // callback can only prepare and destroy objects; it has no send capability.
    bool dispatched=false;
    try {
        dispatched=abi->Dispatch([abi, promise, input] {
            auto value = PrepareInExecutor(*abi, input);
            try { promise->set_value(std::move(value)); } catch (...) {}
        });
    } catch (...) { result.status="native_queue_exception"; return result; }
    if (!dispatched) { result.status = "native_queue_refused"; return result; }
    if (future.wait_for(timeout) != std::future_status::ready) {
        result.status = "readonly_prepare_wait_timed_out"; return result;
    }
    return future.get();
}

json PublicResult(const Result& result) {
    return {{"experimental",true}, {"readonly",true}, {"sends_messages",false},
        {"status",result.status}, {"matched",result.matched}, {"prepared",result.prepared},
        {"cleanup_complete",result.cleanup_complete}, {"examined",result.examined},
        {"metadata_vtable_rva",kInfoVtable}, {"message_vtable_rva",kMessageVtable}, {"factory_rva",kFactoryRva},
        {"scope","Prepare creates and destroys a temporary native object; it is not send or delivery validation"}};
}

#ifndef NATIVE_EMOJI_PREPARE_TEST
constexpr DWORD kTimestamp = 0x6A18046F;
constexpr DWORD kImageSize = 0xAF0E000;
constexpr LONGLONG kFileSize = 183094832;
constexpr char kSha256[] = "f487e4f56930456a25fb0d7b058c77e2dcd364e268d49bdfff4dc0e3be26ba36";
// Experimental no-send build. This enables only the fingerprint-gated probe;
// native_executor::Post separately requires a successful runtime TLS probe
// for the current login generation before allowing Prepare.
constexpr bool kNativeExecutionEnabled = true;

bool CopyChecked(void* out, std::uintptr_t address, std::size_t size) {
    if (!address) return false;
    __try { std::memcpy(out, reinterpret_cast<const void*>(address), size); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
template<class T> bool Read(std::uintptr_t address, T& out) { return CopyChecked(&out,address,sizeof(out)); }
bool CallOut0(std::uintptr_t function, Shared* out) {
    __try { reinterpret_cast<void(__fastcall*)(Shared*)>(function)(out); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallOut1(std::uintptr_t function, std::uintptr_t object, Shared* out) {
    __try { reinterpret_cast<void(__fastcall*)(void*,Shared*)>(function)(reinterpret_cast<void*>(object),out); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ─────────────────────────────────────────────────────────────────────────────
// 收件人字符串：MSVC std::string 布局与容量
//
// object+0xB0 是 MS_VC std::string 的 _Bx，+0xC0=_Mysize，+0xC8=_Myres。
// SSO 阈值 15：len<=15 内联，len>=16 时 +0xB0 是堆指针。
// 实测样本（len=19）：+0xC0=19，+0xC8=31 —— 与下面的公式一致。
// ─────────────────────────────────────────────────────────────────────────────
std::uint64_t CalcCapacity(std::uint64_t length) {
    const std::uint64_t need = length + 1;
    return ((need + 0xF) & ~static_cast<std::uint64_t>(0xF)) - 1;
}

// ─────────────────────────────────────────────────────────────────────────────
// 分配器归属诊断
//
// Weixin.dll 的依赖表只有 KERNEL32.dll（静态链接 CRT，没有 msvcp140/ucrtbase），
// 并且导入了 HeapCreate —— 所以不能假定它的 std::string 用的是进程默认堆。
// 我们塞进对象里的指针必须由微信析构时用的那个分配器所属的堆分配，否则那次
// free 会破坏堆。
//
// 判据：枚举进程全部堆，用 HeapValidate 找出「微信自己分配的字符串」落在哪个堆。
// 这个结果是运行时可测的，不靠推断。探测失败时回落到 GetProcessHeap（即既有
// filehelper 路径的用法），行为与改动前一致。
// ─────────────────────────────────────────────────────────────────────────────
enum AllocatorKind { kAllocatorUnknown = 0, kAllocatorDefaultHeap = 1,
                     kAllocatorPrivateHeap = 2, kAllocatorNotHeapBlock = 3 };
struct HeapChoice {
    std::uintptr_t heap = 0;
    int index = -1;
    unsigned enumerated = 0;
    bool found = false;
};
bool FindHeapOfPointer(void* pointer, HeapChoice& out) {
    out = HeapChoice{};
    __try {
        HANDLE heaps[64]{};
        const DWORD count = ::GetProcessHeaps(64, heaps);
        out.enumerated = count;
        if (!pointer || !count || count > 64) return false;
        for (DWORD i = 0; i < count; ++i) {
            if (::HeapValidate(heaps[i], 0, pointer)) {
                out.index = static_cast<int>(i);
                out.heap = reinterpret_cast<std::uintptr_t>(heaps[i]);
                out.found = true;
                return true;
            }
        }
        return true;   // 枚举成功但没有任何堆认领：不是普通堆块
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
std::atomic<std::uintptr_t> g_weixinHeap{0};
std::atomic<int> g_weixinHeapIndex{-1};
std::atomic<int> g_allocatorKind{kAllocatorUnknown};
std::atomic<unsigned> g_heapCount{0};
std::atomic<std::uint64_t> g_sampledStringChars{0};
std::atomic<std::uint64_t> g_sampledStringCapacity{0};

bool ReleaseNative(Shared value) noexcept {
    if (!value.control) return !value.object;
    __try {
        auto control = reinterpret_cast<void*>(value.control);
        auto strong = reinterpret_cast<volatile LONG*>(value.control+8);
        auto weak = reinterpret_cast<volatile LONG*>(value.control+12);
        if (*strong <= 0) return false;
        if (InterlockedDecrement(strong) == 0) {
            auto vtable = *reinterpret_cast<std::uintptr_t*>(control);
            reinterpret_cast<void(__fastcall*)(void*)>(*reinterpret_cast<std::uintptr_t*>(vtable))(control);
            if (InterlockedDecrement(weak) == 0) {
                vtable = *reinterpret_cast<std::uintptr_t*>(control);
                reinterpret_cast<void(__fastcall*)(void*)>(*reinterpret_cast<std::uintptr_t*>(vtable+8))(control);
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool DiskHashMatches(HMODULE module) {
    std::vector<wchar_t> path(32768);
    const auto length = GetModuleFileNameW(module,path.data(),static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) return false;
    HANDLE file = CreateFileW(path.data(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    HCRYPTPROV provider=0; HCRYPTHASH hash=0;
    bool ok = GetFileSizeEx(file,&size) && size.QuadPart == kFileSize
        && CryptAcquireContextW(&provider,nullptr,nullptr,PROV_RSA_AES,CRYPT_VERIFYCONTEXT)
        && CryptCreateHash(provider,CALG_SHA_256,0,0,&hash);
    unsigned char buffer[65536]; DWORD read=0;
    while (ok) {
        if (!ReadFile(file,buffer,sizeof(buffer),&read,nullptr)) { ok=false; break; }
        if (!read) break;
        if (!CryptHashData(hash,buffer,read,0)) ok=false;
    }
    unsigned char digest[32]{}; DWORD count=sizeof(digest);
    ok = ok && CryptGetHashParam(hash,HP_HASHVAL,digest,&count,0) && count==sizeof(digest);
    if (hash) CryptDestroyHash(hash);
    if (provider) CryptReleaseContext(provider,0);
    CloseHandle(file);
    if (!ok) return false;
    constexpr char hex[]="0123456789abcdef"; char actual[65]{};
    for (unsigned i=0;i<32;++i) { actual[2*i]=hex[digest[i]>>4]; actual[2*i+1]=hex[digest[i]&15]; }
    return std::memcmp(actual,kSha256,64)==0;
}

struct SourceInfo { const char* file; const char* function; std::uint32_t line; std::uint32_t padding; std::uintptr_t caller; };
static_assert(sizeof(SourceInfo)==32,"native sourceInfo ABI mismatch");
bool PostNative(std::uintptr_t base, Shared owner, Shared* handle, SourceInfo* source, std::function<void()>* work) {
    __try {
        reinterpret_cast<void(__fastcall*)(SourceInfo*,const char*,const char*,std::uint32_t)>(base+0x18CE40)
            (source,"NativeEmojiPrepare.cpp","experimental_readonly_prepare",1);
        reinterpret_cast<void(__fastcall*)(void*,Shared*,SourceInfo*,std::function<void()>*,std::int32_t)>(base+0x2BBCB0)
            (reinterpret_cast<void*>(owner.object),handle,source,work,1); // taskNameTag, not async flag.
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool AppOwner(std::uintptr_t base, Shared* out) {
    __try {
        auto app = reinterpret_cast<std::uintptr_t(__fastcall*)()>(base+0x0410C0)();
        if (!app) return false;
        reinterpret_cast<void(__fastcall*)(void*,Shared*)>(base+0x0426B0)(reinterpret_cast<void*>(app),out);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

#include "NativeCoroutineExecutor.inl"

// ─────────────────────────────────────────────────────────────────────────────
// 主线程任务泵（方案 A）
//
// 历史备用实现，当前 Dispatch 不使用此泵。
// 更正：0x2BBCB0 下游会条件性启动任务；历史超时不能证明它只登记不执行。
// 2026-10-17 已确认 manager 需要原生任务 TLS，单纯进入 UI 线程不足以证明可用。
//
// 做法：替换微信主窗口的窗口过程（WndProc）。微信 UI 线程本来就在不停处理消息，
// 我们在每次消息处理时顺带把自己排队的任务跑掉 —— 天然运行在微信线程里，
// 不需要去逆向它自己的调度器。
//
// 安全边界：只做「探测队列 → 执行 → 转发给原窗口过程」，
// 不修改消息内容、不拦截任何消息；队列为空时开销是一次 mutex 尝试。
// ─────────────────────────────────────────────────────────────────────────────
namespace native_pump {
std::mutex g_mutex;
std::vector<std::function<void()>> g_queue;
HWND g_hwnd = nullptr;
WNDPROC g_original = nullptr;
std::atomic<bool> g_ready{false};
// 诊断计数：用来判断泵到底有没有装上、消息过程有没有被调用、任务有没有跑掉
std::atomic<std::uint64_t> g_procCalls{0};   // Hooked 被调用次数
std::atomic<std::uint64_t> g_posts{0};       // Post 入队次数
std::atomic<std::uint64_t> g_drains{0};      // 实际取走任务的次数
std::atomic<std::uint64_t> g_run{0};         // 实际执行的任务数
std::atomic<long> g_lastError{0};            // Ensure 失败时的 GetLastError

void Drain() {
    std::vector<std::function<void()>> batch;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_queue.empty()) return;
        batch.swap(g_queue);
    }
    g_drains.fetch_add(1);
    for (auto& fn : batch) {
        g_run.fetch_add(1);
        try { fn(); } catch (...) { /* 单个任务失败不能影响微信的消息循环 */ }
    }
}

LRESULT CALLBACK Hooked(HWND h, UINT m, WPARAM w, LPARAM l) {
    g_procCalls.fetch_add(1);
    Drain();
    return CallWindowProcW(g_original, h, m, w, l);
}

struct Pick { HWND hwnd; LONG area; };

BOOL CALLBACK EnumCB(HWND h, LPARAM p) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(h)) return TRUE;
    RECT r{};
    if (!GetWindowRect(h, &r)) return TRUE;
    const LONG area = (r.right - r.left) * (r.bottom - r.top);
    auto* pk = reinterpret_cast<Pick*>(p);
    if (area > pk->area) { pk->area = area; pk->hwnd = h; }
    return TRUE;
}

bool Ensure() {
    if (g_ready.load()) return true;
    Pick pk{nullptr, 0};
    EnumWindows(&EnumCB, reinterpret_cast<LPARAM>(&pk));
    if (!pk.hwnd) { g_lastError.store(-1); return false; }
    SetLastError(0);
    auto orig = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(pk.hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&Hooked)));
    if (!orig) { g_lastError.store(GetLastError()); return false; }
    g_original = orig;
    g_hwnd = pk.hwnd;
    g_ready.store(true);
    return true;
}

bool Post(std::function<void()> fn) {
    if (!Ensure()) return false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_queue.push_back(std::move(fn));
    }
    g_posts.fetch_add(1);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);   // 唤醒消息循环，保证空闲时也会跑一轮
    return true;
}
} // namespace native_pump

class NativeAbi final : public Abi {
    std::atomic<std::uintptr_t> base_{0};
    static thread_local bool inside_callback_;
public:
    bool Gate(std::string& refusal) override {
        if (!kNativeExecutionEnabled) { refusal="native_executor_disabled"; return false; }
        if (sizeof(std::function<void()>)!=0x40) { refusal="std_function_abi_mismatch"; return false; }
        auto module=GetModuleHandleW(L"Weixin.dll");
        if (!module) { refusal="module_not_loaded"; return false; }
        base_=reinterpret_cast<std::uintptr_t>(module);
        IMAGE_DOS_HEADER dos{}; IMAGE_NT_HEADERS64 nt{};
        if (!Read(base_,dos) || dos.e_magic!=IMAGE_DOS_SIGNATURE || dos.e_lfanew<=0 || dos.e_lfanew>0x100000
            || !Read(base_+dos.e_lfanew,nt) || nt.Signature!=IMAGE_NT_SIGNATURE
            || nt.FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64 || nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC
            || nt.FileHeader.TimeDateStamp!=kTimestamp || nt.OptionalHeader.SizeOfImage!=kImageSize || !DiskHashMatches(module)) {
            refusal="weixin_fingerprint_mismatch"; return false;
        }
        return true;
    }
    bool Dispatch(std::function<void()> work) override {
        if (!kNativeExecutionEnabled || !base_) return false;
        return native_executor::Post(base_,[work=std::move(work)](bool valid) {
            const bool previous=inside_callback_;
            inside_callback_=valid;
            try { work(); } catch (...) { inside_callback_=previous; throw; }
            inside_callback_=previous;
        },false);
    }
    json ProbeExecutor() {
        std::string refusal;
        if (!Gate(refusal)) return {{"status",refusal},{"sends_messages",false}};
        auto promise=std::make_shared<std::promise<bool>>();
        auto future=promise->get_future();
        const bool submitted=native_executor::Post(base_,[promise](bool valid) { promise->set_value(valid); },true);
        bool ready=false,valid=false;
        if (submitted && future.wait_for(std::chrono::seconds(10))==std::future_status::ready) {
            ready=true; valid=future.get();
        }
        auto result=native_executor::Status();
        result["probe_ok"]=ready && valid && result.value("cleanup_ok",false);
        result["sends_messages"]=false;
        if (!submitted) result["probe_request_status"]="not_submitted";
        else if (!ready) result["probe_request_status"]="timed_out_do_not_resubmit";
        else result["probe_request_status"]=valid ? "validated" : "missing_native_task_context";
        return result;
    }
    // Set only in a native callback whose login generation and TLS were checked.
    bool InExecutor() const override { return inside_callback_; }
    bool Context(Shared& out) override {
        // 分段记录，供诊断接口读取。
        // 更正（GPT 审查）：先前这里写着「差异只可能出在 InExecutor」，该推断不成立 ——
        // 实测已出现 stage=3（调用成功但返回空），而 0x425C0 只是复制 app+0x68/+0x70，
        // 该成员具有【登录生命周期】：InitLoginContext 写入、退出登录清零。
        // 所以 stage=3 的正确解释是「那一刻登录 context 不存在」，与执行线程无关。
        if (!InExecutor()) { g_ctxStage.store(1); return false; }   // 1 = InExecutor 拒绝
        const bool ok = CallOut0(base_+0x0425C0, &out);
        g_ctxObj.store(out.object);
        g_ctxCtl.store(out.control);
        g_ctxStage.store(!ok ? 2 : ((out.object && out.control) ? 4 : 3));  // 2=调用失败 3=返回空 4=成功
        return ok;
    }
    bool Registry(Shared context, Shared& out) override { return InExecutor() && CallOut1(base_+0x032C900,context.object,&out); }
    bool Manager(Shared registry, Shared& out) override { return InExecutor() && CallOut1(base_+0x0E147C0,registry.object,&out); }
    bool FavGroup(Shared manager, Shared& out) override { return InExecutor() && CallOut1(base_+0x2AF8690,manager.object,&out); }
    bool Entries(Shared group, std::vector<Shared>& out) override {
        if (!InExecutor()) return false;
        std::uintptr_t begin=0,end=0,capacity=0;
        if (!Read(group.object+0x90,begin) || !Read(group.object+0x98,end) || !Read(group.object+0xA0,capacity)
            || end<begin || capacity<end || (end-begin)%16 || (capacity-begin)%16 || end-begin>16*10000) return false;
        if (!begin) return !end && !capacity;
        for (auto at=begin;at<end;at+=16) { Shared value{}; if (!Read(at,value)) return false; out.push_back(value); }
        return true;
    }
    bool Inspect(Shared value, Metadata& out) override {
        if (!InExecutor() || !value.object || !value.control) return false;
        std::uintptr_t vtable=0,chars=0; std::uint64_t length=0,capacity=0;
        if (!Read(value.object,vtable) || vtable<base_ || vtable-base_>=kImageSize) return false;
        out.vtable_rva=vtable-base_;
        if (out.vtable_rva!=kInfoVtable) return true; // Do not read an unrelated object's layout.
        if (!Read(value.object+8,out.type) || !Read(value.object+0x20,length) || !Read(value.object+0x28,capacity)) return false;
        if (length!=32 || capacity<32 || capacity>1024*1024 || !Read(value.object+0x10,chars)) return false;
        char md5[33]{};
        if (!CopyChecked(md5,chars,sizeof(md5)) || md5[32]) return false;
        out.md5.assign(md5,32);
        return ValidMd5(out.md5);
    }
    bool Make47(Shared& out) override { return InExecutor() && CallOut0(base_+kFactoryRva,&out); }
    bool Configure47(Shared message, Shared metadata) override;
    bool Verify47(Shared message, Shared metadata) override;
    // 任意收件人（群 chatroom / 联系人 wxid / filehelper），长度 1..200。
    // 刻意不是虚函数：离线测试用的 fake ABI 不必实现它。
    bool Configure47To(Shared message, Shared metadata, const char* recipient, std::size_t length);
    bool Verify47To(Shared message, Shared metadata, const char* recipient, std::size_t length);
    // 用微信自己分配的 metadata md5 字符串反查它用的堆。只读，不构造消息。
    bool LearnAllocator(Shared metadata, std::string& detail) {
        if (!InExecutor()) { detail = "wrong_executor"; return false; }
        std::uintptr_t vtable = 0, chars = 0;
        std::uint64_t length = 0, capacity = 0;
        if (!metadata.object || !Read(metadata.object, vtable) || vtable != base_ + kInfoVtable) {
            detail = "wrong_metadata_object"; return false;
        }
        if (!Read(metadata.object + 0x20, length) || !Read(metadata.object + 0x28, capacity)
            || !Read(metadata.object + 0x10, chars)) { detail = "unreadable_md5_string"; return false; }
        if (!chars || length != 32 || capacity < 16) { detail = "md5_string_not_heap_backed"; return false; }
        HeapChoice pick;
        if (!FindHeapOfPointer(reinterpret_cast<void*>(chars), pick)) { detail = "heap_enumeration_failed"; return false; }
        g_heapCount.store(pick.enumerated);
        g_sampledStringChars.store(chars);
        g_sampledStringCapacity.store(capacity);
        if (!pick.found) {
            g_allocatorKind.store(kAllocatorNotHeapBlock);
            detail = "weixin_string_is_not_a_heap_block"; return false;
        }
        const bool isDefault = pick.heap == reinterpret_cast<std::uintptr_t>(::GetProcessHeap());
        g_allocatorKind.store(isDefault ? kAllocatorDefaultHeap : kAllocatorPrivateHeap);
        g_weixinHeap.store(pick.heap);
        g_weixinHeapIndex.store(pick.index);
        detail = isDefault ? "process_default_heap" : "private_heap";
        return true;
    }
    bool Release(Shared value) noexcept override { return ReleaseNative(value); }
};
thread_local bool NativeAbi::inside_callback_=false;

// Leaf SEH function contains no C++ objects needing unwind.
//
// 收件人写法与 wx_send.cpp 的 SetWeixinString 同源，但修掉了它一个隐患：
// 那边对长串分配 len+1 字节却声明 cap=CalcWxCapacity(len)（len=20 时只有 21
// 字节却声称容量 31）。这里按 cap+1 分配，任何落在容量内的写入都在界内。
//
// 堆的选择来自 LearnAllocator()：微信自己分配的字符串落在哪个堆，就用哪个堆。
// 拿不到探测结果时回落到 GetProcessHeap（与既有 filehelper 路径一致）。
bool ConfigureNative(std::uintptr_t base, Shared message, Shared metadata,
                     const char* recipient, std::size_t length, std::uintptr_t heap) {
    bool retained = false;
    char* allocated = nullptr;
    __try {
        auto bytes = reinterpret_cast<unsigned char*>(message.object);
        if (!message.object || !message.control || !metadata.object || !metadata.control
            || !recipient || length < 1 || length > 200
            || *reinterpret_cast<std::uintptr_t*>(bytes) != base + kMessageVtable
            || *reinterpret_cast<std::uintptr_t*>(metadata.object) != base + kInfoVtable
            || *reinterpret_cast<std::uint32_t*>(metadata.object + 8) != 3
            || *reinterpret_cast<std::uint64_t*>(bytes + 0xC0) != 0
            || *reinterpret_cast<std::uint64_t*>(bytes + 0xC8) != 15
            || *reinterpret_cast<std::uintptr_t*>(bytes + 0x708) || *reinterpret_cast<std::uintptr_t*>(bytes + 0x710)
            || *reinterpret_cast<std::uintptr_t*>(bytes + 0x718) || *reinterpret_cast<std::uintptr_t*>(bytes + 0x720)) return false;
        std::memset(bytes + 0xB0, 0, 16);
        if (length <= 15) {
            std::memcpy(bytes + 0xB0, recipient, length);          // SSO 内联，容量保持 15
        } else {
            const std::uint64_t capacity = CalcCapacity(length);    // len=20 -> 31
            const HANDLE owner = heap ? reinterpret_cast<HANDLE>(heap) : ::GetProcessHeap();
            allocated = static_cast<char*>(::HeapAlloc(owner, HEAP_ZERO_MEMORY,
                static_cast<SIZE_T>(capacity + 1)));
            if (!allocated) return false;
            std::memcpy(allocated, recipient, length);
            *reinterpret_cast<std::uintptr_t*>(bytes + 0xB0) = reinterpret_cast<std::uintptr_t>(allocated);
            *reinterpret_cast<std::uint64_t*>(bytes + 0xC8) = capacity;
        }
        *reinterpret_cast<std::uint64_t*>(bytes + 0xC0) = length;
        *reinterpret_cast<std::uint32_t*>(bytes + 0xD8) = 47;
        auto count = reinterpret_cast<volatile LONG*>(metadata.control + 8);
        if (*count <= 0) return false;
        InterlockedIncrement(count); retained = true;
        std::memcpy(bytes + 0x708, &metadata, sizeof(metadata));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (retained) ReleaseNative(metadata);
        return false;
    }
}
// 逐字节读回校验：vtable / type / 收件人(长度+容量+内容+NUL) / metadata 挂载。
bool VerifyNative(std::uintptr_t base, Shared message, Shared metadata,
                  const char* recipient, std::size_t length) {
    __try {
        if (!message.object || !message.control || !metadata.object || !metadata.control
            || !recipient || length < 1 || length > 200) return false;
        auto bytes = reinterpret_cast<const unsigned char*>(message.object);
        std::uintptr_t vtable = 0; std::uint32_t type = 0;
        std::uint64_t size = 0, capacity = 0, chars = 0;
        Shared attached{}, search{};
        std::memcpy(&vtable, bytes, sizeof(vtable));
        std::memcpy(&type, bytes + 0xD8, sizeof(type));
        std::memcpy(&size, bytes + 0xC0, sizeof(size));
        std::memcpy(&capacity, bytes + 0xC8, sizeof(capacity));
        if (vtable != base + kMessageVtable || type != 47) return false;
        if (size != length) return false;
        if (capacity != (length <= 15 ? 15 : CalcCapacity(length))) return false;
        if (length <= 15) {
            if (std::memcmp(bytes + 0xB0, recipient, length) != 0) return false;
        } else {
            std::memcpy(&chars, bytes + 0xB0, sizeof(chars));
            if (!chars) return false;
            char buffer[202]{};
            if (!CopyChecked(buffer, static_cast<std::uintptr_t>(chars), length + 1)) return false;
            if (std::memcmp(buffer, recipient, length) != 0) return false;
            if (buffer[length] != 0) return false;
        }
        std::memcpy(&attached, bytes + 0x708, sizeof(attached));
        std::memcpy(&search, bytes + 0x718, sizeof(search));
        return attached.object == metadata.object && attached.control == metadata.control
            && !search.object && !search.control;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool NativeAbi::Configure47(Shared message, Shared metadata) {
    return InExecutor() && ConfigureNative(base_,message,metadata,"filehelper",10,g_weixinHeap.load());
}
bool NativeAbi::Verify47(Shared message, Shared metadata) {
    return InExecutor() && VerifyNative(base_,message,metadata,"filehelper",10);
}
bool NativeAbi::Configure47To(Shared message, Shared metadata, const char* recipient, std::size_t length) {
    return InExecutor() && ConfigureNative(base_,message,metadata,recipient,length,g_weixinHeap.load());
}
bool NativeAbi::Verify47To(Shared message, Shared metadata, const char* recipient, std::size_t length) {
    return InExecutor() && VerifyNative(base_,message,metadata,recipient,length);
}
#include "NativeEmojiSend.inl"
#include "NativeEmojiShape.inl"
#endif
} // namespace native_emoji_prepare_internal

#ifndef NATIVE_EMOJI_PREPARE_TEST
// 链条自检用的纯 POD 探针。
// 注意 1：MSVC 不允许在「需要对象展开（有析构函数）」的函数里用 __try，
//         所以这里刻意只用 POD、不出现任何 C++ 对象，json 组装留给调用方。
// 注意 2（2026-10-16 经 GPT 审查后修正）：调用 0x425C0 会给登录 context 的强引用 +1，
//         而 POD 没有析构，必须显式配平 Release。否则每次诊断都泄漏一份 context ——
//         实测后果是微信随后崩溃（22:11 那次）。所以 ctx 的持有必须归还，且过程可观测。
struct ChainProbe {
    bool singOk = false;  std::uintptr_t singleton = 0;
    bool appOk = false;   std::uintptr_t app = 0;
    bool a310Ok = false;  std::uintptr_t o310 = 0, c310 = 0, vt310 = 0;
    bool ctxOk = false;   std::uintptr_t ctxObj = 0, ctxCtl = 0, ctxVt = 0;
    bool ctxReleased = false;   // 归还是否成功（GPT 要求：不能只用注释声称无保留）
};

// 释放一个 shared_ptr 控制块（与 ReleaseNative 同语义，但保持纯 POD、可用 __try）
bool ReleasePairPOD(std::uintptr_t control) noexcept {
    if (!control) return true;   // 无控制块 = 无引用需要归还
    __try {
        auto strong = reinterpret_cast<volatile LONG*>(control + 8);
        auto weak = reinterpret_cast<volatile LONG*>(control + 12);
        if (*strong <= 0) return false;
        if (InterlockedDecrement(strong) == 0) {
            auto vtable = *reinterpret_cast<std::uintptr_t*>(control);
            auto slot0 = *reinterpret_cast<std::uintptr_t*>(vtable);
            reinterpret_cast<void(__fastcall*)(void*)>(slot0)(reinterpret_cast<void*>(control));
            if (InterlockedDecrement(weak) == 0) {
                vtable = *reinterpret_cast<std::uintptr_t*>(control);
                auto slot1 = *reinterpret_cast<std::uintptr_t*>(vtable + 8);
                reinterpret_cast<void(__fastcall*)(void*)>(slot1)(reinterpret_cast<void*>(control));
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void ProbeChainPOD(std::uintptr_t base, ChainProbe* p) {
    __try {
        p->singleton = *reinterpret_cast<std::uintptr_t*>(base + 0xA83AB20);
        p->singOk = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { p->singOk = false; }

    __try {
        p->app = reinterpret_cast<std::uintptr_t(__fastcall*)()>(base + 0x0410C0)();
        p->appOk = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { p->appOk = false; }

    if (p->appOk && p->app) {
        __try {
            p->o310 = *reinterpret_cast<std::uintptr_t*>(p->app + 0x310);
            p->c310 = *reinterpret_cast<std::uintptr_t*>(p->app + 0x318);
            p->a310Ok = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { p->a310Ok = false; }
        if (p->a310Ok && p->o310) {
            __try { p->vt310 = *reinterpret_cast<std::uintptr_t*>(p->o310); }
            __except (EXCEPTION_EXECUTE_HANDLER) { p->vt310 = 0; }
        }
    }

    struct Pair { std::uintptr_t object; std::uintptr_t control; };
    Pair ctx{0, 0};
    __try {
        reinterpret_cast<void(__fastcall*)(Pair*)>(base + 0x0425C0)(&ctx);
        p->ctxOk = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { p->ctxOk = false; }
    p->ctxObj = ctx.object;
    p->ctxCtl = ctx.control;
    if (ctx.object) {
        __try { p->ctxVt = *reinterpret_cast<std::uintptr_t*>(ctx.object); }
        __except (EXCEPTION_EXECUTE_HANDLER) { p->ctxVt = 0; }
    }
    // ★ 归还：0x425C0 在 control 非空时做过 lock inc，这里必须配平一次。
    //   只在「调用成功且 control 非空」时释放 —— 异常中途的半成品值不盲目释放。
    if (p->ctxOk && ctx.control) {
        p->ctxReleased = ReleasePairPOD(ctx.control);
    } else {
        p->ctxReleased = true;   // 没有取得引用，也就没有需要归还的
    }
}

#include "NativeEmojiRequestRoutes.inl"

void RegisterNativeEmojiPrepare(httplib::Server& server) {
    using namespace native_emoji_prepare_internal;
    auto abi=std::make_shared<NativeAbi>();
    RegisterNativeEmojiRequests(server,abi);

    server.Get("/NativeEmoji/SendOnceStatus",[](const httplib::Request& request,httplib::Response& response) {
        if (!Loopback(request.remote_addr)) { response.status=403; response.set_content("{}","application/json"); return; }
        response.set_content(native_send::Status().dump(),"application/json");
    });
    server.Post("/NativeEmoji/SendOnce",[abi](const httplib::Request& request,httplib::Response& response) {
        if (!Loopback(request.remote_addr)) { response.status=403; response.set_content("{}","application/json"); return; }
        try {
            if (request.body.size()>512 || !native_send::Parse(json::parse(request.body))) throw std::invalid_argument("invalid");
        } catch (...) { response.status=400; response.set_content("{\"status\":\"fixed_filehelper_test_request_required\"}","application/json"); return; }
        const auto result=native_send::Once(abi);
        response.status=result.value("submitted",false) ? 202 : 409;
        response.set_content(result.dump(),"application/json");
    });

    server.Get("/NativeExecutor/Status",[](const httplib::Request& request,httplib::Response& response) {
        if (!Loopback(request.remote_addr)) { response.status=403; response.set_content("{}","application/json"); return; }
        response.set_content(native_executor::Status().dump(),"application/json");
    });
    server.Post("/NativeExecutor/Probe",[abi](const httplib::Request& request,httplib::Response& response) {
        if (!Loopback(request.remote_addr)) { response.status=403; response.set_content("{}","application/json"); return; }
        if (!request.body.empty() && request.body!="{}") { response.status=400; response.set_content("{}","application/json"); return; }
        const auto result=abi->ProbeExecutor();
        response.status=result.value("probe_ok",false) ? 200 : 409;
        response.set_content(result.dump(),"application/json");
    });


    // 泵的自检接口：判断「WndProc 替换是否装上、消息过程有没有被调用、任务有没有被执行」。
    // 方案 A 若超时，看这里就能定位是"没装上"还是"装上了但不被调用"。
    server.Get("/NativePump/Status", [](const httplib::Request& request, httplib::Response& response) {
        if (!Loopback(request.remote_addr)) { response.status=403; response.set_content("{}","application/json"); return; }
        std::size_t queued = 0;
        { std::lock_guard<std::mutex> lock(native_pump::g_mutex); queued = native_pump::g_queue.size(); }
        nlohmann::json j = {
            {"ready", native_pump::g_ready.load()},
            {"hwnd", static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(native_pump::g_hwnd))},
            {"proc_calls", native_pump::g_procCalls.load()},
            {"posts", native_pump::g_posts.load()},
            {"drains", native_pump::g_drains.load()},
            {"tasks_run", native_pump::g_run.load()},
            {"queued", queued},
            {"last_error", native_pump::g_lastError.load()},
            {"note", "ready=false 表示没找到窗口或替换窗口过程失败；proc_calls=0 表示窗口过程从未被调用"}
        };
        response.set_content(j.dump(), "application/json");
    });

    // 链条自检：把 AppOwner → Context 这条链每一步的原始值报出来，
    // 用来定位 context_unavailable 到底断在哪一环（app 为空？还是虚调用返回空？）。
    server.Get("/NativeDiag/Chain", [](const httplib::Request& request, httplib::Response& response) {
        if (!Loopback(request.remote_addr)) { response.status=403; response.set_content("{}","application/json"); return; }
        const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"Weixin.dll"));
        ChainProbe p;
        ProbeChainPOD(base, &p);

        auto rva_of = [base](std::uintptr_t v) -> nlohmann::json {
            if (!v || v <= base || v - base >= 0xAF0E000) return nullptr;
            return static_cast<std::uint64_t>(v - base);
        };

        nlohmann::json j = {
            {"base", static_cast<std::uint64_t>(base)},
            {"singleton_readable", p.singOk},
            {"singleton", static_cast<std::uint64_t>(p.singleton)},
            {"app_call_ok", p.appOk},
            {"app", static_cast<std::uint64_t>(p.app)},
            {"app_310_readable", p.a310Ok},
            {"app_310_object", static_cast<std::uint64_t>(p.o310)},
            {"app_318_control", static_cast<std::uint64_t>(p.c310)},
            {"app_310_vtable_rva", rva_of(p.vt310)},
            {"ctx_call_ok", p.ctxOk},
            {"ctx_object", static_cast<std::uint64_t>(p.ctxObj)},
            {"ctx_control", static_cast<std::uint64_t>(p.ctxCtl)},
            {"ctx_vtable_rva", rva_of(p.ctxVt)},
            {"ctx_released", p.ctxReleased},
            {"singleton_matches_app", p.singOk && p.appOk && (p.singleton == p.app)},
            {"ctx_stage_from_prepare", g_ctxStage.load()},
            {"ctx_obj_from_prepare", g_ctxObj.load()},
            {"ctx_ctl_from_prepare", g_ctxCtl.load()},
            {"ctx_stage_legend", "1=InExecutor refused 2=native call threw 3=returned empty 4=ok"},
            {"registry_stage", g_stageRegistry.load()},
            {"registry_object", g_regObj.load()},
            {"registry_control", g_regCtl.load()},
            {"manager_stage", g_stageManager.load()},
            {"manager_object", g_mgrObj.load()},
            {"manager_control", g_mgrCtl.load()},
            {"downstream_legend", "stage 0=not reached 1=acquired"},
            {"note", "read-only diagnostic; the context reference obtained here IS released (see ctx_released)"}
        };
        response.set_content(j.dump(), "application/json");
    });

    server.Post("/NativeEmoji/Prepare",[abi](const httplib::Request& request,httplib::Response& response) {
        if (!Loopback(request.remote_addr)) { response.status=403; response.set_content("{\"status\":\"loopback_only\"}","application/json"); return; }
        if (request.body.size()>1024) { response.status=400; response.set_content("{\"status\":\"invalid_request\"}","application/json"); return; }
        Input input;
        try { if (!ParseInput(json::parse(request.body),input)) throw std::invalid_argument("invalid"); }
        catch (...) { response.status=400; response.set_content("{\"status\":\"readonly_filehelper_and_32hex_md5_required\"}","application/json"); return; }
        const auto result=Prepare(abi,std::move(input));
        response.status=result.prepared ? 200 : 409;
        response.set_content(PublicResult(result).dump(),"application/json");
    });

    // 不发送的构造验证：支持任意长度收件人（群 chatroom / 联系人 wxid）。
    // 只造对象、写收件人、读回、释放；没有任何提交入口。
    server.Get("/NativeEmoji/PrepareShapeStatus",[](const httplib::Request& request,httplib::Response& response) {
        if (!Loopback(request.remote_addr)) { response.status=403; response.set_content("{}","application/json"); return; }
        response.set_content(native_shape::Status().dump(),"application/json");
    });
    server.Post("/NativeEmoji/PrepareShape",[abi](const httplib::Request& request,httplib::Response& response) {
        if (!Loopback(request.remote_addr)) { response.status=403; response.set_content("{\"status\":\"loopback_only\"}","application/json"); return; }
        std::string recipient, md5;
        bool valid=false;
        try {
            if (request.body.size()<=512) {
                const auto body=json::parse(request.body);
                if (body.is_object() && body.size()==3
                    && body.contains("to_wxid") && body["to_wxid"].is_string()
                    && body.contains("md5") && body["md5"].is_string()
                    && body.contains("confirm") && body["confirm"].is_string()) {
                    recipient=body["to_wxid"].get<std::string>();
                    md5=LowerMd5(body["md5"].get<std::string>());
                    valid=body["confirm"].get<std::string>()==native_shape::kConfirm
                        && ValidMd5(md5) && native_shape::ValidRecipient(recipient);
                }
            }
        } catch (...) {}
        if (!valid) {
            response.status=400;
            response.set_content("{\"status\":\"to_wxid_md5_and_prepare_shape_only_confirmation_required\"}","application/json");
            return;
        }
        const auto result=native_shape::Once(abi,recipient,md5.c_str());
        response.status=result.value("prepared",false) ? 200 : 409;
        response.set_content(result.dump(),"application/json");
    });
}
#endif

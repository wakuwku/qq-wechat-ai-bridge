// Diagnostic only: observe ONE native type-47 send to filehelper.
// No message is created, modified, queued, retried, or sent by this component.
#include "httplib.h"
#include "json.hpp"
#include "NativeEmojiProbe.h"
#include <windows.h>
#include <MinHook.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

namespace {
using json = nlohmann::json;
using SendFunction = std::uint64_t(__fastcall*)(void*, void*);
constexpr std::uintptr_t kSendRva = 0x1677A30;
constexpr std::uintptr_t kEmojiVtable = 0x84F99C8;
constexpr std::uintptr_t kInfoVtable = 0x899BB88;
constexpr DWORD kTimestamp = 0x6A18046F;
constexpr DWORD kImageSize = 0xAF0E000;
std::uintptr_t g_base = 0;
SendFunction g_original = nullptr;
std::mutex g_mutex;
std::atomic<bool> g_armed{false};
std::atomic<ULONGLONG> g_deadline{0};
std::atomic<std::uint64_t> g_generation{0};
bool g_installed = false;
json g_observation = nullptr;
#ifdef NATIVE_EMOJI_PROBE_TEST
void (*test_before_claim)() = nullptr;
#endif

// SEH is kept in a function with no C++ objects requiring unwinding.
bool CopyChecked(void* out, std::uintptr_t address, std::size_t size) {
    if (!address) return false;
    __try { std::memcpy(out, reinterpret_cast<const void*>(address), size); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
template<class T> bool Read(std::uintptr_t address, T& out) {
    return CopyChecked(&out, address, sizeof(out));
}
json ModuleRva(std::uintptr_t address) {
    if (address >= g_base && address - g_base < kImageSize) return address - g_base;
    return nullptr; // Never expose foreign pointers or raw heap addresses.
}
bool IsFilehelper(std::uintptr_t message) {
    std::uint64_t length = 0, capacity = 0;
    if (!Read(message + 0xC0, length) || !Read(message + 0xC8, capacity) || length != 10) return false;
    std::uintptr_t chars = message + 0xB0;
    if (capacity >= 16 && !Read(chars, chars)) return false;
    char recipient[11]{};
    return CopyChecked(recipient, chars, 11) && std::memcmp(recipient, "filehelper", 11) == 0;
}
json SharedPointerShape(std::uintptr_t address) {
    std::uintptr_t object = 0, control = 0, object_vtable = 0, control_vtable = 0;
    if (!Read(address, object) || !Read(address + 8, control)) return { {"readable", false} };
    const bool has_object = object != 0, has_control = control != 0;
    const bool object_readable = has_object && Read(object, object_vtable);
    const bool control_readable = has_control && Read(control, control_vtable);
    // No raw metadata, strings, URLs, AES/auth keys, chat content or user IDs.
    return {
        {"readable", true}, {"has_object", has_object}, {"has_control", has_control},
        {"object_vtable_rva", object_readable ? ModuleRva(object_vtable) : json(nullptr)},
        {"control_vtable_rva", control_readable ? ModuleRva(control_vtable) : json(nullptr)},
        {"matches_known_emoticon_info_vtable", object_readable && object_vtable == g_base + kInfoVtable}
    };
}
// ─────────────────────────────────────────────────────────────────────────────
// 诊断 2.0：无条件记录每次 send-wrapper 调用的【实际】结构
//
// 为什么加：原版只在 4 个条件全中时才记录（vtable/type/收件人/容器形状）。
// 实机测试中 installed=true、用户确实发了 type-47 表情，但 observation 仍是 null
// —— 说明那几个字段偏移（+0xB0/+0xC0/+0xD8）或容器假设至少有一个不成立。
// 这个记录器不预设任何条件，把真实字节原样记下来，供对比修正。
//
// 安全：只读；全程走 CopyChecked（SEH 保护）；不记录聊天正文、不记录密钥；
//       不创建/修改/重发任何消息。
// ─────────────────────────────────────────────────────────────────────────────
json g_diag = json::array();
constexpr std::size_t kDiagMax = 6;   // 环形缓冲，防内存膨胀

std::string HexBytes(std::uintptr_t address, std::size_t n) {
    unsigned char buf[64]{};
    if (n > sizeof(buf)) n = sizeof(buf);
    if (!CopyChecked(buf, address, n)) return std::string("<unreadable>");
    static const char* H = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (std::size_t i = 0; i < n; ++i) { s += H[buf[i] >> 4]; s += H[buf[i] & 0x0F]; }
    return s;
}

std::string AsciiPreview(std::uintptr_t address, std::size_t n) {
    unsigned char buf[64]{};
    if (n > sizeof(buf)) n = sizeof(buf);
    if (!CopyChecked(buf, address, n)) return std::string("<unreadable>");
    std::string s;
    for (std::size_t i = 0; i < n; ++i) {
        const unsigned char c = buf[i];
        s += (c >= 32 && c < 127) ? static_cast<char>(c) : '.';
    }
    return s;
}

void RecordDiag(void* argument) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_diag.size() >= kDiagMax) g_diag.erase(g_diag.begin());

    const auto wrapper = reinterpret_cast<std::uintptr_t>(argument);
    json rec;
    rec["tick"] = static_cast<std::uint64_t>(GetTickCount64());
    rec["arg0"] = wrapper;
    rec["arg0_rva"] = ModuleRva(wrapper);
    rec["head64_hex"] = HexBytes(wrapper, 64);

    // 按原假设解析容器：+8 = begin，+16 = end
    std::uintptr_t begin = 0, end = 0;
    const bool rb = Read(wrapper + 8, begin);
    const bool re = Read(wrapper + 16, end);
    rec["assumed_container"] = {
        {"begin_readable", rb}, {"end_readable", re},
        {"begin", begin}, {"end", end},
        {"delta", (re && end > begin) ? json(end - begin) : json(nullptr)}
    };

    json ents = json::array();
    if (rb && re && begin && end > begin && ((end - begin) % 16) == 0 && (end - begin) <= 16 * 16) {
        std::size_t i = 0;
        for (auto entry = begin; entry < end && i < 8; entry += 16, ++i) {
            std::uintptr_t object = 0, vtable = 0;
            std::uint32_t type = 0;
            const bool ro = Read(entry, object);
            const bool rv = ro && object && Read(object, vtable);
            const bool rt = rv && Read(object + 0xD8, type);

            json e;
            e["entry_addr"] = entry;
            e["object"] = object;
            e["object_rva"] = ModuleRva(object);
            e["vtable"] = vtable;
            e["vtable_rva"] = rv ? ModuleRva(vtable) : json(nullptr);
            e["vtable_is_emoji_84F99C8"] = rv && vtable == g_base + kEmojiVtable;
            e["type_at_d8"] = rt ? json(type) : json(nullptr);
            e["type_is_47"] = rt && type == 47;

            // 收件人相关的三个假设位置（原版只用 +0xB0/+0xC0）
            std::uint64_t len_c0 = 0, cap_c8 = 0, len_b8 = 0, len_a8 = 0;
            Read(object + 0xC0, len_c0);
            Read(object + 0xC8, cap_c8);
            Read(object + 0xB8, len_b8);
            Read(object + 0xA8, len_a8);
            e["len_c0"] = len_c0;
            e["cap_c8"] = cap_c8;
            e["len_b8"] = len_b8;
            e["len_a8"] = len_a8;
            e["ascii_b0_32"] = AsciiPreview(object + 0xB0, 32);
            e["head_hex_a0_64"] = HexBytes(object + 0xA0, 64);
            // 诊断 2.1：对象头部完整 dump。
            // 实测 0x827C0E8 读出来像 ASCII 字符串而不是函数指针表，
            // 说明「object+0 是虚表指针」这个假设不成立 —— 必须看清真实布局。
            e["object_head64_hex"] = HexBytes(object, 64);
            e["object_first_qword"] = [&]() -> json {
                std::uint64_t q = 0;
                return Read(object, q) ? json(q) : json(nullptr);
            }();
            e["object_first_qword_rva"] = [&]() -> json {
                std::uint64_t q = 0;
                if (!Read(object, q)) return json(nullptr);
                return ModuleRva(static_cast<std::uintptr_t>(q));
            }();
            e["object_plus100_hex"] = HexBytes(object + 0x100, 48);
            // 也记录 entry 自身所在的结构（entry 指向的指针 + 后面几字节）
            e["entry_head32_hex"] = HexBytes(entry, 32);
            e["looks_filehelper_by_original_rule"] = IsFilehelper(object);
            ents.push_back(e);
        }
    }
    rec["entries"] = ents;
    rec["note"] = "unconditional diagnostic record of one send-wrapper call; observation only";
    g_diag.push_back(rec);
}

void Observe(void* argument) {
    if (!g_armed.load()) return;
    const auto generation = g_generation.load();
    if (GetTickCount64() >= g_deadline.load()) {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (generation == g_generation.load() && GetTickCount64() >= g_deadline.load()) g_armed.store(false);
        return;
    }
    // ★ 诊断 2.0：无条件记录本次调用的实际结构（无论后面是否匹配）。
    //    放在这里是因为上面的 deadline 分支已离开临界区，不会自锁。
    try { RecordDiag(argument); } catch (...) { /* 诊断绝不影响原始发送 */ }

    const auto wrapper = reinterpret_cast<std::uintptr_t>(argument);
    std::uintptr_t begin = 0, end = 0;
    if (!Read(wrapper + 8, begin) || !Read(wrapper + 16, end) || !begin || end < begin || (end - begin) % 16 || end - begin > 16 * 16) return;
    for (auto entry = begin; entry < end; entry += 16) {
        std::uintptr_t object = 0, vtable = 0;
        std::uint32_t type = 0;
        if (!Read(entry, object) || !Read(object, vtable) || vtable != g_base + kEmojiVtable || !Read(object + 0xD8, type) || type != 47 || !IsFilehelper(object)) continue;
#ifdef NATIVE_EMOJI_PROBE_TEST
        if (test_before_claim) { auto callback=test_before_claim; test_before_claim=nullptr; callback(); }
#endif
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            // Claim and rearm share a short lock. An observer from an old arm
            // must never consume a newer observation window.
            if (!g_armed.load() || generation != g_generation.load()) return;
            if (GetTickCount64() >= g_deadline.load()) { g_armed.store(false); return; }
            g_armed.store(false);
        }
        json capture = {
            {"kind", "observed_native_send_call"}, {"message_type", 47}, {"recipient_scope", "filehelper"},
            {"message", SharedPointerShape(entry)},
            {"metadata_708", SharedPointerShape(object + 0x708)},
            {"metadata_718", SharedPointerShape(object + 0x718)},
            {"note", "Observation of a native send call is not delivery confirmation"}
        };
        void* frames[40]{};
        const auto count = CaptureStackBackTrace(0, 40, frames, nullptr);
        capture["weixin_stack_rvas"] = json::array();
        for (USHORT i = 0; i < count; ++i) {
            auto rva = ModuleRva(reinterpret_cast<std::uintptr_t>(frames[i]));
            if (!rva.is_null()) capture["weixin_stack_rvas"].push_back(rva);
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        if (generation == g_generation.load()) g_observation = std::move(capture);
        return;
    }
}
std::uint64_t __fastcall Detour(void* first, void* second) {
    const DWORD previous_error = GetLastError();
    try { Observe(first); } catch (...) { /* Diagnostics must not affect a newer arm or the original send. */ }
    SetLastError(previous_error);
    // Exactly one unmodified call to the original. No alternative/fallback send.
    return g_original(first, second);
}
bool Install(std::string& error) {
    if (g_installed) return true;
    const auto module = GetModuleHandleW(L"Weixin.dll");
    if (!module) { error = "Weixin.dll is not loaded"; return false; }
    g_base = reinterpret_cast<std::uintptr_t>(module);
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    if (!Read(g_base, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 || dos.e_lfanew > 0x100000 || !Read(g_base + dos.e_lfanew, nt)
        || nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64
        || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC
        || nt.FileHeader.TimeDateStamp != kTimestamp || nt.OptionalHeader.SizeOfImage != kImageSize) {
        error = "Unsupported Weixin image fingerprint; diagnostic hook refused"; return false;
    }
    constexpr unsigned char signature[] = {0x55,0x41,0x56,0x56,0x57,0x53,0x48,0x81,0xEC,0xE0,0x01,0x00,0x00,0x48,0x8D,0xAC};
    unsigned char actual[sizeof(signature)]{};
    if (!CopyChecked(actual, g_base + kSendRva, sizeof(actual)) || std::memcmp(actual, signature, sizeof(signature))) {
        error = "Send-wrapper signature mismatch; diagnostic hook refused"; return false;
    }
    const auto initialized = MH_Initialize();
    if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED) { error = MH_StatusToString(initialized); return false; }
    auto target = reinterpret_cast<LPVOID>(g_base + kSendRva);
    auto created = MH_CreateHook(target, reinterpret_cast<LPVOID>(Detour), reinterpret_cast<LPVOID*>(&g_original));
    if (created != MH_OK) { error = MH_StatusToString(created); return false; }
    const auto enabled = MH_EnableHook(target);
    if (enabled != MH_OK) { MH_RemoveHook(target); error = MH_StatusToString(enabled); g_original = nullptr; return false; }
    g_installed = true;
    return true;
}
json StatusLocked() {
    if (g_armed.load() && GetTickCount64() >= g_deadline.load()) g_armed.store(false);
    return {{"ret",0}, {"installed",g_installed}, {"armed",g_armed.load()},
        {"scope","one manually initiated native type-47 send to filehelper"},
        {"creates_messages",false}, {"changes_message_arguments",false},
        {"captures_chat_content_or_keys",false}, {"observation",g_observation}};
}
bool Local(const httplib::Request& request, httplib::Response& response) {
    if (request.remote_addr == "127.0.0.1" || request.remote_addr == "::1") return true;
    response.status = 403;
    response.set_content("{\"ret\":-1,\"error\":\"loopback only\"}","application/json");
    return false;
}
}

void RegisterNativeEmojiProbe(httplib::Server& server) {
    // 诊断 2.0：读/清无条件记录（只读观察数据，不含聊天正文与密钥）
    server.Get("/NativeEmojiProbe/Diag", [](const httplib::Request& req, httplib::Response& res) {
        if (!Local(req,res)) return;
        std::lock_guard<std::mutex> lock(g_mutex);
        res.set_content(json({{"ret",0},{"count",g_diag.size()},{"records",g_diag}}).dump(),"application/json");
    });
    server.Post("/NativeEmojiProbe/DiagClear", [](const httplib::Request& req, httplib::Response& res) {
        if (!Local(req,res)) return;
        std::lock_guard<std::mutex> lock(g_mutex);
        g_diag = json::array();
        res.set_content(json({{"ret",0},{"count",0}}).dump(),"application/json");
    });
    server.Get("/NativeEmojiProbe/Status", [](const httplib::Request& req, httplib::Response& res) {
        if (!Local(req,res)) return;
        std::lock_guard<std::mutex> lock(g_mutex);
        res.set_content(StatusLocked().dump(),"application/json");
    });
    server.Post("/NativeEmojiProbe/Arm", [](const httplib::Request& req, httplib::Response& res) {
        if (!Local(req,res)) return;
        if (req.get_header_value("X-Emoji-Probe-Confirm") != "filehelper-one-event") {
            res.status=400;
            res.set_content("{\"ret\":-1,\"error\":\"explicit filehelper-one-event header required\"}","application/json");
            return;
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        std::string error;
        if (!Install(error)) { res.status=409; res.set_content(json({{"ret",-1},{"error",error}}).dump(),"application/json"); return; }
        g_armed.store(false);
        ++g_generation;
        g_observation=nullptr;
        g_deadline.store(GetTickCount64()+120000);
        g_armed.store(true);
        res.set_content(StatusLocked().dump(),"application/json");
    });
    server.Post("/NativeEmojiProbe/Disarm", [](const httplib::Request& req, httplib::Response& res) {
        if (!Local(req,res)) return;
        std::lock_guard<std::mutex> lock(g_mutex);
        g_armed.store(false);
        ++g_generation;
        res.set_content(StatusLocked().dump(),"application/json");
    });
}

#pragma once
// Durable at-most-once request coordinator. No native ABI calls in this file.
#include <windows.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include "json.hpp"

namespace native_requests {
using json=nlohmann::json;
namespace fs=std::filesystem;
struct Request { std::string id,recipient,md5; };
struct Reply { int http=500; json body; };
inline bool ValidId(const std::string& id) {
    if(id.size()<8 || id.size()>64) return false;
    for(unsigned char c:id) if(!((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='_')) return false;
    return true;
}
// 收件人形状校验：filehelper / 纯数字 @chatroom / wxid_。
// 是否在白名单里由调用方（微信 bridge）负责；这里只拦明显不是收件人的输入。
inline bool ValidRecipient(const std::string& value) {
    if(value=="filehelper") return true;
    if(value.size()<4 || value.size()>200) return false;
    const std::string suffix="@chatroom";
    if(value.size()>suffix.size() && value.compare(value.size()-suffix.size(),suffix.size(),suffix)==0) {
        const std::size_t digits=value.size()-suffix.size();
        if(digits<1 || digits>20) return false;
        for(std::size_t i=0;i<digits;++i) if(value[i]<'0'||value[i]>'9') return false;
        return true;
    }
    if(value.compare(0,5,"wxid_")==0) {
        if(value.size()<=6) return false;              // 裸 "wxid_" / "wxid_x" 不是有效 id
        for(std::size_t i=5;i<value.size();++i) {
            const char c=value[i];
            if(!((c>='0'&&c<='9')||(c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'||c=='-')) return false;
        }
        return true;
    }
    return false;
}
inline bool Parse(const json& body,Request& out) noexcept {
    try {
        if(!body.is_object() || body.size()!=4) return false;
        out.id=body.at("request_id").get<std::string>();
        out.recipient=body.at("to_wxid").get<std::string>();
        out.md5=body.at("md5").get<std::string>();
        if(!ValidId(out.id) || !ValidRecipient(out.recipient) || out.md5.size()!=32
           || body.at("confirm").get<std::string>()!="send_native_emoji") return false;
        for(char& c:out.md5) {
            if(c>='A'&&c<='F') c=static_cast<char>(c+32);
            if(!((c>='0'&&c<='9')||(c>='a'&&c<='f'))) return false;
        }
        return true;
    } catch(...) { return false; }
}
inline json Identity(const Request& r) {
    return {{"request_id",r.id},{"to_wxid",r.recipient},{"md5",r.md5}};
}
inline std::int64_t Now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
inline bool WriteNew(const fs::path& path,const json& value) {
    const auto bytes=value.dump();
    HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE) return false;
    DWORD written=0;
    bool ok=WriteFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr)
        && written==bytes.size() && FlushFileBuffers(file);
    CloseHandle(file);
    // A partial record is deliberately preserved and blocks reuse of this ID.
    return ok;
}
inline json Read(const fs::path& path) {
    HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE) throw std::runtime_error("ledger_read_failed");
    LARGE_INTEGER length{};
    if(!GetFileSizeEx(file,&length)||length.QuadPart<=0||length.QuadPart>65536) {
        CloseHandle(file); throw std::runtime_error("invalid_ledger_size");
    }
    std::string bytes(static_cast<std::size_t>(length.QuadPart),'\0'); DWORD count=0;
    const bool ok=ReadFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&count,nullptr)&&count==bytes.size();
    CloseHandle(file);
    if(!ok) throw std::runtime_error("ledger_read_failed");
    return json::parse(bytes);
}
inline bool WriteAtomic(const fs::path& path,const json& value) {
    static std::atomic<unsigned long long> sequence{0};
    fs::path temp=path;
    temp+=L".tmp-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(++sequence);
    if(!WriteNew(temp,value)) return false;
    const bool ok=MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
    if(!ok) DeleteFileW(temp.c_str());
    return ok;
}
struct Hooks {
    // Empty refusal means ready. Gate must include module, executor and login checks.
    std::function<std::string()> ready;
    std::function<bool(std::function<void()>)> dispatch;
    std::function<json(const Request&,const fs::path&)> run;
};
class Service:public std::enable_shared_from_this<Service> {
    fs::path root_;
    Hooks hooks_;
    std::mutex mutex_;
    std::string active_;
    bool faulted_=false;
    fs::path Path(const std::string& id,const wchar_t* suffix) const {
        return root_/(L"request-"+std::wstring(id.begin(),id.end())+suffix);
    }
    Reply Error(int code,const std::string& status,const std::string& id="") const {
        return {code,{{"status",status},{"request_id",id},{"accepted",false},
                      {"submitted",false},{"delivery_verified",false}}};
    }
    json Stamp(const Request& r,json result) const {
        result.update(Identity(r)); result["accepted"]=true;
        result["delivery_verified"]=false; result["updated_at_ms"]=Now();
        return result;
    }
    Reply Existing(const std::string& id,const Request* expected=nullptr) {
        try {
            if(!fs::exists(Path(id,L".json"))) return Error(404,"request_not_found",id);
            const auto record=Read(Path(id,L".json"));
            Request r{record.at("request_id").get<std::string>(),record.at("to_wxid").get<std::string>(),
                      record.at("md5").get<std::string>()};
            if(r.id!=id || !ValidRecipient(r.recipient) || r.md5.size()!=32) throw std::runtime_error("bad_record");
            if(expected && Identity(*expected)!=Identity(r)) return Error(409,"request_id_conflict",id);
            json result;
            if(fs::exists(Path(id,L".result.json"))) {
                result=Read(Path(id,L".result.json"));
                if(result.value("request_id",std::string{})!=id || result.value("md5",std::string{})!=r.md5
                   || result.value("to_wxid",std::string{})!=r.recipient) throw std::runtime_error("bad_result");
            } else if(active_==id) {
                result=Stamp(r,{{"status","pending"},{"submitted",nullptr},{"submission_attempted",nullptr},{"terminal",false}});
            } else {
                result=Stamp(r,{{"status","outcome_unknown_do_not_retry"},{"submitted",nullptr},
                    {"submission_attempted",nullptr},{"terminal",true},
                    {"note","Durable accepted request has no final result. It will never be redispatched."}});
            }
            result["replayed"]=true;
            return {result.value("terminal",true)?200:202,result};
        } catch(...) {
            auto reply=Error(503,"request_ledger_unreadable_do_not_retry",id);
            reply.body["accepted"]=nullptr; reply.body["submitted"]=nullptr;
            reply.body["submission_attempted"]=nullptr;
            return reply;
        }
    }
    void Finish(const Request& r,json value) {
        std::lock_guard<std::mutex> lock(mutex_);
        try {
            value=Stamp(r,std::move(value)); value["terminal"]=true;
            value["replayed"]=false;
            if(!value.value("cleanup_complete",false)) faulted_=true;
            if(!WriteAtomic(Path(r.id,L".result.json"),value)) faulted_=true;
        } catch(...) { faulted_=true; }
        if(active_==r.id) active_.clear();
    }
public:
    Service(fs::path root,Hooks hooks):root_(std::move(root)),hooks_(std::move(hooks)) {}
    Reply Get(const std::string& id) {
        if(!ValidId(id)) return Error(400,"invalid_request_id",id);
        std::lock_guard<std::mutex> lock(mutex_);
        return Existing(id);
    }
    Reply Send(const Request& r) {
        // Validate again: callers other than HTTP must not bypass path validation.
        Request checked;
        auto body=Identity(r); body["confirm"]="send_native_emoji";
        if(!Parse(body,checked)) return Error(400,"invalid_request",r.id);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            try {
                fs::create_directories(root_);
                if(fs::exists(Path(r.id,L".json"))) return Existing(r.id,&checked);
                if(faulted_) return Error(503,"sender_requires_inspection",r.id);
                if(!active_.empty()) return Error(429,"sender_busy_request_not_accepted",r.id);
                const auto refusal=hooks_.ready();
                if(!refusal.empty()) return Error(409,refusal,r.id);
                auto record=Identity(checked); record["schema_version"]=1;
                record["created_at_ms"]=Now(); record["process_id"]=GetCurrentProcessId();
                if(!WriteNew(Path(r.id,L".json"),record)) {
                    if(fs::exists(Path(r.id,L".json"))) return Existing(r.id,&checked);
                    return Error(503,"request_ledger_write_failed",r.id);
                }
                active_=r.id;
            } catch(...) { return Error(503,"request_ledger_unavailable",r.id); }
        }
        const auto self=shared_from_this();
        const auto started=std::make_shared<std::atomic<bool>>(false);
        bool posted=false;
        try {
            posted=hooks_.dispatch([self,checked,started] {
                if(started->exchange(true)) return;
                json result;
                try { result=self->hooks_.run(checked,self->Path(checked.id,L".submit.claim")); }
                catch(...) {
                    result={{"status","outcome_unknown_do_not_retry"},{"submitted",nullptr},
                            {"submission_attempted",nullptr},{"cleanup_complete",false}};
                }
                self->Finish(checked,std::move(result));
            });
        } catch(...) {}
        std::lock_guard<std::mutex> lock(mutex_);
        // Native dispatch false/exception can mean an uncertain post, not proof
        // of no send. Keep accepted record and active reservation until callback
        // finishes, or until restart converts it to an unknown durable result.
        auto reply=Existing(checked.id,&checked);
        reply.body["replayed"]=false;
        if(!posted && !reply.body.value("terminal",true)) {
            reply.body["status"]="dispatch_outcome_unknown_do_not_retry";
            reply.body["submitted"]=nullptr;
            reply.body["submission_attempted"]=nullptr;
        }
        return reply;
    }
};
} // namespace native_requests

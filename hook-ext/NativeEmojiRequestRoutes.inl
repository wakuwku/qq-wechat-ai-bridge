// Included outside native_emoji_prepare_internal; reusable filehelper sender.

// 持久 request_id / 票据的存放目录。相对路径按进程当前工作目录解析，
// 需要换位置时在编译命令里 -DNATIVE_EMOJI_STATE_DIR=L"..." 覆盖即可。
#ifndef NATIVE_EMOJI_STATE_DIR
#define NATIVE_EMOJI_STATE_DIR L"state\\native-emoji-requests"
#endif

void RegisterNativeEmojiRequests(httplib::Server& server,
    const std::shared_ptr<native_emoji_prepare_internal::NativeAbi>& abi) {
    using namespace native_emoji_prepare_internal;
    native_requests::Hooks hooks;
    hooks.ready=[abi] {
        std::string refusal;
        if(!abi->Gate(refusal)) return refusal;
        auto state=native_executor::Status();
        if(state.value("in_flight",true)) return std::string("executor_busy");
        if(!state.value("cleanup_ok",false)||!state.value("probe_validated",false))
            return std::string("executor_probe_required");
        std::uintptr_t context=0,scope=0;
        const auto base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"Weixin.dll"));
        if(!native_executor::ReadGeneration(base,&context,&scope)) return std::string("login_context_unavailable");
        {
            std::lock_guard<std::mutex> lock(native_executor::mutex);
            if(context!=native_executor::verified_context||scope!=native_executor::verified_scope)
                return std::string("login_generation_probe_required");
        }
        return std::string{};
    };
    hooks.dispatch=[abi](std::function<void()> work) { return abi->Dispatch(std::move(work)); };
    hooks.run=[abi](const native_requests::Request& request,const std::filesystem::path& ticket) {
        return native_send::Run(*abi,request.md5.c_str(),ticket.c_str(),request.recipient.c_str());
    };
    auto service=std::make_shared<native_requests::Service>(
        NATIVE_EMOJI_STATE_DIR,std::move(hooks));
    server.Post("/NativeEmoji/Send",[service](const httplib::Request& request,httplib::Response& response) {
        if(!Loopback(request.remote_addr)) { response.status=403; response.set_content("{}","application/json"); return; }
        native_requests::Request parsed;
        bool valid=false;
        try { valid=request.body.size()<=1024 && native_requests::Parse(json::parse(request.body),parsed); } catch(...) {}
        if(!valid) {
            response.status=400;
            response.set_content("{\"status\":\"request_id_recipient_md5_and_confirmation_required\"}","application/json");
            return;
        }
        auto reply=service->Send(parsed);
        response.status=reply.http; response.set_content(reply.body.dump(),"application/json");
    });
    server.Get("/NativeEmoji/SendStatus",[service](const httplib::Request& request,httplib::Response& response) {
        if(!Loopback(request.remote_addr)) { response.status=403; response.set_content("{}","application/json"); return; }
        auto reply=service->Get(request.get_param_value("request_id"));
        response.status=reply.http; response.set_content(reply.body.dump(),"application/json");
    });
}

#pragma once

namespace httplib { class Server; }

// Prepare plus explicit bounded filehelper send-test endpoints.
// Native execution requires the matching module and validated executor.
void RegisterNativeEmojiPrepare(httplib::Server& server);

#pragma once

#include <functional>

#include "security/rbac_manager.h"
#include "server/http_protocol.h"

namespace fastllm {

using AuthRequired = std::function<bool(const HttpRequest&, HttpResponse&, UserInfo&)>;

}

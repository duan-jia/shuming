#pragma once

#include "routes/auth_required.h"
#include "server/http_server.h"
#include "services/document_service.h"

namespace fastllm {

void register_document_routes(HttpServer& server, DocumentService& documents, AuthRequired require_auth);

}

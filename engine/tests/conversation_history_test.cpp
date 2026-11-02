#include "agent/conversation_history.h"

#include <cassert>
#include <iostream>

using namespace fastllm;
using Json = nlohmann::json;

int main() {
    ChatMessage call{"assistant", "Checking", "reason"};
    call.tool_calls = Json::array({{{"id", "call-a"}, {"type", "function"},
        {"function", {{"name", "search_knowledge"}, {"arguments", "{\"query\":\"深信服\"}"}}}}});
    ChatMessage result{"tool", "{\"results\":[]}", ""};
    result.tool_call_id = "call-a";
    auto recovered = message_from_record(message_to_record(call), "request-1");
    assert(recovered.tool_calls == call.tool_calls && recovered.reasoning == "reason");
    assert(recovered.request_id == "request-1");
    auto turns = conversation_turns({{"user", "Question", ""}, call, result, {"assistant", "Answer", ""},
        {"user", "Follow-up", ""}, {"assistant", "Follow-up answer", ""}});
    assert(turns.size() == 2 && turns[0].size() == 4);
    assert(turns[0][2].tool_call_id == "call-a");
    // A crash after declaring a batch cannot fabricate a successful execution.
    turns = conversation_turns({{"user", "Question", ""}, call});
    assert(turns[0].size() == 4);
    assert(turns[0][2].role == "tool" && turns[0][2].content.find("outcome is unknown") != std::string::npos);
    assert(turns[0][3].role == "assistant");
    // Failure markers must follow repaired tool results, not precede them.
    turns = conversation_turns({{"user", "Question", ""}, call, {"assistant", "Stopped", ""}});
    assert(turns[0][2].role == "tool" && turns[0][3].content == "Stopped");
    // Do not forward an orphan tool result from a corrupt/incomplete old tail.
    turns = conversation_turns({result, {"user", "Question", ""}, result, {"assistant", "Answer", ""}});
    assert(turns.size() == 1 && turns[0].size() == 2);
    Json payload = {{"content", Json{{"download_url", "https://host/api/knowledge/download?token=secret"},
        {"evidence", "actual evidence"}}.dump()}, {"image", "data:image/png;base64,secret-image"}};
    const auto safe = trace_payload(payload);
    assert(safe.dump().find("secret") == std::string::npos);
    assert(safe.dump().find("actual evidence") != std::string::npos);
    assert(payload.dump().find("secret") != std::string::npos); // original model payload is not mutated
    std::cout << "PASS: complete turns, call/result replay, interrupted batches, orphan results and trace redaction\n";
}

#pragma once

#include "inference/i_engine.h"

#include <set>

namespace fastllm {

inline nlohmann::json message_to_record(const ChatMessage& message) {
    nlohmann::json value = {{"role", message.role}, {"content", message.content}};
    if (!message.reasoning.empty()) value["reasoning_content"] = message.reasoning;
    if (!message.tool_calls.empty()) value["tool_calls"] = message.tool_calls;
    if (!message.tool_call_id.empty()) value["tool_call_id"] = message.tool_call_id;
    return value;
}

inline ChatMessage message_from_record(const nlohmann::json& value, const std::string& request_id = "") {
    ChatMessage message;
    message.role = value.at("role").get<std::string>();
    message.content = value.value("content", "");
    message.reasoning = value.value("reasoning_content", "");
    message.tool_calls = value.value("tool_calls", nlohmann::json::array());
    message.tool_call_id = value.value("tool_call_id", "");
    message.request_id = request_id;
    return message;
}

// Select and trim whole user requests, never individual call/result messages.
// Crash recovery closes missing results with explicit uncertainty, not execution.
inline std::vector<std::vector<ChatMessage>> conversation_turns(const std::vector<ChatMessage>& history) {
    std::vector<std::vector<ChatMessage>> turns;
    for (const auto& message : history) {
        if (message.role == "user" && (turns.empty() || message.request_id.empty()
            || turns.back().front().request_id != message.request_id)) {
            turns.push_back({});
        }
        if (!turns.empty()) turns.back().push_back(message);
    }
    for (auto& turn : turns) {
        std::set<std::string> pending;
        std::vector<ChatMessage> repaired;
        auto close_pending = [&] {
            for (const auto& id : pending) {
                ChatMessage missing{"tool", "{\"error\":\"No durable tool result exists. Execution may not have started or may have been interrupted; outcome is unknown. Do not claim success or retry automatically.\"}", ""};
                missing.tool_call_id = id;
                repaired.push_back(std::move(missing));
            }
            pending.clear();
        };
        for (const auto& message : turn) {
            if (message.role == "assistant") {
                close_pending();
                for (const auto& call : message.tool_calls) pending.insert(call.at("id").get<std::string>());
            } else if (message.role == "tool") {
                if (!pending.count(message.tool_call_id)) continue;
                pending.erase(message.tool_call_id);
            }
            repaired.push_back(message);
        }
        close_pending();
        if (repaired.back().role != "assistant" || !repaired.back().tool_calls.empty()) {
            repaired.push_back({"assistant", "[The previous request ended without a recorded final answer.]", ""});
        }
        turn = std::move(repaired);
    }
    return turns;
}

// Trace storage must not retain temporary bearer links or Base64 image bodies.
// The model still receives the original payload; omissions are explicit in trace.
inline nlohmann::json trace_payload(nlohmann::json value) {
    if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (it.key() == "download_url" || it.key() == "authorization" || it.key() == "api_key")
                it.value() = "[redacted]";
            else it.value() = trace_payload(std::move(it.value()));
        }
    } else if (value.is_array()) {
        for (auto& item : value) item = trace_payload(std::move(item));
    } else if (value.is_string()) {
        auto text = value.get<std::string>();
        if (text.rfind("data:image/", 0) == 0) return "[image data omitted; fetch the document again to inspect pages]";
        // Tool message content and arguments can themselves contain JSON.
        auto nested = nlohmann::json::parse(text, nullptr, false);
        if (nested.is_object() || nested.is_array()) return trace_payload(std::move(nested)).dump();
        const std::string prefix = "/api/knowledge/download?";
        size_t start = 0;
        while ((start = text.find(prefix, start)) != std::string::npos) {
            auto end = text.find_first_of(" \t\n\r\"')>]", start);
            text.replace(start, end == std::string::npos ? text.size() - start : end - start,
                         "/api/knowledge/download?[redacted]");
            start += prefix.size() + 10;
        }
        return text;
    }
    return value;
}

} // namespace fastllm

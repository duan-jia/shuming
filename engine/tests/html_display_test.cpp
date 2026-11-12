#include "agent/html_display.h"
#include "agent/agent_runtime.h"
#include <cassert>
#include <iostream>

using namespace fastllm;
using Json = nlohmann::json;

class EmptyKnowledge : public KnowledgeSearchBackend {
public:
    bool is_ready() const override { return true; }
    QueryRewriteInfo rewrite_query(const std::string&) const override { return {}; }
    std::vector<Evidence> retrieve_chunks(const QueryRewriteInfo&, int, int) override { return {}; }
    std::vector<Evidence> retrieve_parents(const QueryRewriteInfo&, int, int) override { return {}; }
};
class DisplayModel : public IInferenceEngine {
public:
    int calls = 0;
    bool invalid = false;
    bool is_loaded() const override { return true; }
    std::string model_name() const override { return "display-fixture"; }
    bool supports_tools() const override { return true; }
    int estimate_input_tokens(const std::string&, const InferParams&) override { return 1; }
    void stream_complete(const std::string&, const InferParams& params,
        std::function<void(const std::string&)>, std::function<void(const std::string&)>,
        std::function<void(const InferResult&)> done) override {
        bool found = false;
        for (const auto& tool : params.tools) if (tool["function"]["name"] == "display_html") found = true;
        assert(found);
        InferResult result;
        if (++calls <= 5) {
            const Json args = invalid ? Json{{"html", "<p>Incomplete</p>"}}
                : Json{{"title", "Card"}, {"html", "<table><tr><td>42</td></tr></table>"}, {"summary", "Value: 42"}};
            result.tool_calls.push_back({std::to_string(calls), "display_html", args.dump()});
        } else { result.text = "Value: 42"; result.finish_reason = "stop"; }
        done(result);
    }
};
int main() {
    const Json valid = {{"title", "卡片"}, {"html", "<p>Hello</p>"}, {"summary", "Hello"}};
    assert(html_display(valid)["type"] == "html_display");
    for (auto bad : {Json(), Json::array(), Json{{"html", "x"}}, Json{{"title", 1}, {"html", "x"}, {"summary", "x"}}}) {
        bool rejected = false;
        try { html_display(bad); } catch (const std::invalid_argument&) { rejected = true; }
        assert(rejected);
    }
    for (const auto& key : {"title", "html", "summary"}) {
        auto bad = valid; bad[key] = std::string(key == std::string("html") ? 12001 : key == std::string("title") ? 121 : 2001, 'x');
        bool rejected = false;
        try { html_display(bad); } catch (const std::invalid_argument&) { rejected = true; }
        assert(rejected);
    }
    auto extra = valid; extra["callback"] = "x";
    try { html_display(extra); assert(false); } catch (const std::invalid_argument&) {}
    EmptyKnowledge backend;
    KnowledgeSearchService knowledge(backend);
    WebSearchService web(WebSearchOptions{});
    KnowledgeFetchOptions fetch_options; fetch_options.signing_secret = "test-only";
    KnowledgeFetchService docs([](int, int, DocumentDetails&) { return false; }, fetch_options);
    ToolService tools(knowledge, web, docs);
    RateLimiter limiter;
    CallerContext caller; caller.principal_id = 1; caller.tenant_id = 1;
    for (int scenario = 0; scenario < 3; ++scenario) {
        DisplayModel model; model.invalid = scenario == 1;
        int displayed = 0, successful = 0, failed = 0;
        InferParams params;
        params.record_event = [&](const Json& event) {
            if (scenario == 2 && event["type"] == "message" && event["data"].value("role", "") == "tool") return false;
            if (event["type"] == "tool_end") {
                if (event["data"]["is_error"] == true) ++failed; else ++successful;
            }
            return true;
        };
        AgentOptions options; options.max_turns = 8;
        AgentRuntime runtime(model, tools, limiter, caller, options, false, false, [&](const Json& event) {
            if (event["type"] == "html_display") {
                assert(successful > displayed); // Durable tool_end precedes display.
                assert(event["display"]["html"].is_string()); ++displayed;
            }
        }, nullptr);
        InferResult final;
        runtime.stream_complete("Show a table", params, nullptr, nullptr, [&](const InferResult& result) { final = result; });
        assert(displayed == (scenario == 0 ? 4 : 0));
        if (scenario == 0) { assert(failed == 1); assert(final.error.empty()); }
        if (scenario == 1) assert(failed == 5);
        if (scenario == 2) assert(final.stop_reason == "persistence_error");
    }
    std::cout << "HTML validation, tool loop, four-display cap and durable-before-display passed\n";
}

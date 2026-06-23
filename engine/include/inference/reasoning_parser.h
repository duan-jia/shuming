#pragma once

#include <string>
#include <functional>

namespace fastllm {

enum class ParseState { IDLE, THINKING, ANSWERING, DONE };

struct ParsedToken {
    ParseState state;
    std::string content;
};

class ReasoningParser {
public:
    ReasoningParser();

    ParsedToken feed(const std::string& token);
    void reset();

    ParseState state() const { return state_; }
    const std::string& reasoning() const { return reasoning_buf_; }
    const std::string& answer() const { return answer_buf_; }

private:
    ParseState state_ = ParseState::IDLE;
    std::string reasoning_buf_;
    std::string answer_buf_;
    std::string tag_buf_;
    bool in_think_tag_ = false;
};

}

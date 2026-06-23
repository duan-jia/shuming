#include "inference/reasoning_parser.h"

namespace fastllm {

ReasoningParser::ReasoningParser() = default;

void ReasoningParser::reset() {
    state_ = ParseState::IDLE;
    reasoning_buf_.clear();
    answer_buf_.clear();
    tag_buf_.clear();
    in_think_tag_ = false;
}

ParsedToken ReasoningParser::feed(const std::string& token) {
    ParsedToken result;
    result.state = state_;

    if (state_ == ParseState::DONE) {
        return result;
    }

    if (state_ == ParseState::IDLE) {
        tag_buf_ += token;

        if (tag_buf_.size() > 20) {
            if (tag_buf_.find("<think") != std::string::npos) {
                size_t pos = tag_buf_.find('>');
                if (pos != std::string::npos) {
                    state_ = ParseState::THINKING;
                    std::string remainder = tag_buf_.substr(pos + 1);
                    tag_buf_.clear();
                    if (!remainder.empty()) {
                        reasoning_buf_ += remainder;
                        result.state = ParseState::THINKING;
                        result.content = remainder;
                        return result;
                    }
                }
            } else {
                state_ = ParseState::ANSWERING;
                answer_buf_ += tag_buf_;
                tag_buf_.clear();
                result.state = ParseState::ANSWERING;
                result.content = token;
                return result;
            }
        }

        // Check if tag_buf might still be forming a <think tag
        if (tag_buf_.find('<') == std::string::npos || tag_buf_.size() > 6) {
            if (tag_buf_.find("<think") == std::string::npos) {
                state_ = ParseState::ANSWERING;
                answer_buf_ += tag_buf_;
                tag_buf_.clear();
                result.state = ParseState::ANSWERING;
                result.content = token;
                return result;
            }
        }

        result.state = ParseState::IDLE;
        return result;
    }

    if (state_ == ParseState::THINKING) {
        tag_buf_ += token;

        if (tag_buf_.find("</think") != std::string::npos) {
            size_t pos = tag_buf_.find("</think");
            reasoning_buf_ += tag_buf_.substr(0, pos);
            size_t end_pos = tag_buf_.find('>', pos);
            if (end_pos != std::string::npos) {
                state_ = ParseState::ANSWERING;
                std::string remainder = tag_buf_.substr(end_pos + 1);
                tag_buf_.clear();
                if (!remainder.empty()) {
                    answer_buf_ += remainder;
                    result.state = ParseState::ANSWERING;
                    result.content = remainder;
                    return result;
                }
            }
            result.state = ParseState::THINKING;
            return result;
        }

        if (tag_buf_.size() > 10) {
            reasoning_buf_ += tag_buf_;
            result.state = ParseState::THINKING;
            result.content = tag_buf_;
            tag_buf_.clear();
            return result;
        }

        result.state = ParseState::THINKING;
        return result;
    }

    if (state_ == ParseState::ANSWERING) {
        answer_buf_ += token;
        result.state = ParseState::ANSWERING;
        result.content = token;
        return result;
    }

    return result;
}

}

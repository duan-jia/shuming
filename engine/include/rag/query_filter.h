#pragma once

#include <string>
#include <unordered_set>

namespace fastllm {

struct QueryFilter {
    int min_difficulty = 0;   // 0=no filter, 1-4 stars
    int max_difficulty = 0;   // 0=no filter
    std::string dish_keyword; // empty=no filter, longest matching dish substring
    std::string category;     // empty=no filter, English dir name (soup/meat_dish/...)

    bool active() const {
        return min_difficulty > 0 || max_difficulty > 0
            || !dish_keyword.empty() || !category.empty();
    }
};

QueryFilter parse_query_filters(const std::string& query,
                                 const std::unordered_set<std::string>& known_dishes);

}

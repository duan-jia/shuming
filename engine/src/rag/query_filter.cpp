#include "rag/query_filter.h"

#include <algorithm>
#include <cstring>

namespace fastllm {

static bool has_any(const std::string& text, std::initializer_list<const char*> keywords) {
    for (auto kw : keywords) {
        if (text.find(kw) != std::string::npos) return true;
    }
    return false;
}

QueryFilter parse_query_filters(const std::string& query,
                                 const std::unordered_set<std::string>& known_dishes) {
    QueryFilter f;

    if (has_any(query, {"高难度", "最难", "复杂", "非常难"})) {
        f.min_difficulty = 4;
    } else if (has_any(query, {"困难", "有难度", "难度"})) {
        f.min_difficulty = 3;
    }

    if (has_any(query, {"非常简单", "超级简单"})) {
        f.max_difficulty = 1;
    } else if (has_any(query, {"简单", "新手", "容易", "初级"})) {
        f.max_difficulty = 2;
    }

    for (const auto& dish : known_dishes) {
        if (query.find(dish) != std::string::npos) {
            if (dish.size() > f.dish_keyword.size()) {
                f.dish_keyword = dish;
            }
        }
        for (size_t d0 = 0; d0 < dish.size(); ) {
            unsigned char c0 = static_cast<unsigned char>(dish[d0]);
            int clen0 = (c0 < 0x80) ? 1 : ((c0 & 0xE0) == 0xC0) ? 2 : ((c0 & 0xF0) == 0xE0) ? 3 : 4;
            for (size_t d1 = d0 + clen0; d1 < dish.size(); ) {
                unsigned char c1 = static_cast<unsigned char>(dish[d1]);
                int clen1 = (c1 < 0x80) ? 1 : ((c1 & 0xE0) == 0xC0) ? 2 : ((c1 & 0xF0) == 0xE0) ? 3 : 4;
                std::string sub = dish.substr(d0, d1 + clen1 - d0);
                if (sub.size() > f.dish_keyword.size() &&
                    query.find(sub) != std::string::npos) {
                    f.dish_keyword = sub;
                }
                d1 += clen1;
            }
            d0 += clen0;
        }
    }

    static const char* CAT_CN[] = {"素菜","荤菜","汤品","甜品","早餐","主食","水产","调料","饮品"};
    static const char* CAT_EN[] = {"vegetable_dish","meat_dish","soup","dessert",
                                    "breakfast","staple","aquatic","condiment","drink"};
    for (int i = 0; i < 9; ++i) {
        if (query.find(CAT_CN[i]) != std::string::npos) { f.category = CAT_EN[i]; break; }
    }

    return f;
}

}

#pragma once

#include "persistence/mysql_pool.h"

namespace fastllm {

class MysqlTransaction {
public:
    explicit MysqlTransaction(MysqlConnection& conn)
        : conn_(conn), active_(conn_.execute("START TRANSACTION")) {}

    ~MysqlTransaction() {
        if (active_) {
            conn_.execute("ROLLBACK");
        }
    }

    bool active() const { return active_; }

    bool commit() {
        if (!active_) return false;
        if (!conn_.execute("COMMIT")) return false;
        active_ = false;
        return true;
    }

    void rollback() {
        if (active_) {
            conn_.execute("ROLLBACK");
            active_ = false;
        }
    }

private:
    MysqlConnection& conn_;
    bool active_ = false;
};

}

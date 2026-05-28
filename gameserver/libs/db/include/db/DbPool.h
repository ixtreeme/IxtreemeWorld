#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>

#include "db/DbConfig.h"
#include "db/Result.h"

namespace sql {
class Connection;
}

namespace gs::db {

class DbPool {
public:
    DbPool(boost::asio::io_context& io, DbConfig config);
    ~DbPool();

    DbPool(const DbPool&) = delete;
    DbPool& operator=(const DbPool&) = delete;

    template <typename T>
    void Submit(std::function<Result<T>(sql::Connection&)> work,
                std::function<void(Result<T>)> completion);

    void SubmitVoid(std::function<VoidResult(sql::Connection&)> work,
                    std::function<void(VoidResult)> completion);

    void Start();
    void Stop();

private:
    struct PooledConnection {
        explicit PooledConnection(std::unique_ptr<sql::Connection> connection);
        ~PooledConnection();

        PooledConnection(const PooledConnection&) = delete;
        PooledConnection& operator=(const PooledConnection&) = delete;

        std::unique_ptr<sql::Connection> connection;
    };

    static sql::Connection& GetSqlConnection(PooledConnection& conn);

    std::unique_ptr<PooledConnection> AcquireConnection();
    void ReleaseConnection(std::unique_ptr<PooledConnection> conn);
    void WorkerLoop();

    boost::asio::io_context& io_;
    DbConfig config_;

    std::vector<std::thread> workers_;
    std::atomic<bool> stopping_{false};
    bool started_ = false;

    std::queue<std::function<void()>> tasks_;
    std::mutex tasks_mutex_;
    std::condition_variable tasks_cv_;

    std::vector<std::unique_ptr<PooledConnection>> free_connections_;
    std::mutex conn_mutex_;
    std::condition_variable conn_cv_;
};

template <typename T>
void DbPool::Submit(std::function<Result<T>(sql::Connection&)> work,
                    std::function<void(Result<T>)> completion)
{
    {
        std::lock_guard lk(tasks_mutex_);
        if (stopping_) {
            boost::asio::post(io_, [completion = std::move(completion)]() mutable {
                Result<T> r;
                r.error = DbError::ConnectionFailed;
                r.message = "db pool is stopping";
                completion(std::move(r));
            });
            return;
        }

        tasks_.push([this, work = std::move(work), completion = std::move(completion)]() mutable {
            auto conn = AcquireConnection();
            Result<T> result;
            if (!conn) {
                result.error = DbError::ConnectionFailed;
                result.message = "no connection available";
            } else {
                try {
                    result = work(GetSqlConnection(*conn));
                } catch (const std::exception& e) {
                    result.error = DbError::QueryFailed;
                    result.message = e.what();
                }
                ReleaseConnection(std::move(conn));
            }

            boost::asio::post(io_,
                              [completion = std::move(completion),
                               result = std::move(result)]() mutable {
                                  completion(std::move(result));
                              });
        });
    }
    tasks_cv_.notify_one();
}

} // namespace gs::db

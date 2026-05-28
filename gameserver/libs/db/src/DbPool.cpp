#include "db/DbPool.h"

#include <format>
#include <stdexcept>
#include <utility>

#include <mariadb/conncpp.hpp>

#include "common/Logging.h"

namespace gs::db {

DbPool::PooledConnection::PooledConnection(std::unique_ptr<sql::Connection> connection_)
    : connection(std::move(connection_))
{
}

DbPool::PooledConnection::~PooledConnection() = default;

DbPool::DbPool(boost::asio::io_context& io, DbConfig config)
    : io_(io)
    , config_(std::move(config))
{
}

DbPool::~DbPool()
{
    Stop();
}

sql::Connection& DbPool::GetSqlConnection(PooledConnection& conn)
{
    return *conn.connection;
}

void DbPool::Start()
{
    if (started_) {
        return;
    }

    stopping_ = false;
    const auto url = std::format("jdbc:mariadb://{}:{}/{}",
                                 config_.host,
                                 config_.port,
                                 config_.database);

    sql::Properties props;
    props["user"] = config_.user;
    props["password"] = config_.password;
    props["connectTimeout"] = std::to_string(config_.connect_timeout_ms);

    {
        std::lock_guard lk(conn_mutex_);
        free_connections_.reserve(config_.pool_size);
        for (std::uint32_t i = 0; i < config_.pool_size; ++i) {
            std::unique_ptr<sql::Connection> conn(
                sql::DriverManager::getConnection(sql::SQLString(url), props));
            if (!conn || conn->isClosed()) {
                throw std::runtime_error("failed to open database connection");
            }
            free_connections_.push_back(std::make_unique<PooledConnection>(std::move(conn)));
        }
    }

    workers_.reserve(config_.thread_pool_size);
    for (std::uint32_t i = 0; i < config_.thread_pool_size; ++i) {
        workers_.emplace_back([this] {
            WorkerLoop();
        });
    }

    started_ = true;
    LOG_INFO("DB pool started: {} connections, {} worker threads",
             config_.pool_size,
             config_.thread_pool_size);
}

void DbPool::Stop()
{
    if (!started_) {
        return;
    }

    stopping_ = true;
    tasks_cv_.notify_all();
    conn_cv_.notify_all();

    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();

    {
        std::lock_guard lk(conn_mutex_);
        free_connections_.clear();
    }

    started_ = false;
    LOG_INFO("DB pool stopped");
}

void DbPool::SubmitVoid(std::function<VoidResult(sql::Connection&)> work,
                        std::function<void(VoidResult)> completion)
{
    {
        std::lock_guard lk(tasks_mutex_);
        if (stopping_) {
            boost::asio::post(io_, [completion = std::move(completion)]() mutable {
                VoidResult result;
                result.error = DbError::ConnectionFailed;
                result.message = "db pool is stopping";
                completion(std::move(result));
            });
            return;
        }

        tasks_.push([this, work = std::move(work), completion = std::move(completion)]() mutable {
            auto conn = AcquireConnection();
            VoidResult result;
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

std::unique_ptr<DbPool::PooledConnection> DbPool::AcquireConnection()
{
    std::unique_lock lk(conn_mutex_);
    conn_cv_.wait(lk, [this] {
        return stopping_ || !free_connections_.empty();
    });

    if (free_connections_.empty()) {
        return nullptr;
    }

    auto conn = std::move(free_connections_.back());
    free_connections_.pop_back();
    return conn;
}

void DbPool::ReleaseConnection(std::unique_ptr<PooledConnection> conn)
{
    if (!conn) {
        return;
    }

    {
        std::lock_guard lk(conn_mutex_);
        free_connections_.push_back(std::move(conn));
    }
    conn_cv_.notify_one();
}

void DbPool::WorkerLoop()
{
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock lk(tasks_mutex_);
            tasks_cv_.wait(lk, [this] {
                return stopping_ || !tasks_.empty();
            });

            if (stopping_ && tasks_.empty()) {
                return;
            }

            task = std::move(tasks_.front());
            tasks_.pop();
        }

        task();
    }
}

const char* DbErrorString(DbError e)
{
    switch (e) {
    case DbError::Ok:
        return "Ok";
    case DbError::ConnectionFailed:
        return "ConnectionFailed";
    case DbError::QueryFailed:
        return "QueryFailed";
    case DbError::NotFound:
        return "NotFound";
    case DbError::InvalidCredentials:
        return "InvalidCredentials";
    case DbError::AccountBanned:
        return "AccountBanned";
    case DbError::DuplicateKey:
        return "DuplicateKey";
    case DbError::InternalError:
        return "InternalError";
    }

    return "Unknown";
}

} // namespace gs::db

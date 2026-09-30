#include "db/DbPool.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#include <boost/asio/executor_work_guard.hpp>
#include <mariadb/conncpp.hpp>

using namespace std::chrono_literals;
namespace {
unsigned assertions = 0;
void Check(bool ok, const char* message)
{
    ++assertions;
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
template<class T> T Await(std::future<T>& result)
{
    Check(result.wait_for(10s) == std::future_status::ready, "callback deadline");
    return result.get();
}
int Scalar(sql::Connection& conn, const char* query)
{
    std::unique_ptr<sql::Statement> stmt(conn.createStatement());
    std::unique_ptr<sql::ResultSet> rows(stmt->executeQuery(query));
    return rows->next() ? rows->getInt(1) : 0;
}
void Execute(sql::Connection& conn, const char* query)
{
    std::unique_ptr<sql::Statement> stmt(conn.createStatement()); stmt->execute(query);
}
}

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "Usage: db_pool_liveness_test <authorized-local-db-config-path>\n";
        return 2;
    }
    try {
        auto cfg = gs::db::LoadDbConfig(argv[1]);
        Check(cfg.host == "127.0.0.1" || cfg.host == "localhost" || cfg.host == "::1",
              "only explicitly configured local database");
        cfg.pool_size = 1; cfg.thread_pool_size = 2;
        boost::asio::io_context io;
        auto guard = boost::asio::make_work_guard(io);
        gs::db::DbPool pool(io, cfg); pool.Start();
        std::thread runner([&] { io.run(); });
        auto submit = [&](auto work) {
            auto promise = std::make_shared<std::promise<gs::db::Result<int>>>();
            auto result = promise->get_future();
            pool.Submit<int>(work, [promise](auto r) { promise->set_value(std::move(r)); });
            return Await(result);
        };
        auto submitVoid = [&](auto work) {
            auto promise = std::make_shared<std::promise<gs::db::VoidResult>>();
            auto result = promise->get_future();
            pool.SubmitVoid(work, [promise](auto r) { promise->set_value(std::move(r)); });
            return Await(result);
        };
        auto scalar = [&](const char* query) {
            return submit([query](sql::Connection& c) {
                gs::db::Result<int> r; r.value = Scalar(c, query); return r;
            });
        };
        Check(scalar("SELECT 1").value == 1, "healthy query");
        auto timeout = submit([](sql::Connection& c) {
            gs::db::Result<int> r; r.value = c.getNetworkTimeout(); return r;
        }).value;
        auto sameTimeout = submit([](sql::Connection& c) {
            gs::db::Result<int> r; r.value = c.getNetworkTimeout(); return r;
        }).value;
        Check(timeout == sameTimeout, "health check preserves query timeout");

        auto expire = [&] {
            Check(submitVoid([](sql::Connection& c) {
                Execute(c, "SET SESSION wait_timeout=1"); return gs::db::VoidResult{};
            }).IsOk(), "set timeout on test-owned session only");
            std::this_thread::sleep_for(2200ms);
        };
        expire();
        Check(scalar("SELECT 1").value == 1, "Submit recovers expired connection");
        Check(scalar("SELECT 1").value == 1, "replacement stays usable");
        expire();
        Check(submitVoid([](sql::Connection& c) {
            Execute(c, "SET @i1_liveness_value=7"); return gs::db::VoidResult{};
        }).IsOk(), "SubmitVoid recovers expired connection");
        Check(scalar("SELECT @i1_liveness_value").value == 7, "session mutation ran once");

        unsigned invocations = 0;
        auto failed = submit([&](sql::Connection& c) -> gs::db::Result<int> {
            ++invocations;
            Execute(c, "SET @i1_liveness_value=@i1_liveness_value+1");
            throw std::runtime_error("test failure after operation; must not replay");
        });
        Check(failed.error == gs::db::DbError::QueryFailed && invocations == 1,
              "failed work is not replayed");
        Check(scalar("SELECT @i1_liveness_value").value == 8, "no duplicate side effect");
        invocations = 0;
        auto voidFailed = submitVoid([&](sql::Connection& c) -> gs::db::VoidResult {
            ++invocations; c.close(); throw std::runtime_error("test disconnect during work");
        });
        Check(voidFailed.error == gs::db::DbError::QueryFailed && invocations == 1,
              "disconnect during SubmitVoid does not replay work");
        Check(scalar("SELECT 1").value == 1, "next operation recovers closed slot");

        std::vector<std::future<gs::db::Result<int>>> futures;
        for (int i=0; i<16; ++i) {
            auto p=std::make_shared<std::promise<gs::db::Result<int>>>();
            futures.push_back(p->get_future());
            pool.Submit<int>([](sql::Connection& c) {
                gs::db::Result<int> r; r.value=Scalar(c,"SELECT 1"); return r;
            },[p](auto r) { p->set_value(std::move(r)); });
        }
        for (auto& f : futures) Check(Await(f).value == 1, "queued borrowers make progress");
        pool.Stop();
        auto stopped=scalar("SELECT 1");
        Check(stopped.error == gs::db::DbError::ConnectionFailed, "stopped pool rejects work");
        guard.reset(); runner.join();
        std::cout << "PASS assertions=" << assertions << '\n'; return 0;
    } catch (...) {
        std::cerr << "Test setup failed; exception details suppressed to protect config secrets\n";
        return 2;
    }
}

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <sodium.h>

#include "common/Logging.h"
#include "db/AccountRepository.h"
#include "db/CharacterRepository.h"
#include "db/DbConfig.h"
#include "db/DbPool.h"
#include "db/Result.h"

namespace {

void PrintUsage()
{
    std::cerr << "Usage:\n"
              << "  db_test --make-hash <password>\n"
              << "  db_test --login <username> <password>\n";
}

int MakeHash(std::string password)
{
    if (sodium_init() < 0) {
        std::cerr << "libsodium initialization failed\n";
        return 1;
    }

    char hash[crypto_pwhash_STRBYTES]{};
    if (crypto_pwhash_str(hash,
                          password.c_str(),
                          static_cast<unsigned long long>(password.size()),
                          crypto_pwhash_OPSLIMIT_INTERACTIVE,
                          crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0) {
        sodium_memzero(password.data(), password.size());
        std::cerr << "argon2id hash generation failed\n";
        return 1;
    }

    sodium_memzero(password.data(), password.size());
    std::cout << hash << '\n';
    return 0;
}

int Login(std::string username, std::string password)
{
    try {
        gs::common::InitLogging("info", "logs/db_test.log");

        auto config = gs::db::LoadDbConfig("database.json");
        boost::asio::io_context io;
        auto work_guard = boost::asio::make_work_guard(io);
        gs::db::DbPool pool(io, std::move(config));
        pool.Start();

        gs::db::AccountRepository accounts(pool);
        gs::db::CharacterRepository characters(pool);

        int exit_code = 1;
        accounts.Authenticate(std::move(username),
                              std::move(password),
                              [&](gs::db::Result<gs::db::Account> result) {
                                  if (!result || !result.value) {
                                      LOG_ERROR("login failed: {} ({})",
                                                gs::db::DbErrorString(result.error),
                                                result.message);
                                      work_guard.reset();
                                      return;
                                  }

                                  const auto account = *result.value;
                                  LOG_INFO("login OK: id={}, username={}",
                                           static_cast<std::uint64_t>(account.id),
                                           account.username);

                                  characters.ListByAccount(
                                      account.id,
                                      [&](gs::db::Result<std::vector<gs::db::Character>>
                                              char_result) {
                                          if (!char_result || !char_result.value) {
                                              LOG_ERROR("character list failed: {} ({})",
                                                        gs::db::DbErrorString(
                                                            char_result.error),
                                                        char_result.message);
                                          } else {
                                              LOG_INFO("characters: {}", char_result.value->size());
                                              for (const auto& character : *char_result.value) {
                                                  LOG_INFO(
                                                      "  slot={} name='{}' level={} class={}",
                                                      character.slot,
                                                      character.name,
                                                      character.level,
                                                      character.class_id);
                                              }
                                              exit_code = 0;
                                          }

                                          work_guard.reset();
                                      });
                              });

        io.run();
        pool.Stop();
        return exit_code;
    } catch (const std::exception& e) {
        std::cerr << "db_test failed: " << e.what() << '\n';
        return 1;
    }
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc == 3 && std::string(argv[1]) == "--make-hash") {
        return MakeHash(argv[2]);
    }

    if (argc == 4 && std::string(argv[1]) == "--login") {
        return Login(argv[2], argv[3]);
    }

    PrintUsage();
    return 1;
}

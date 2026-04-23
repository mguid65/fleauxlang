#include <catch2/catch_session.hpp>
#include <catch2/catch_test_case_info.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include <iostream>
#include <string>
#include <vector>

namespace {

struct FailedTestEntry {
  std::string test_name;
  std::string file;
  int line = 0;
};

class FailureSummaryListener final : public Catch::EventListenerBase {
public:
  using Catch::EventListenerBase::EventListenerBase;

  void testCaseEnded(const Catch::TestCaseStats& test_case_stats) override {
    if (test_case_stats.totals.assertions.failed == 0) { return; }

    FailedTestEntry entry;
    entry.test_name = test_case_stats.testInfo->name;
    entry.file = test_case_stats.testInfo->lineInfo.file;
    entry.line = test_case_stats.testInfo->lineInfo.line;
    failed_tests_.push_back(std::move(entry));
  }

  void testRunEnded(const Catch::TestRunStats&) override {
    if (failed_tests_.empty()) { return; }

    std::cout << "\n========================================\n";
    std::cout << "Failing tests summary\n";
    std::cout << "========================================\n";
    for (const auto& failed_test : failed_tests_) {
      std::cout << "- " << failed_test.test_name << " (" << failed_test.file << ":" << failed_test.line << ")\n";
    }
    std::cout << "========================================\n";
  }

private:
  std::vector<FailedTestEntry> failed_tests_;
};

CATCH_REGISTER_LISTENER(FailureSummaryListener)

}  // namespace

int main(const int argc, char* argv[]) {
  Catch::Session session;
  return session.run(argc, argv);
}

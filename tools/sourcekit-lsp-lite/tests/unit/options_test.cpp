// The command line and initializationOptions.

#include "options.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace lite;

TEST_SUITE("options") {
  TEST_CASE("command-line options set what they name, and -- starts compiler arguments") {
    Options O;
    const std::vector<std::string> Args{"--root", "/p",   "--exclude", "Tests/", "--log",
                                        "--",     "-sdk", "/s",        "--root"};
    CHECK(parseArguments(Args, O).empty());
    CHECK(O.Root == "/p");
    CHECK(O.Exclude == std::vector<std::string>{"Tests/"});
    CHECK(O.Log);
    CHECK(O.CompilerArgs == std::vector<std::string>{"-sdk", "/s", "--root"});
  }

  TEST_CASE("a bad command line says what is wrong") {
    Options O;
    CHECK(parseArguments(std::vector<std::string>{"--root"}, O) == "--root needs a directory");
    CHECK(parseArguments(std::vector<std::string>{"--max-completions", "0"}, O)
              .starts_with("--max-completions"));
    CHECK(parseArguments(std::vector<std::string>{"--bogus"}, O) == "unknown argument: --bogus");
  }

  TEST_CASE("initializationOptions add to the command line, and wrong types are ignored") {
    Options O;
    O.CompilerArgs = {"-a"};
    const auto Init = json::parse(
        R"({"compilerArgs":["-b", 3],"moduleName":"App","scan":false,"maxCompletions":5,"exclude":"x"})");
    REQUIRE(Init);
    applyInitializationOptions(*Init, O);
    CHECK(O.CompilerArgs == std::vector<std::string>{"-a", "-b"});
    CHECK(O.ModuleName == "App");
    CHECK_FALSE(O.Scan);
    CHECK(O.MaxCompletions == 5);
    CHECK(O.Exclude.empty());
  }

  TEST_CASE("the module is the one @testable import names, if exactly one") {
    CHECK(inferModuleName(std::vector<std::string>{
              "let a = 1", "import XCTest\n@testable import App\n"}) == "App");
    CHECK(inferModuleName(std::vector<std::string>{"@testable import A", "@testable import B"}) ==
          "main");
    CHECK(inferModuleName(std::vector<std::string>{}) == "main");
  }
}

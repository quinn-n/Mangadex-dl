# tests/e2e/cases/hello.cmake: installed-binary cases for the scaffold's `hello` subcommand
# (tests e2e.hello.<case>, label e2e). One file per subcommand, globbed, so a track that adds a
# subcommand adds its own cases/<sub>.cmake without touching shared CMake.
#
# Always an exact EXPECT_EXIT, and stdout/stderr asserted separately. A GOLDEN case's expected output
# comes only from `pixi run bless` (human-approved); never hand-write tests/e2e/golden/*.

cli_e2e_case(NAME default ARGS hello
             EXPECT_EXIT 0 EXPECT_STDERR_EMPTY GOLDEN)
cli_e2e_case(NAME named ARGS hello Ada
             EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "^Hello, Ada!\n$" EXPECT_STDERR_EMPTY)
cli_e2e_case(NAME json ARGS hello --json Ada
             EXPECT_EXIT 0 EXPECT_JSON EXPECT_STDOUT_REGEX "Hello, Ada!" EXPECT_STDERR_EMPTY)
cli_e2e_case(NAME unknown-option ARGS hello --definitely-not-an-option
             EXPECT_EXIT 2 EXPECT_STDOUT_EMPTY EXPECT_STDERR_REGEX ".")

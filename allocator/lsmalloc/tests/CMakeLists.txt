include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG release-1.12.1
)
FetchContent_MakeAvailable(googletest)

set(TEST_NAMES
  # basic_test
  random_ops_test
)

foreach(test_name ${TEST_NAMES})
    add_executable(${test_name} ${test_name}.cc)
    target_link_libraries(${test_name} PRIVATE gtest_main lsmalloc)
endforeach()

enable_testing()
foreach(test_name ${TEST_NAMES})
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

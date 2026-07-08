add_executable(torrentx_tests
    test_main.cpp
)

target_link_libraries(torrentx_tests PRIVATE torrentx_core)

if(MSVC)
    target_compile_options(torrentx_tests PRIVATE /W4)
else()
    target_compile_options(torrentx_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME unit_tests COMMAND torrentx_tests)

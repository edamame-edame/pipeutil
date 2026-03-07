# tests/cpp/CMakeLists.txt — C++ ユニット / 統合テスト

# ─── ヘルパーマクロ ───────────────────────────────────────────────────
# usage: pipeutil_add_test(target source1.cpp [source2.cpp ...])
macro(pipeutil_add_test TARGET_NAME)
    add_executable(${TARGET_NAME} ${ARGN})
    target_link_libraries(${TARGET_NAME} PRIVATE
        pipeutil_core
        GTest::gtest_main
    )
    target_compile_options(${TARGET_NAME} PRIVATE
        # /wd4834: [[nodiscard]] 戻り値を EXPECT_THROW マクロが破棄する際の警告を抑止
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX /utf-8 /wd4251 /wd4275 /wd4834>
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic -Werror>
    )
    # DLL を実行ファイルと同じ出力ディレクトリに配置（Windows 実行に必要）
    if(WIN32)
        add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:pipeutil_core>
                $<TARGET_FILE_DIR:${TARGET_NAME}>/$<TARGET_FILE_NAME:pipeutil_core>
            COMMENT "Copying pipeutil_core.dll to ${TARGET_NAME} output dir"
        )
    endif()
    # CTest に自動登録
    gtest_discover_tests(${TARGET_NAME})
endmacro()

# ─── テストターゲット ─────────────────────────────────────────────────
pipeutil_add_test(test_message    test_message.cpp)
pipeutil_add_test(test_io_buffer  test_io_buffer.cpp)
pipeutil_add_test(test_error      test_error.cpp)
pipeutil_add_test(test_roundtrip  test_roundtrip.cpp)
pipeutil_add_test(test_multi_pipe_server  test_multi_pipe_server.cpp)

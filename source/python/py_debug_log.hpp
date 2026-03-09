// py_debug_log.hpp — C 拡張デバッグログマクロ
//
// ビルド時に -DPIPEUTIL_DEBUG_LOG を渡すと stderr にログを出力する。
// リリースビルドでは何も生成されないため実行時コストはゼロ。
//
// 使用例:
//   PIPELOG("connect: before GIL release, timeout_ms=%lld", (long long)ms);
//
#pragma once

#ifdef PIPEUTIL_DEBUG_LOG

#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>   // GetCurrentThreadId
#  include <cstdio>      // fprintf, stderr

/// スレッド ID 付きのデバッグ出力。リリース時は完全に除去される。
#  define PIPELOG(fmt, ...)                                                 \
     do {                                                                   \
         fprintf(stderr,                                                    \
                 "[PIPELOG TID=%lu] %s:%d " fmt "\n",                      \
                 (unsigned long)GetCurrentThreadId(),                       \
                 __FILE__, __LINE__,                                        \
                 ##__VA_ARGS__);                                            \
         fflush(stderr);                                                    \
     } while (0)

#else  // PIPEUTIL_DEBUG_LOG は未定義 → ログ無効

#  define PIPELOG(fmt, ...) do {} while (0)

#endif // PIPEUTIL_DEBUG_LOG

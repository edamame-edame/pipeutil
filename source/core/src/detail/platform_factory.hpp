// platform_factory.hpp — プラットフォーム別 IPlatformPipe 実装の選択
// このヘッダは pipe_server.cpp / pipe_client.cpp のみからインクルードする。
// 公開ヘッダに含めてはならない（実装詳細の漏洩防止）。
#pragma once

#ifdef _WIN32
#  include "platform/win32_pipe.hpp"
namespace pipeutil::detail {
    using PlatformPipeImpl = Win32Pipe;
}
#else
#  include "platform/posix_pipe.hpp"
namespace pipeutil::detail {
    using PlatformPipeImpl = PosixPipe;
}
#endif

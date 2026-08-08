#pragma once
#include <string>
#include <algorithm>

namespace NoticeClient {
    struct EolResult {
        bool ok = false;
        std::wstring title;
        std::wstring version;
        std::wstring body;
        std::wstring error;

        EolResult() : ok(false) {}
        explicit EolResult(bool success) : ok(success) {}
    };

    EolResult FetchEolNotice();
}

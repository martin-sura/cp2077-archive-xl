#pragma once

// Diagnostic switches for the Patch 2.5 investigation (see DEBUGGING_NOTES.md).
// A setting is read from the process environment first, then from "axl_debug.ini"
// next to ArchiveXL.dll (one KEY=VALUE per line, '#' starts a comment).
// The file exists because launchers like Steam do not pass console environment variables to the game.
namespace App::DebugSettings
{
inline std::filesystem::path GetFilePath()
{
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&GetFilePath), &module);

    wchar_t modulePath[MAX_PATH] = {};
    GetModuleFileNameW(module, modulePath, MAX_PATH);

    return std::filesystem::path(modulePath).parent_path() / L"axl_debug.ini";
}

inline std::string Get(std::string_view aName)
{
    char envBuf[256] = {};
    const auto envLen = GetEnvironmentVariableA(std::string(aName).c_str(), envBuf, sizeof(envBuf));
    if (envLen > 0 && envLen < sizeof(envBuf))
        return envBuf;

    std::ifstream file(GetFilePath());
    std::string line;
    while (std::getline(file, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        if (line.empty() || line[0] == '#')
            continue;

        const auto sep = line.find('=');
        if (sep != std::string::npos && std::string_view(line).substr(0, sep) == aName)
            return line.substr(sep + 1);
    }

    return {};
}
}

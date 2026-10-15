// Converts the GoPro Reframe clips of a DaVinci Resolve timeline file (.drt) to Max2 Reframe.
#pragma once

#include <string>
#include <utility>
#include <vector>

struct ConvertResult
{
    std::string timeline;            // timeline name in the new file
    int goproClips = 0;              // clips with a GoPro Reframe Fusion composition
    int converted = 0;               // of these, the clips that now use Max2 Reframe
    std::vector<std::string> notes;  // one line for each clip that was not converted
    std::vector<std::string> log;    // one line for each converted clip, with the values used (for tests)
    // GoPro Player export -> its .360 file (UTF-8 paths), for each converted clip's file
    std::vector<std::pair<std::string, std::string>> sources;
};

// Reads `in`, writes `out` (which can be the same file). Returns false only if the file cannot be read or written;
// clips that cannot be converted are left as they are and listed in `notes`.
bool convertTimelineFile(const std::wstring& in, const std::wstring& out, ConvertResult& result, std::string* err);

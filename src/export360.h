// Finds the .360 file that a GoPro Player export (an equirectangular .mp4) was made from.
#pragma once

#include <string>

// For an export such as "D:\Trip\MAX2_equi\GS011840.mp4", looks for "GS011840.360" in the export's folder, in the
// folders next to it (and one level below them) and in the parent folder. A candidate must have the same number of
// video frames and the same start timecode as the export. Returns false if `path` is not a single-stream video, or
// if no .360 file matches; `why` then says why.
bool findSource360ForExport(const std::wstring& path, std::wstring& source360, std::string* why = nullptr);

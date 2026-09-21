#include "OchPackReader.h"
#include "platform/Storage.h"
#include "platform/Log.h"
#include "platform/storage/PosixFileSystem.h"
#include "unzip.h"

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <sstream>

static void parseModInfoString(const std::string &content, OchPackInfo &outInfo)
{
    std::istringstream stream(content);
    std::string line;
    while (std::getline(stream, line))
    {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
            line.erase(line.begin());

        if (line.empty() || line[0] == '#' || line[0] == ';')
            continue;

        size_t sep = line.find('=');
        if (sep == std::string::npos)
            sep = line.find(':');

        if (sep != std::string::npos)
        {
            std::string key = line.substr(0, sep);
            std::string val = line.substr(sep + 1);

            while (!key.empty() && (key.back() == ' ' || key.back() == '\t'))
                key.pop_back();
            while (!val.empty() && (val.front() == ' ' || val.front() == '\t'))
                val.erase(val.begin());

            std::transform(key.begin(), key.end(), key.begin(), ::tolower);

            if (key == "id")
                outInfo.id = val;
            else if (key == "name")
                outInfo.name = val;
            else if (key == "version")
                outInfo.version = val;
            else if (key == "author")
                outInfo.author = val;
            else if (key == "description")
                outInfo.description = val;
        }
    }
}

namespace OchPackReader
{
bool readInfo(const std::string &filePath, OchPackInfo &outInfo)
{
    outInfo.valid = false;
    outInfo.filePath = filePath;

    size_t lastSep = filePath.find_last_of("/\\");
    outInfo.fileName = (lastSep == std::string::npos) ? filePath : filePath.substr(lastSep + 1);

    unzFile uf = unzOpen(filePath.c_str());
    if (!uf)
        uf = unzOpen64(filePath.c_str());

    // If still null, try adding slash after colon (e.g. host:mods/... -> host:/mods/...)
    if (!uf)
    {
        size_t colon = filePath.find(':');
        if (colon != std::string::npos && colon + 1 < filePath.size() && filePath[colon + 1] != '/')
        {
            std::string alt = filePath.substr(0, colon + 1) + "/" + filePath.substr(colon + 1);
            uf = unzOpen(alt.c_str());
            if (!uf)
                uf = unzOpen64(alt.c_str());
            if (uf)
                outInfo.filePath = alt;
        }
    }

    if (!uf)
    {
        MC_LOG_WARN("mods", "OchPackReader: Failed to open '%s'\n", filePath.c_str());
        return false;
    }

    int res = unzLocateFile(uf, "mod.info", 2);
    if (res != UNZ_OK)
        res = unzLocateFile(uf, "manifest.txt", 2);

    if (res != UNZ_OK)
    {
        MC_LOG_WARN("mods", "OchPackReader: No mod.info or manifest.txt in '%s'\n", filePath.c_str());
        unzClose(uf);
        return false;
    }

    unz_file_info fileInfo;
    if (unzGetCurrentFileInfo(uf, &fileInfo, nullptr, 0, nullptr, 0, nullptr, 0) != UNZ_OK)
    {
        unzClose(uf);
        return false;
    }

    if (unzOpenCurrentFile(uf) != UNZ_OK)
    {
        unzClose(uf);
        return false;
    }

    uLong toRead = fileInfo.uncompressed_size > 4096 ? 4096 : fileInfo.uncompressed_size;
    std::vector<char> buffer(toRead + 1, 0);
    int readBytes = unzReadCurrentFile(uf, buffer.data(), static_cast<unsigned int>(toRead));
    unzCloseCurrentFile(uf);
    unzClose(uf);

    if (readBytes <= 0)
        return false;

    buffer[readBytes] = '\0';
    parseModInfoString(std::string(buffer.data()), outInfo);

    if (outInfo.id.empty())
    {
        std::string base = outInfo.fileName;
        size_t dot = base.find_last_of('.');
        if (dot != std::string::npos)
            base = base.substr(0, dot);
        outInfo.id = base;
    }
    if (outInfo.name.empty())
        outInfo.name = outInfo.id;
    if (outInfo.version.empty())
        outInfo.version = "1.0";

    outInfo.valid = true;
    return true;
}

std::vector<OchPackInfo> scanDirectory(const std::string &dirPath)
{
    std::vector<OchPackInfo> results;
    std::vector<std::string> candidateFiles;

    // 1. Try reading packlist.txt or mods.list in this directory
    static const char* const LIST_NAMES[] = {
        "packlist.txt",
        "mods.list",
        "mods.txt"
    };

    for (const char* listName : LIST_NAMES)
    {
        std::string listPath = PlatformStorage::join(dirPath, listName);
        std::vector<unsigned char> listData;
        if (PlatformStorage::readFile(listPath, listData) && !listData.empty())
        {
            std::string listContent(listData.begin(), listData.end());
            std::istringstream stream(listContent);
            std::string line;
            while (std::getline(stream, line))
            {
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
                    line.pop_back();
                while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
                    line.erase(line.begin());

                if (!line.empty() && line[0] != '#' && line[0] != ';')
                {
                    candidateFiles.push_back(line);
                }
            }
            break;
        }
    }

    // 2. Built-in candidate probe list for filesystems where opendir() is not supported (e.g. PCSX2 host:, CD-ROM)
    static const char* const PROBE_NAMES[] = {
        "TooManyItems.ochpack",
        "toomanyitems.ochpack",
        "ReiMinimap.ochpack",
        "reiminimap.ochpack",
        "SampleTestMod.ochpack",
        "sampletestmod.ochpack",
        "CraftGuide.ochpack",
        "craftguide.ochpack",
        "OptiFine.ochpack",
        "optifine.ochpack",
        "Mod.ochpack",
        "mod.ochpack",
        "Test.ochpack",
        "test.ochpack"
    };

    for (const char* probeName : PROBE_NAMES)
    {
        bool alreadyCandidate = false;
        for (const auto& c : candidateFiles)
        {
            if (c == probeName)
            {
                alreadyCandidate = true;
                break;
            }
        }
        if (!alreadyCandidate)
            candidateFiles.push_back(probeName);
    }

    // 3. Try standard directory enumeration (for platforms where opendir works, like PC & USB FAT32)
    std::vector<std::string> entries;
    if (PlatformStorage::listPathEntries(dirPath, entries))
    {
        for (const auto &entry : entries)
        {
            if (entry.size() >= 8)
            {
                std::string ext = entry.substr(entry.size() - 8);
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext == ".ochpack")
                {
                    bool alreadyCandidate = false;
                    for (const auto& c : candidateFiles)
                    {
                        if (c == entry)
                        {
                            alreadyCandidate = true;
                            break;
                        }
                    }
                    if (!alreadyCandidate)
                        candidateFiles.push_back(entry);
                }
            }
        }
    }

    // 4. Test each candidate file
    for (const auto &fileName : candidateFiles)
    {
        std::string fullPath = PlatformStorage::join(dirPath, fileName);
        if (PlatformStorage::fileReadable(fullPath))
        {
            OchPackInfo info;
            if (readInfo(fullPath, info))
            {
                bool duplicate = false;
                for (const auto &existing : results)
                {
                    if (existing.id == info.id)
                    {
                        duplicate = true;
                        break;
                    }
                }
                if (!duplicate)
                    results.push_back(info);
            }
        }
        else
        {
            // Try alternative slash after colon (e.g. host:mods/file -> host:/mods/file)
            size_t colon = fullPath.find(':');
            if (colon != std::string::npos && colon + 1 < fullPath.size() && fullPath[colon + 1] != '/')
            {
                std::string alt = fullPath.substr(0, colon + 1) + "/" + fullPath.substr(colon + 1);
                if (PlatformStorage::fileReadable(alt))
                {
                    OchPackInfo info;
                    if (readInfo(alt, info))
                    {
                        bool duplicate = false;
                        for (const auto &existing : results)
                        {
                            if (existing.id == info.id)
                            {
                                duplicate = true;
                                break;
                            }
                        }
                        if (!duplicate)
                            results.push_back(info);
                    }
                }
            }
        }
    }

    return results;
}
}

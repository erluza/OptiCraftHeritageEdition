#include "OchPackReader.h"
#include "platform/Storage.h"
#include "platform/Log.h"
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
    std::vector<std::string> entries;
    if (!PlatformStorage::listPathEntries(dirPath, entries))
        return results;

    for (const auto &entry : entries)
    {
        if (entry.size() < 8)
            continue;

        std::string ext = entry.substr(entry.size() - 8);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".ochpack")
        {
            std::string fullPath = PlatformStorage::join(dirPath, entry);
            OchPackInfo info;
            if (readInfo(fullPath, info))
            {
                results.push_back(info);
            }
        }
    }
    return results;
}
}

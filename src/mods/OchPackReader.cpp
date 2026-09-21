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

// In-memory stream buffer for minizip to avoid buggy IOP seek drivers
struct MemZipBuffer
{
    const unsigned char *buffer;
    uLong size;
    uLong pos;
};

static voidpf ZCALLBACK mem_open(voidpf opaque, const char *filename, int mode)
{
    return opaque;
}

static uLong ZCALLBACK mem_read(voidpf opaque, voidpf stream, void *buf, uLong size)
{
    MemZipBuffer *m = static_cast<MemZipBuffer *>(stream);
    if (!m || m->pos >= m->size)
        return 0;
    uLong avail = m->size - m->pos;
    uLong toRead = (size < avail) ? size : avail;
    std::memcpy(buf, m->buffer + m->pos, toRead);
    m->pos += toRead;
    return toRead;
}

static long ZCALLBACK mem_tell(voidpf opaque, voidpf stream)
{
    MemZipBuffer *m = static_cast<MemZipBuffer *>(stream);
    return m ? static_cast<long>(m->pos) : -1;
}

static long ZCALLBACK mem_seek(voidpf opaque, voidpf stream, uLong offset, int origin)
{
    MemZipBuffer *m = static_cast<MemZipBuffer *>(stream);
    if (!m)
        return -1;
    switch (origin)
    {
    case ZLIB_FILEFUNC_SEEK_CUR:
        m->pos += offset;
        break;
    case ZLIB_FILEFUNC_SEEK_END:
        m->pos = m->size + offset;
        break;
    case ZLIB_FILEFUNC_SEEK_SET:
        m->pos = offset;
        break;
    default:
        return -1;
    }
    return 0;
}

static int ZCALLBACK mem_close(voidpf opaque, voidpf stream)
{
    return 0;
}

static int ZCALLBACK mem_error(voidpf opaque, voidpf stream)
{
    return 0;
}

static bool readFileBytes(const std::string &path, std::vector<unsigned char> &out, std::string *resolvedPath = nullptr)
{
    std::vector<std::string> candidates;
    candidates.push_back(path);

    // Backslash variant
    std::string bs = path;
    for (char &c : bs)
        if (c == '/')
            c = '\\';
    if (bs != path)
        candidates.push_back(bs);

    // Slash after colon (host:mods -> host:/mods)
    size_t colon = path.find(':');
    if (colon != std::string::npos && colon + 1 < path.size() && path[colon + 1] != '/' && path[colon + 1] != '\\')
    {
        std::string withSlash = path.substr(0, colon + 1) + "/" + path.substr(colon + 1);
        candidates.push_back(withSlash);

        std::string withBs = path.substr(0, colon + 1) + "\\" + path.substr(colon + 1);
        for (char &c : withBs)
            if (c == '/')
                c = '\\';
        candidates.push_back(withBs);
    }

    for (const auto &candidate : candidates)
    {
        FILE *f = std::fopen(candidate.c_str(), "rb");
        if (f)
        {
            std::fseek(f, 0, SEEK_END);
            long sz = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            if (sz > 0)
            {
                out.resize(static_cast<size_t>(sz));
                size_t readCount = std::fread(out.data(), 1, static_cast<size_t>(sz), f);
                std::fclose(f);
                if (readCount == static_cast<size_t>(sz))
                {
                    if (resolvedPath)
                        *resolvedPath = candidate;
                    return true;
                }
            }
            else
            {
                std::fclose(f);
            }
        }
    }
    return false;
}

namespace OchPackReader
{
bool readInfoFromBytes(const std::vector<unsigned char> &data, const std::string &originalPath, OchPackInfo &outInfo)
{
    outInfo.valid = false;
    outInfo.filePath = originalPath;

    size_t lastSep = originalPath.find_last_of("/\\");
    outInfo.fileName = (lastSep == std::string::npos) ? originalPath : originalPath.substr(lastSep + 1);

    if (data.size() < 22)
        return false;

    MemZipBuffer memBuf{ data.data(), static_cast<uLong>(data.size()), 0 };
    zlib_filefunc_def filefunc;
    filefunc.zopen_file = mem_open;
    filefunc.zread_file = mem_read;
    filefunc.zwrite_file = nullptr;
    filefunc.ztell_file = mem_tell;
    filefunc.zseek_file = mem_seek;
    filefunc.zclose_file = mem_close;
    filefunc.zerror_file = mem_error;
    filefunc.opaque = &memBuf;

    unzFile uf = unzOpen2("mem", &filefunc);
    if (!uf)
    {
        std::printf("[OptiCraftMods] Failed unzOpen2 for '%s'\n", originalPath.c_str());
        return false;
    }

    int res = unzLocateFile(uf, "mod.info", 2);
    if (res != UNZ_OK)
        res = unzLocateFile(uf, "manifest.txt", 2);

    if (res != UNZ_OK)
    {
        std::printf("[OptiCraftMods] mod.info not found inside '%s'\n", originalPath.c_str());
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
    std::printf("[OptiCraftMods] Loaded: id='%s' name='%s' ver='%s' (%s)\n",
                outInfo.id.c_str(), outInfo.name.c_str(), outInfo.version.c_str(), originalPath.c_str());
    return true;
}

bool readInfo(const std::string &filePath, OchPackInfo &outInfo)
{
    std::vector<unsigned char> data;
    std::string resolved;
    if (!readFileBytes(filePath, data, &resolved))
        return false;
    return readInfoFromBytes(data, resolved, outInfo);
}

std::vector<OchPackInfo> scanDirectory(const std::string &dirPath, std::vector<std::string> *outDebugLogs)
{
    std::vector<OchPackInfo> results;
    std::vector<std::string> candidateFiles;

    if (outDebugLogs)
        outDebugLogs->push_back("Scanning: " + dirPath);

    // 1. Try reading packlist.txt or mods.list in this directory
    static const char *const LIST_NAMES[] = {
        "packlist.txt",
        "mods.list",
        "mods.txt"
    };

    for (const char *listName : LIST_NAMES)
    {
        std::string listPath = PlatformStorage::join(dirPath, listName);
        std::vector<unsigned char> listData;
        if (readFileBytes(listPath, listData) && !listData.empty())
        {
            if (outDebugLogs)
                outDebugLogs->push_back("Found " + std::string(listName) + " in " + dirPath);
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
    static const char *const PROBE_NAMES[] = {
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

    for (const char *probeName : PROBE_NAMES)
    {
        bool alreadyCandidate = false;
        for (const auto &c : candidateFiles)
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
                    for (const auto &c : candidateFiles)
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
        std::vector<unsigned char> fileData;
        std::string resolved;
        if (readFileBytes(fullPath, fileData, &resolved))
        {
            OchPackInfo info;
            if (readInfoFromBytes(fileData, resolved, info))
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
                {
                    results.push_back(info);
                    if (outDebugLogs)
                        outDebugLogs->push_back("Found mod: " + info.name + " (" + info.version + ")");
                }
            }
        }
    }

    return results;
}
}

#include "sdmc.hpp"

#include <cstring>

namespace sdmc {

    namespace {

        FsFileSystem sdmc;
        char path_buffer[FS_MAX_PATH];

    }

    Result Open() {
        return fsOpenSdCardFileSystem(&sdmc);
    }

    void Close() {
        fsFsClose(&sdmc);
    }

    Result OpenFile(FsFile *file, const char *path, int open_mode) {
        std::strcpy(path_buffer, path);
        return fsFsOpenFile(&sdmc, path_buffer, open_mode, file);
    }

    Result OpenDir(FsDir *dir, const char *path, int open_mode) {
        std::strcpy(path_buffer, path);
        return fsFsOpenDirectory(&sdmc, path_buffer, open_mode, dir);
    }

    Result GetType(const char* path, FsDirEntryType* type) {
        std::strcpy(path_buffer, path);
        return fsFsGetEntryType(&sdmc, path_buffer, type);;
    }

    bool FileExists(const char* path) {
        FsDirEntryType type;
        return R_SUCCEEDED(GetType(path, &type)) && type == FsDirEntryType_File;
    }

    Result CreateFolder(const char* path) {
        std::strcpy(path_buffer, path);
        return fsFsCreateDirectory(&sdmc, path_buffer);
    }

    Result WriteFile(const char* path, const void* data, size_t size) {
        std::strcpy(path_buffer, path);
        fsFsDeleteFile(&sdmc, path_buffer);   // ignore error if absent
        Result rc = fsFsCreateFile(&sdmc, path_buffer, (s64)size, 0);
        if (R_FAILED(rc)) return rc;
        FsFile file;
        rc = fsFsOpenFile(&sdmc, path_buffer, FsOpenMode_Write, &file);
        if (R_FAILED(rc)) return rc;
        rc = fsFileWrite(&file, 0, data, size, FsWriteOption_Flush);
        fsFileClose(&file);
        return rc;
    }

}

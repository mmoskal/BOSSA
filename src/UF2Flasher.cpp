///////////////////////////////////////////////////////////////////////////////
// BOSSA
//
// Copyright (c) 2011-2012, ShumaTech
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above copyright
//       notice, this list of conditions and the following disclaimer in the
//       documentation and/or other materials provided with the distribution.
//     * Neither the name of the <organization> nor the
//       names of its contributors may be used to endorse or promote products
//       derived from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
// DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
///////////////////////////////////////////////////////////////////////////////

#include <string>
#include <exception>
#include <vector>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#ifdef __WIN32__
#include <windows.h>
#endif

#include "Devices.h"

#define UF2_MAGIC_START0 0x0A324655UL // "UF2\n"
#define UF2_MAGIC_START1 0x9E5D5157UL // Randomly selected
#define UF2_MAGIC_END 0x0AB16F30UL    // Ditto

using namespace std;

class UF2Error : public std::exception
{
  public:
    UF2Error(const char *message) : exception(), _message(message){};
    const char *what() const throw() { return _message; }
  protected:
    const char *_message;
};

static vector<string>
listDirsAt(string path)
{
    vector<string> res;

#ifdef __WIN32__
    (void)path;

    uint32_t drives = GetLogicalDrives();
    for (int letter = 'A'; letter <= 'Z'; letter++) {
        int driveNo = letter - 'A';
        if (drives & (1 << driveNo)) {
            char buf[5] = "A:\\";
            buf[0] = letter;
            uint32_t type = GetDriveType(buf);
            if (type == 2) {
                buf[2] = 0;
                res.push_back(buf);
            }
        }
    }
#else
    DIR *d = opendir(path.c_str());
    struct dirent *entry;

    while ((entry = readdir(d)))
    {
        if (entry->d_type == DT_DIR)
            res.push_back(path + "/" + entry->d_name);
    }
#endif

    return res;
}

#define BLK_SIZE 512
#define BLK_PAYLOAD 256

static vector<string>
getDrives(bool info = false)
{
    vector<string> dirs;
    vector<string> res;

#if defined(__APPLE__)
    dirs = listDirsAt("/Volumes");
#elif defined(__WIN32__)
    dirs = listDirsAt("/");
#else
    dirs = listDirsAt(string("/media/") + getenv("USER"));
    if (dirs.size() == 0)
        dirs = listDirsAt(std::string("/media"));
#endif

    char buffer[BLK_SIZE];
    for (uint32_t i = 0; i < dirs.size(); ++i)
    {
        string filename = dirs[i] + "/INFO_UF2.TXT";
        FILE *f = fopen(filename.c_str(), "rt");
        if (f)
        {
            memset(buffer, 0, BLK_SIZE);
            fread(buffer, 1, BLK_SIZE - 1, f);
            fclose(f);

            if (info)
            {
                printf("\n*** UF2 drive at %s, info:\n%s\n", dirs[i].c_str(), buffer);
            }
            else
            {
                char *ptr = strstr(buffer, "Board-ID: ");
                if (ptr)
                {
                    ptr += 10;
                    char *end = ptr;
                    while (*end && *end != '\r' && *end != '\n')
                        end++;
                    *end = 0;
                }
                printf("Found UF2 drive at %s (%s)\n", dirs[i].c_str(), ptr);
            }
            res.push_back(dirs[i]);
        }
    }

    return res;
}

static void writeLE(uint8_t *dst, uint32_t num)
{
    dst[0] = num & 0xff;
    dst[1] = (num >> 8) & 0xff;
    dst[2] = (num >> 16) & 0xff;
    dst[3] = (num >> 24) & 0xff;
}

static void flashBinFile(FILE *bin, uint32_t sz, uint32_t addr, string dest)
{
    FILE *fout = fopen(dest.c_str(), "wb");

    fseek(bin, 0L, SEEK_SET);

    uint8_t block[BLK_SIZE];
    memset(block, 0, BLK_SIZE);

    int currBlock = 0;
    int totalBlocks = (sz + BLK_PAYLOAD - 1) / BLK_PAYLOAD;
    while (fread(block + 32, 1, BLK_PAYLOAD, bin))
    {
        writeLE(block + 0, UF2_MAGIC_START0);
        writeLE(block + 4, UF2_MAGIC_START1);
        writeLE(block + 8, 0);            // flags
        writeLE(block + 12, addr);        // target address
        writeLE(block + 16, BLK_PAYLOAD); // payload size
        writeLE(block + 20, currBlock++); // number of blocks
        writeLE(block + 24, totalBlocks); // numBlocks
        writeLE(block + 28, 0);           // reserved
        writeLE(block + BLK_SIZE - 4, UF2_MAGIC_END);

        addr += BLK_PAYLOAD;

        fwrite(block, 1, BLK_SIZE, fout);

        // clear for next iteration, in case we get a short read
        memset(block, 0, BLK_SIZE);
    }
    fclose(fout);
    if (totalBlocks != currBlock)
        throw UF2Error("File size changed.");
    printf("Wrote %d blocks to %s\n", currBlock, dest.c_str());
}

void infoUF2()
{
    vector<string> drives = getDrives(true);
    if (drives.size() == 0)
        printf("No drives found.");
}

void writeUF2(const char *filename)
{
    vector<string> drives = getDrives();

    if (drives.size() == 0)
        throw UF2Error("No drives found.");

    FILE *bin = fopen(filename, "rb");

    if (!bin)
        throw UF2Error("BIN file not found.");

    fseek(bin, 0L, SEEK_END);
    uint32_t sz = ftell(bin);

    for (uint32_t i = 0; i < drives.size(); ++i)
    {
        flashBinFile(bin, sz, ATSAMD_BOOTLOADER_SIZE, drives[i] + "/NEW.UF2");
    }

    fclose(bin);
}

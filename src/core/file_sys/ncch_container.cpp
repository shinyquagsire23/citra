// Copyright 2017 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <cinttypes>
#include <cstring>
#include <memory>
#include "common/common_types.h"
#include "common/logging/log.h"
#include "core/core.h"
#include "core/file_sys/ncch_container.h"
#include "core/loader/loader.h"

////////////////////////////////////////////////////////////////////////////////////////////////////
// FileSys namespace

namespace FileSys {

static const int kMaxSections = 8;   ///< Maximum number of sections (files) in an ExeFs
static const int kBlockSize = 0x200; ///< Size of ExeFS blocks (in bytes)
static const u64_le updateMask = 0x0000000e00000000;

/**
 * Get the decompressed size of an LZSS compressed ExeFS file
 * @param buffer Buffer of compressed file
 * @param size Size of compressed buffer
 * @return Size of decompressed buffer
 */
static u32 LZSS_GetDecompressedSize(const u8* buffer, u32 size) {
    u32 offset_size = *(u32*)(buffer + size - 4);
    return offset_size + size;
}

/**
 * Decompress ExeFS file (compressed with LZSS)
 * @param compressed Compressed buffer
 * @param compressed_size Size of compressed buffer
 * @param decompressed Decompressed buffer
 * @param decompressed_size Size of decompressed buffer
 * @return True on success, otherwise false
 */
static bool LZSS_Decompress(const u8* compressed, u32 compressed_size, u8* decompressed,
                            u32 decompressed_size) {
    const u8* footer = compressed + compressed_size - 8;
    u32 buffer_top_and_bottom = *reinterpret_cast<const u32*>(footer);
    u32 out = decompressed_size;
    u32 index = compressed_size - ((buffer_top_and_bottom >> 24) & 0xFF);
    u32 stop_index = compressed_size - (buffer_top_and_bottom & 0xFFFFFF);

    memset(decompressed, 0, decompressed_size);
    memcpy(decompressed, compressed, compressed_size);

    while (index > stop_index) {
        u8 control = compressed[--index];

        for (unsigned i = 0; i < 8; i++) {
            if (index <= stop_index)
                break;
            if (index <= 0)
                break;
            if (out <= 0)
                break;

            if (control & 0x80) {
                // Check if compression is out of bounds
                if (index < 2)
                    return false;
                index -= 2;

                u32 segment_offset = compressed[index] | (compressed[index + 1] << 8);
                u32 segment_size = ((segment_offset >> 12) & 15) + 3;
                segment_offset &= 0x0FFF;
                segment_offset += 2;

                // Check if compression is out of bounds
                if (out < segment_size)
                    return false;

                for (unsigned j = 0; j < segment_size; j++) {
                    // Check if compression is out of bounds
                    if (out + segment_offset >= decompressed_size)
                        return false;

                    u8 data = decompressed[out + segment_offset];
                    decompressed[--out] = data;
                }
            } else {
                // Check if compression is out of bounds
                if (out < 1)
                    return false;
                decompressed[--out] = compressed[--index];
            }
            control <<= 1;
        }
    }
    return true;
}

NCCHContainer::NCCHContainer(std::string filepath) : filepath(filepath) {
    file = FileUtil::IOFile(filepath, "rb");
}

Loader::ResultStatus NCCHContainer::OpenFile(std::string filepath) {
    this->filepath = filepath;
    file = FileUtil::IOFile(filepath, "rb");

    LOG_DEBUG(Service_FS, "Opening %s", filepath.c_str());
}

Loader::ResultStatus NCCHContainer::Load() {
    if (is_loaded)
        return Loader::ResultStatus::Success;

    // Reset read pointer in case this file has been read before.
    file.Seek(0, SEEK_SET);

    if (file.ReadBytes(&ncch_header, sizeof(NCCH_Header)) != sizeof(NCCH_Header))
        return Loader::ResultStatus::Error;

    // Skip NCSD header and load first NCCH (NCSD is just a container of NCCH files)...
    if (Loader::MakeMagic('N', 'C', 'S', 'D') == ncch_header.magic) {
        LOG_DEBUG(Service_FS, "Only loading the first (bootable) NCCH within the NCSD file!");
        ncch_offset = 0x4000;
        file.Seek(ncch_offset, SEEK_SET);
        file.ReadBytes(&ncch_header, sizeof(NCCH_Header));
    }

    // Verify we are loading the correct file type...
    if (Loader::MakeMagic('N', 'C', 'C', 'H') != ncch_header.magic)
        return Loader::ResultStatus::ErrorInvalidFormat;

    // Read ExHeader...

    if (file.ReadBytes(&exheader_header, sizeof(ExHeader_Header)) != sizeof(ExHeader_Header))
        return Loader::ResultStatus::Error;

    is_compressed = (exheader_header.codeset_info.flags.flag & 1) == 1;
    u32 entry_point = exheader_header.codeset_info.text.address;
    u32 code_size = exheader_header.codeset_info.text.code_size;
    u32 stack_size = exheader_header.codeset_info.stack_size;
    u32 bss_size = exheader_header.codeset_info.bss_size;
    u32 core_version = exheader_header.arm11_system_local_caps.core_version;
    u8 priority = exheader_header.arm11_system_local_caps.priority;
    u8 resource_limit_category = exheader_header.arm11_system_local_caps.resource_limit_category;

    LOG_DEBUG(Service_FS, "Name:                        %s", exheader_header.codeset_info.name);
    LOG_DEBUG(Service_FS, "Program ID:                  %016" PRIX64, ncch_header.program_id);
    LOG_DEBUG(Service_FS, "Code compressed:             %s", is_compressed ? "yes" : "no");
    LOG_DEBUG(Service_FS, "Entry point:                 0x%08X", entry_point);
    LOG_DEBUG(Service_FS, "Code size:                   0x%08X", code_size);
    LOG_DEBUG(Service_FS, "Stack size:                  0x%08X", stack_size);
    LOG_DEBUG(Service_FS, "Bss size:                    0x%08X", bss_size);
    LOG_DEBUG(Service_FS, "Core version:                %d", core_version);
    LOG_DEBUG(Service_FS, "Thread priority:             0x%X", priority);
    LOG_DEBUG(Service_FS, "Resource limit category:     %d", resource_limit_category);
    LOG_DEBUG(Service_FS, "System Mode:                 %d",
              static_cast<int>(exheader_header.arm11_system_local_caps.system_mode));

    if (exheader_header.arm11_system_local_caps.program_id &
        ~updateMask != ncch_header.program_id) {
        LOG_ERROR(Service_FS, "ExHeader Program ID mismatch: the ROM is probably encrypted.");
        return Loader::ResultStatus::ErrorEncrypted;
    }

    // Read ExeFS...

    exefs_offset = ncch_header.exefs_offset * kBlockSize;
    u32 exefs_size = ncch_header.exefs_size * kBlockSize;

    LOG_DEBUG(Service_FS, "ExeFS offset:                0x%08X", exefs_offset);
    LOG_DEBUG(Service_FS, "ExeFS size:                  0x%08X", exefs_size);

    file.Seek(exefs_offset + ncch_offset, SEEK_SET);
    if (file.ReadBytes(&exefs_header, sizeof(ExeFs_Header)) != sizeof(ExeFs_Header))
        return Loader::ResultStatus::Error;

    is_loaded = true;
    return Loader::ResultStatus::Success;
}

Loader::ResultStatus NCCHContainer::LoadSectionExeFS(const char* name, std::vector<u8>& buffer) {
    if (!file.IsOpen())
        return Loader::ResultStatus::Error;

    Loader::ResultStatus result = Load();
    if (result != Loader::ResultStatus::Success)
        return result;

    LOG_DEBUG(Service_FS, "%d sections:", kMaxSections);
    // Iterate through the ExeFs archive until we find a section with the specified name...
    for (unsigned section_number = 0; section_number < kMaxSections; section_number++) {
        const auto& section = exefs_header.section[section_number];

        // Load the specified section...
        if (strcmp(section.name, name) == 0) {
            LOG_DEBUG(Service_FS, "%d - offset: 0x%08X, size: 0x%08X, name: %s", section_number,
                      section.offset, section.size, section.name);

            s64 section_offset =
                (section.offset + exefs_offset + sizeof(ExeFs_Header) + ncch_offset);
            file.Seek(section_offset, SEEK_SET);

            if (strcmp(section.name, ".code") == 0 && is_compressed) {
                // Section is compressed, read compressed .code section...
                std::unique_ptr<u8[]> temp_buffer;
                try {
                    temp_buffer.reset(new u8[section.size]);
                } catch (std::bad_alloc&) {
                    return Loader::ResultStatus::ErrorMemoryAllocationFailed;
                }

                if (file.ReadBytes(&temp_buffer[0], section.size) != section.size)
                    return Loader::ResultStatus::Error;

                // Decompress .code section...
                u32 decompressed_size = LZSS_GetDecompressedSize(&temp_buffer[0], section.size);
                buffer.resize(decompressed_size);
                if (!LZSS_Decompress(&temp_buffer[0], section.size, &buffer[0], decompressed_size))
                    return Loader::ResultStatus::ErrorInvalidFormat;
            } else {
                // Section is uncompressed...
                buffer.resize(section.size);
                if (file.ReadBytes(&buffer[0], section.size) != section.size)
                    return Loader::ResultStatus::Error;
            }
            return Loader::ResultStatus::Success;
        }
    }
    return Loader::ResultStatus::ErrorNotUsed;
}

Loader::ResultStatus NCCHContainer::ReadRomFS(std::shared_ptr<FileUtil::IOFile>& romfs_file,
                                              u64& offset, u64& size) {
    if (!file.IsOpen())
        return Loader::ResultStatus::Error;

    // Check if the NCCH has a RomFS...
    if (ncch_header.romfs_offset != 0 && ncch_header.romfs_size != 0) {
        u32 romfs_offset = ncch_offset + (ncch_header.romfs_offset * kBlockSize) + 0x1000;
        u32 romfs_size = (ncch_header.romfs_size * kBlockSize) - 0x1000;

        LOG_DEBUG(Service_FS, "RomFS offset:           0x%08X", romfs_offset);
        LOG_DEBUG(Service_FS, "RomFS size:             0x%08X", romfs_size);

        if (file.GetSize() < romfs_offset + romfs_size)
            return Loader::ResultStatus::Error;

        // We reopen the file, to allow its position to be independent from file's
        romfs_file = std::make_shared<FileUtil::IOFile>(filepath, "rb");
        if (!romfs_file->IsOpen())
            return Loader::ResultStatus::Error;

        offset = romfs_offset;
        size = romfs_size;

        return Loader::ResultStatus::Success;
    }
    LOG_DEBUG(Service_FS, "NCCH has no RomFS");
    return Loader::ResultStatus::ErrorNotUsed;
}

Loader::ResultStatus NCCHContainer::ReadProgramId(u64_le& program_id) {
    Loader::ResultStatus result = Load();
    if (result != Loader::ResultStatus::Success)
        return result;

    program_id = ncch_header.program_id;
    return Loader::ResultStatus::Success;
}

} // namespace FileSys

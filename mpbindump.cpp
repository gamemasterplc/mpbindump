#include <stdio.h>
#include <direct.h>
#include <string>
#include "external/zlib/zlib.h"
#include "external/tinyxml2/tinyxml2.h"

void PrintUsage(char *prog_name)
{
    printf("Usage: %s bin_file [xml_path]", prog_name);
}

void PrintError(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    exit(1);
    va_end(args);
}

uint8_t ReadU8(FILE *file)
{
    uint8_t temp;
    fread(&temp, 1, 1, file);
    return temp;
}

uint32_t ReadU32(FILE *file)
{
    uint8_t temp[4];
    fread(temp, 1, 4, file);
    return (temp[0] << 24) | (temp[1] << 16) | (temp[2] << 8) | temp[3];
}

void SetSeek(FILE *file, size_t ofs)
{
    fseek(file, ofs, SEEK_SET);
}

void DecodeLZSS(FILE *file, uint8_t *dst, uint32_t len)
{
    uint8_t window[1024];
    uint32_t window_ofs = 958;
    uint16_t flag = 0;
    memset(window, 0, 1024);
    while (len > 0) {
        flag >>= 1;
        if (!(flag & 0x100)) {
            flag = 0xFF00 | ReadU8(file);
        }
        if (flag & 0x1) {
            window[window_ofs++] = *dst++ = ReadU8(file);
            window_ofs %= 1024;
            len--;
        }
        else {
            uint32_t i;
            uint8_t byte1 = ReadU8(file);
            uint8_t byte2 = ReadU8(file);
            uint32_t ofs = ((byte2 & 0xC0) << 2) | byte1;
            uint32_t copy_len = (byte2 & 0x3F) + 3;
            for (i = 0; i < copy_len; i++) {
                window[window_ofs++] = *dst++ = window[(ofs + i) % 1024];
                window_ofs %= 1024;
            }
            len -= i;
        }
    }
}

void DecodeSlide(FILE *file, uint8_t *dst, uint32_t len)
{
    uint32_t temp_len = ReadU32(file);
    uint32_t num_bits = 0;
    uint32_t mask = 0;
    uint8_t *base_ptr = dst;
    while (len) {
        if (num_bits == 0) {
            mask = ReadU32(file);
            num_bits = 32;
        }
        if (mask & 0x80000000) {
            *dst++ = ReadU8(file);
            len--;
        } else {
            uint32_t copy_ofs = (ReadU8(file) << 8) + ReadU8(file);
            uint32_t copy_len = (copy_ofs & 0xF000) >> 12;
            uint8_t *lookback_ptr;
            copy_ofs &= 0xFFF;
            lookback_ptr = dst - copy_ofs;
            if (copy_len == 0) {
                copy_len = ReadU8(file) + 18;
            } else {
                copy_len += 2;
            }
            len -= copy_len;
            while (copy_len) {
                if (lookback_ptr - 1 < base_ptr) {
                    *dst++ = 0;
                } else {
                    *dst++ = *(lookback_ptr - 1);
                }
                copy_len--;
                lookback_ptr++;
            }
        }
        mask <<= 1;
        num_bits--;
    }
}

void DecodeRle(FILE *file, uint8_t *dst, uint32_t len)
{
    while (len) {
        uint8_t len_value = ReadU8(file);
        if (len_value < 128) {
            uint8_t value = ReadU8(file);
            for (uint8_t i = 0; i < len_value; i++) {
                *dst++ = value;
            }
        } else {
            len_value -= 128;
            for (uint8_t i = 0; i < len_value; i++) {
                *dst++ = ReadU8(file);
            }
        }
        len -= len_value;
    }
}

void DecodeZlib(FILE *file, uint8_t *dst, uint32_t len)
{
    z_stream stream = { 0 };
    uint8_t *pack_buf;
    uint32_t raw_size = ReadU32(file);
    uint32_t pack_size = ReadU32(file);
    pack_buf = new uint8_t[pack_size];
    fread(pack_buf, 1, pack_size, file);
    stream.total_in = stream.avail_in = pack_size;
    stream.total_out = stream.avail_out = raw_size;
    stream.next_in = (Bytef *)pack_buf;
    stream.next_out = (Bytef *)dst;
    stream.zalloc = Z_NULL;
    stream.zfree = Z_NULL;
    stream.opaque = Z_NULL;
    int error = -1;
    error = inflateInit2(&stream, (15 + 32)); //15 window bits, and the +32 tells zlib to to detect if using gzip or zlib
    if (error == Z_OK)
    {
        error = inflate(&stream, Z_FINISH);
        inflateEnd(&stream);
    }
    else
    {
        inflateEnd(&stream);
    }
	delete[] pack_buf;
}

std::string GetBufExtension(uint8_t *buf)
{
    uint8_t atb_marker[4] = { 0, 0, 0, 0x14 };
    if (!memcmp(buf, "HSFV037", 7)) {
        return "hsf";
    }
    if (!memcmp(buf, "ANIM", 4)|| !memcmp(buf+12, atb_marker, 4)) {
        return "anm";
    }
    return "dat";
}

int main(int argc, char **argv)
{
    if (argc != 2 && argc != 3) {
        PrintUsage(argv[0]);
        return 1;
    }
    FILE *bin_file = fopen(argv[1], "rb");
    if (!bin_file) {
        PrintError("Failed to open %s for reading.", argv[1]);
    }
    uint32_t file_count = ReadU32(bin_file);
    uint32_t *file_offsets = new uint32_t[file_count];
    for (uint32_t i = 0; i < file_count; i++) {
        file_offsets[i] = ReadU32(bin_file);
    }
    std::string out_dir = argv[1];
    if (out_dir.find_last_of("\\/") != std::string::npos) {
        out_dir = out_dir.substr(0, out_dir.find_last_of("\\/"));
    } else {
        out_dir = "";
    }
    std::string out_name = "";
    if (argc == 3) {
        out_name = argv[2];
        if (out_name.find_last_of(".") != std::string::npos) {
            size_t length = out_name.find_last_of(".");
            std::string temp(out_name.c_str(), length);
            out_name = temp;
        }
    } else {
        out_name = argv[1];
        if (out_name.find_last_of("\\/") != std::string::npos && out_name.find_last_of(".") != std::string::npos) {
            size_t start = out_name.find_last_of("\\/") + 1;
            size_t end = out_name.find_last_of(".");
            std::string temp(out_name.c_str() + start, end - start);
            out_name = temp;
        } else if (out_name.find_last_of(".") != std::string::npos) {
            size_t length = out_name.find_last_of(".");
            std::string temp(out_name.c_str(), length);
            out_name = temp;
        }
    }
    std::string create_dir = out_dir + "\\" + out_name + "\\";
    if (mkdir(create_dir.c_str()) == ENOENT) {
        PrintError("Failed to create directory %s.\n", create_dir.c_str());
    }
    tinyxml2::XMLDocument document;
    tinyxml2::XMLNode *root = document.NewElement("files");
    document.InsertFirstChild(root);
    for (uint32_t i = 0; i < file_count; i++) {
        std::string comp_type_str;
        tinyxml2::XMLElement *file_element = document.NewElement("file");
        SetSeek(bin_file, file_offsets[i]);
        uint32_t raw_size, comp_type;
        raw_size = ReadU32(bin_file);
        comp_type = ReadU32(bin_file);
        uint8_t *decomp_buf = new uint8_t[raw_size];
        switch (comp_type) {
            case 1:
                comp_type_str = "lzss";
                DecodeLZSS(bin_file, decomp_buf, raw_size);
                break;

            case 2:
            case 3:
            case 4:
                comp_type_str = "slide";
                DecodeSlide(bin_file, decomp_buf, raw_size);
                break;

            case 5:
                comp_type_str = "rle";
                DecodeRle(bin_file, decomp_buf, raw_size);
                break;

            case 7:
                comp_type_str = "zlib";
                DecodeZlib(bin_file, decomp_buf, raw_size);
                break;

            default:
                comp_type_str = "none";
                fread(decomp_buf, 1, raw_size, bin_file);
                break;
        }
        std::string buf_extension = GetBufExtension(decomp_buf);
        std::string file_id = "file" + std::to_string(i) + "_" + buf_extension;
        file_element->SetAttribute("id", file_id.c_str());
        file_element->SetAttribute("compression_type", comp_type_str.c_str());
        std::string xml_path = out_name + "\\" + "file" + std::to_string(i) + "." + buf_extension;
        file_element->SetAttribute("path", xml_path.c_str());
        std::string new_file_name = create_dir+"file"+std::to_string(i) + "." + buf_extension;
        FILE *new_file = fopen(new_file_name.c_str(), "wb");
        if (!new_file) {
            PrintError("Failed to create file %s.\n", new_file_name.c_str());
        }
        fwrite(decomp_buf, 1, raw_size, new_file);
        root->InsertEndChild(file_element);
        fclose(new_file);
        delete[] decomp_buf;
    }
    std::string out_xml = out_dir + "\\" + out_name + ".xml";
    document.SaveFile(out_xml.c_str());
    delete[] file_offsets;
    fclose(bin_file);
    return 0;
}
/*
 * Amiga Fuse - FIXED VERSION WITH WRITE SUPPORT for macOS
 * Implements full read/write support for Amiga ADF disk images
 */

#define FUSE_USE_VERSION 26

#ifdef __APPLE__
#define typeof __typeof__
#endif

#include <fuse.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <format>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>
#include <set>

namespace amiga_fuse {

// Constants
constexpr size_t BLOCK_SIZE = 512;
constexpr size_t BCPL_STRING_MAX = 30;
constexpr size_t HASH_TABLE_SIZE = 72;
constexpr size_t MAX_BLOCKS = 1760;  // Standard DD disk

// Block types
constexpr int32_t T_HEADER = 2;
constexpr int32_t T_DATA = 8;
constexpr int32_t T_LIST = 16;
constexpr int32_t T_DIRC = 33;
constexpr int32_t T_SHORT = -3;
constexpr int32_t T_LONG = -4;
constexpr int32_t ST_ROOT = 1;
constexpr int32_t ST_DIR = 2;
constexpr int32_t ST_FILE = -3;

// DOS types
constexpr uint32_t DOS_OFS = 0x444F5300;
constexpr uint32_t DOS_FFS = 0x444F5301;
constexpr uint32_t DOS_FFS_INTL = 0x444F5303;
constexpr uint32_t DOS_FFS_DC = 0x444F5305;

// Endian helpers
namespace endian {
    template<typename T>
    constexpr T byteswap(T value) noexcept {
        if constexpr (sizeof(T) == 1) {
            return value;
        } else if constexpr (sizeof(T) == 2) {
            return static_cast<T>(__builtin_bswap16(static_cast<std::uint16_t>(value)));
        } else if constexpr (sizeof(T) == 4) {
            return static_cast<T>(__builtin_bswap32(static_cast<std::uint32_t>(value)));
        } else if constexpr (sizeof(T) == 8) {
            return static_cast<T>(__builtin_bswap64(static_cast<std::uint64_t>(value)));
        }
    }

    template<typename T>
    constexpr T from_big_endian(T value) noexcept {
        if constexpr (std::endian::native == std::endian::big) {
            return value;
        } else {
            return byteswap(value);
        }
    }
    
    template<typename T>
    constexpr T to_big_endian(T value) noexcept {
        return from_big_endian(value); // Same operation for both directions
    }
}

// BCPL string handling
class BcplString {
public:
    static std::string read(const uint8_t* data, size_t max_len = BCPL_STRING_MAX) {
        if (!data || data[0] == 0) return "";
        size_t len = std::min(static_cast<size_t>(data[0]), max_len);
        return std::string(reinterpret_cast<const char*>(data + 1), len);
    }
    
    static void write(uint8_t* data, const std::string& str, size_t max_len = BCPL_STRING_MAX) {
        size_t len = std::min(str.length(), max_len);
        data[0] = static_cast<uint8_t>(len);
        if (len > 0) {
            std::memcpy(data + 1, str.data(), len);
        }
        // Clear remaining bytes
        if (len < max_len) {
            std::memset(data + 1 + len, 0, max_len - len);
        }
    }
};

// Block structures
#pragma pack(push, 1)
struct BootBlock {
    uint32_t disk_type;
    uint32_t checksum;
    uint32_t root_block;
    uint8_t boot_code[1012];
};

struct RootBlock {
    uint32_t type;
    uint32_t header_key;
    uint32_t high_seq;
    uint32_t hash_table_size;
    uint32_t first_size;
    uint32_t checksum;
    uint32_t hash_table[HASH_TABLE_SIZE];
    uint32_t bm_flag;
    uint32_t bm_pages[25];
    uint32_t bm_ext;
    uint32_t days;
    uint32_t mins;
    uint32_t ticks;
    uint8_t name[32];
    uint8_t comment[80];
    uint32_t days2;
    uint32_t mins2;
    uint32_t ticks2;
    uint32_t created_days;
    uint32_t created_mins;
    uint32_t created_ticks;
    uint32_t next_hash;
    uint32_t parent;
    uint32_t extension;
    int32_t sec_type;
};

struct FileBlock {
    uint32_t type;                    // 0
    uint32_t header_key;              // 4  
    uint32_t high_seq;                // 8
    uint32_t data_size;               // 12
    uint32_t first_data;              // 16
    uint32_t checksum;                // 20
    uint32_t data_blocks[HASH_TABLE_SIZE]; // 24 (288 bytes)
    uint8_t padding1[12];             // 312 to 324
    uint32_t file_size;               // 324
    uint8_t comment[80];              // 328
    uint32_t days;                    // 408
    uint32_t mins;                    // 412
    uint32_t ticks;                   // 416
    uint8_t padding2[12];             // 420 to 432
    uint8_t filename[32];             // 432
    uint8_t padding3[32];             // 464 to 496
    uint32_t hash_chain;              // 496
    uint32_t parent;                  // 500
    uint32_t extension;               // 504
    int32_t sec_type;                 // 508
};

struct DataBlock {
    uint32_t type;
    uint32_t header_key;
    uint32_t seq_num;
    uint32_t data_size;
    uint32_t next_data;
    uint32_t checksum;
    uint8_t data[488];
};

struct BitmapBlock {
    uint32_t checksum;
    uint32_t map[127];  // Each bit represents a block
};

struct BitmapExtBlock {
    uint32_t bitmap_flag;
    uint32_t next_bitmap;
    uint32_t bitmap_blocks[127];
};
#pragma pack(pop)

// Directory entry
struct Entry {
    std::string name;
    bool is_directory;
    size_t size;
    time_t mtime;
    uint32_t block_num;
};

// Main ADF image handler with write support
class AdfImage {
private:
    std::string filename_;
    int fd_ = -1;
    void* mapped_data_ = nullptr;
    size_t file_size_ = 0;
    
    uint32_t dos_type_ = 0;
    uint32_t root_block_num_ = 0;
    std::string volume_name_;
    bool is_ffs_ = false;
    bool read_only_ = false;
    
    std::unordered_map<std::string, std::vector<Entry>> dir_cache_;
    std::set<uint32_t> free_blocks_;
    std::set<uint32_t> used_blocks_;
    
public:
    explicit AdfImage(std::string_view filename) : filename_(filename) {}
    
    ~AdfImage() {
        close();
    }
    
    bool open(bool write_mode = true) {
        // Try to open with write access first
        fd_ = ::open(filename_.c_str(), write_mode ? O_RDWR : O_RDONLY);
        if (fd_ == -1) {
            // Fall back to read-only if write fails
            fd_ = ::open(filename_.c_str(), O_RDONLY);
            if (fd_ == -1) return false;
            read_only_ = true;
        }
        
        struct stat st;
        if (fstat(fd_, &st) == -1) {
            ::close(fd_);
            fd_ = -1;
            return false;
        }
        
        file_size_ = static_cast<size_t>(st.st_size);
        
        // Map with appropriate protection
        int prot = PROT_READ;
        if (!read_only_) prot |= PROT_WRITE;
        
        mapped_data_ = mmap(nullptr, file_size_, prot, MAP_SHARED, fd_, 0);
        if (mapped_data_ == MAP_FAILED) {
            ::close(fd_);
            fd_ = -1;
            mapped_data_ = nullptr;
            return false;
        }
        
        return parse_filesystem();
    }
    
    void close() {
        if (mapped_data_ && mapped_data_ != MAP_FAILED) {
            // Sync changes to disk if writeable
            if (!read_only_) {
                msync(mapped_data_, file_size_, MS_SYNC);
            }
            munmap(mapped_data_, file_size_);
            mapped_data_ = nullptr;
        }
        if (fd_ != -1) {
            ::close(fd_);
            fd_ = -1;
        }
    }
    
    bool is_valid() const {
        return fd_ != -1 && mapped_data_ && file_size_ >= BLOCK_SIZE * 2;
    }
    
    bool is_read_only() const {
        return read_only_;
    }
    
    template<typename T>
    const T* get_block(uint32_t block_num) const {
        if (!is_valid() || block_num * BLOCK_SIZE + sizeof(T) > file_size_) {
            return nullptr;
        }
        return reinterpret_cast<const T*>(
            static_cast<const uint8_t*>(mapped_data_) + block_num * BLOCK_SIZE
        );
    }
    
    template<typename T>
    T* get_block_writable(uint32_t block_num) {
        if (!is_valid() || read_only_ || block_num * BLOCK_SIZE + sizeof(T) > file_size_) {
            return nullptr;
        }
        return reinterpret_cast<T*>(
            static_cast<uint8_t*>(mapped_data_) + block_num * BLOCK_SIZE
        );
    }
    
    uint32_t calculate_checksum(const void* block) {
        const uint32_t* data = static_cast<const uint32_t*>(block);
        uint32_t sum = 0;
        for (int i = 0; i < 128; i++) {
            if (i != 5) { // Skip checksum field itself
                sum += endian::from_big_endian(data[i]);
            }
        }
        return -sum;
    }
    
    void update_checksum(void* block) {
        uint32_t* data = static_cast<uint32_t*>(block);
        data[5] = 0; // Clear checksum field
        uint32_t checksum = calculate_checksum(block);
        data[5] = endian::to_big_endian(checksum);
    }
    
    bool parse_filesystem() {
        const auto* boot = get_block<BootBlock>(0);
        if (!boot) return false;
        
        dos_type_ = endian::from_big_endian(boot->disk_type);
        root_block_num_ = endian::from_big_endian(boot->root_block);
        
        is_ffs_ = (dos_type_ == DOS_FFS || dos_type_ == DOS_FFS_INTL || dos_type_ == DOS_FFS_DC);
        
        if ((dos_type_ & 0xFFFFFF00) != 0x444F5300) {
            root_block_num_ = 880;
        }
        
        const auto* root = get_block<RootBlock>(root_block_num_);
        if (!root) return false;
        
        uint32_t root_type = endian::from_big_endian(root->type);
        int32_t root_sec_type = endian::from_big_endian(root->sec_type);
        
        // Be more lenient - some ADFs have sec_type as 0 instead of 1
        if (root_type != T_HEADER) {
            return false;
        }
        
        // Accept both 0 and ST_ROOT for sec_type
        if (root_sec_type != ST_ROOT && root_sec_type != 0) {
            return false;
        }
        
        volume_name_ = BcplString::read(root->name);
        
        // Parse bitmap to find free blocks
        parse_bitmap();
        
        return true;
    }
    
    void parse_bitmap() {
        free_blocks_.clear();
        used_blocks_.clear();
        
        // Initially mark all blocks as free
        uint32_t total_blocks = file_size_ / BLOCK_SIZE;
        for (uint32_t i = 2; i < total_blocks; i++) {
            free_blocks_.insert(i);
        }
        
        // Mark system blocks as used
        used_blocks_.insert(0); // Boot block
        used_blocks_.insert(1); // Boot block
        used_blocks_.insert(root_block_num_);
        free_blocks_.erase(0);
        free_blocks_.erase(1);
        free_blocks_.erase(root_block_num_);
        
        // Parse the actual bitmap
        const auto* root = get_block<RootBlock>(root_block_num_);
        if (!root) return;
        
        for (int i = 0; i < 25; i++) {
            uint32_t bm_block = endian::from_big_endian(root->bm_pages[i]);
            if (bm_block == 0) break;
            
            used_blocks_.insert(bm_block);
            free_blocks_.erase(bm_block);
            
            const auto* bitmap = get_block<BitmapBlock>(bm_block);
            if (!bitmap) continue;
            
            // Each bitmap block covers 127 * 32 = 4064 blocks
            uint32_t base_block = i * 4064;
            
            for (int j = 0; j < 127; j++) {
                uint32_t map_word = endian::from_big_endian(bitmap->map[j]);
                for (int bit = 0; bit < 32; bit++) {
                    uint32_t block_num = base_block + j * 32 + bit;
                    if (block_num >= total_blocks) break;
                    
                    if (!(map_word & (1 << bit))) {
                        // Bit clear = block used
                        used_blocks_.insert(block_num);
                        free_blocks_.erase(block_num);
                    }
                }
            }
        }
        
        // Also mark blocks used by directory structure
        scan_used_blocks(root_block_num_);
    }
    
    void scan_used_blocks(uint32_t block_num) {
        if (block_num == 0 || used_blocks_.count(block_num)) return;
        
        used_blocks_.insert(block_num);
        free_blocks_.erase(block_num);
        
        const auto* block = get_block<FileBlock>(block_num);
        if (!block) return;
        
        int32_t sec_type = endian::from_big_endian(block->sec_type);
        
        // Scan hash table for directories (also accept 0 for root)
        if (sec_type == ST_ROOT || sec_type == 0 || sec_type == ST_DIR) {
            // For root block, use RootBlock structure
            if (block_num == root_block_num_) {
                const auto* root = get_block<RootBlock>(block_num);
                if (root) {
                    for (int i = 0; i < HASH_TABLE_SIZE; i++) {
                        uint32_t hash_block = endian::from_big_endian(root->hash_table[i]);
                        if (hash_block != 0) {
                            scan_used_blocks(hash_block);
                        }
                    }
                }
            } else {
                // For directory blocks, scan the data_blocks array (used as hash table)
                for (int i = 0; i < HASH_TABLE_SIZE; i++) {
                    uint32_t hash_block = endian::from_big_endian(block->data_blocks[i]);
                    if (hash_block != 0) {
                        scan_used_blocks(hash_block);
                    }
                }
            }
        }
        
        // Scan data blocks for files
        if (sec_type == ST_FILE) {
            uint32_t data_block = endian::from_big_endian(block->first_data);
            while (data_block != 0) {
                used_blocks_.insert(data_block);
                free_blocks_.erase(data_block);
                
                const auto* data = get_block<DataBlock>(data_block);
                if (!data) break;
                data_block = endian::from_big_endian(data->next_data);
            }
        }
        
        // Scan chain
        uint32_t next = endian::from_big_endian(block->hash_chain);
        if (next != 0) {
            scan_used_blocks(next);
        }
    }
    
    uint32_t allocate_block() {
        if (free_blocks_.empty()) return 0;
        
        uint32_t block = *free_blocks_.begin();
        free_blocks_.erase(block);
        used_blocks_.insert(block);
        
        // Update bitmap
        update_bitmap_for_block(block, false); // false = mark as used
        
        // Clear the block
        void* data = get_block_writable<uint8_t>(block);
        if (data) {
            std::memset(data, 0, BLOCK_SIZE);
        }
        
        return block;
    }
    
    void free_block(uint32_t block) {
        if (block < 2 || block == root_block_num_) return; // Don't free system blocks
        
        used_blocks_.erase(block);
        free_blocks_.insert(block);
        
        // Update bitmap
        update_bitmap_for_block(block, true); // true = mark as free
    }
    
    void update_bitmap_for_block(uint32_t block, bool is_free) {
        // Find which bitmap block this belongs to
        uint32_t bitmap_index = block / 4064;
        uint32_t bit_offset = block % 4064;
        uint32_t word_index = bit_offset / 32;
        uint32_t bit_index = bit_offset % 32;
        
        if (bitmap_index >= 25) return;
        
        const auto* root = get_block<RootBlock>(root_block_num_);
        if (!root) return;
        
        uint32_t bm_block = endian::from_big_endian(root->bm_pages[bitmap_index]);
        if (bm_block == 0) {
            // Need to allocate a new bitmap block - TODO
            return;
        }
        
        auto* bitmap = get_block_writable<BitmapBlock>(bm_block);
        if (!bitmap) return;
        
        uint32_t map_word = endian::from_big_endian(bitmap->map[word_index]);
        if (is_free) {
            map_word |= (1 << bit_index);  // Set bit = free
        } else {
            map_word &= ~(1 << bit_index); // Clear bit = used
        }
        bitmap->map[word_index] = endian::to_big_endian(map_word);
        
        // Update bitmap checksum
        update_checksum(bitmap);
    }
    
    uint32_t hash_name(const std::string& name) {
        uint32_t hash = static_cast<uint32_t>(name.length());
        for (char c : name) {
            hash = hash * 13 + static_cast<uint32_t>(std::toupper(c));
        }
        return hash % HASH_TABLE_SIZE;
    }
    
    std::optional<std::vector<Entry>> list_directory(const std::string& path) {
        if (auto cached = get_cached_dir(path)) {
            return cached;
        }
        
        uint32_t dir_block = find_directory_block(path);
        if (dir_block == 0) return std::nullopt;
        
        std::vector<Entry> entries;
        const auto* dir = get_block<FileBlock>(dir_block);
        if (!dir) return std::nullopt;
        
        // Scan hash table
        for (int i = 0; i < HASH_TABLE_SIZE; i++) {
            uint32_t block_num = endian::from_big_endian(
                (dir_block == root_block_num_) ?
                reinterpret_cast<const RootBlock*>(dir)->hash_table[i] :
                dir->data_blocks[i]
            );
            
            while (block_num != 0) {
                const auto* block = get_block<FileBlock>(block_num);
                if (!block) break;
                
                Entry entry;
                entry.name = BcplString::read(block->filename);
                if (entry.name.empty()) break;
                
                int32_t sec_type = endian::from_big_endian(block->sec_type);
                entry.is_directory = (sec_type == ST_DIR);
                entry.size = entry.is_directory ? 0 : endian::from_big_endian(block->file_size);
                entry.block_num = block_num;
                
                uint32_t days = endian::from_big_endian(block->days);
                uint32_t mins = endian::from_big_endian(block->mins);
                uint32_t ticks = endian::from_big_endian(block->ticks);
                entry.mtime = amiga_to_unix_time(days, mins, ticks);
                
                entries.push_back(entry);
                
                block_num = endian::from_big_endian(block->hash_chain);
            }
        }
        
        cache_directory(path, entries);
        return entries;
    }
    
    std::optional<Entry> get_entry(const std::string& path) {
        if (path == "/" || path.empty()) {
            Entry root;
            root.name = "";
            root.is_directory = true;
            root.size = 0;
            root.mtime = time(nullptr);
            root.block_num = root_block_num_;
            return root;
        }
        
        size_t last_slash = path.find_last_of('/');
        std::string parent_path = (last_slash == 0) ? "/" : path.substr(0, last_slash);
        std::string entry_name = path.substr(last_slash + 1);
        
        auto entries = list_directory(parent_path);
        if (!entries) return std::nullopt;
        
        for (const auto& entry : *entries) {
            if (entry.name == entry_name) {
                return entry;
            }
        }
        
        return std::nullopt;
    }
    
    std::vector<uint8_t> read_file(uint32_t file_block_num, size_t offset, size_t size) {
        if (file_block_num == 0) {
            return {};
        }
        
        const auto* file_block = get_block<FileBlock>(file_block_num);
        if (!file_block) return {};
        
        uint32_t file_size = endian::from_big_endian(file_block->file_size);
        if (offset >= file_size) return {};
        
        size = std::min(size, file_size - offset);
        std::vector<uint8_t> result;
        result.reserve(size);
        
        uint32_t first_data_block = endian::from_big_endian(file_block->first_data);
        if (first_data_block == 0) {
            return {};
        }
        
        uint32_t current_block = first_data_block;
        size_t bytes_read = 0;
        size_t current_offset = 0;
        
        while (current_block != 0 && bytes_read < size) {
            const auto* data_block = get_block<DataBlock>(current_block);
            if (!data_block) break;
            
            uint32_t data_size = endian::from_big_endian(data_block->data_size);
            data_size = std::min(data_size, 488u);
            
            size_t block_start = 0;
            size_t block_read_size = data_size;
            
            if (current_offset + data_size <= offset) {
                current_offset += data_size;
                current_block = endian::from_big_endian(data_block->next_data);
                continue;
            }
            
            if (current_offset < offset) {
                block_start = offset - current_offset;
                block_read_size = data_size - block_start;
            }
            
            block_read_size = std::min(block_read_size, size - bytes_read);
            
            const uint8_t* block_data = data_block->data + block_start;
            result.insert(result.end(), block_data, block_data + block_read_size);
            
            bytes_read += block_read_size;
            current_offset += data_size;
            current_block = endian::from_big_endian(data_block->next_data);
        }
        
        return result;
    }
    
    int write_file(uint32_t file_block_num, const void* buf, size_t size, size_t offset) {
        if (read_only_) return -EROFS;
        if (file_block_num == 0) return -ENOENT;
        
        auto* file_block = get_block_writable<FileBlock>(file_block_num);
        if (!file_block) return -EIO;
        
        uint32_t current_size = endian::from_big_endian(file_block->file_size);
        uint32_t new_size = std::max(current_size, static_cast<uint32_t>(offset + size));
        
        // Update file size if needed
        if (new_size != current_size) {
            file_block->file_size = endian::to_big_endian(new_size);
            update_checksum(file_block);
        }
        
        // Write to data blocks
        uint32_t first_data_block = endian::from_big_endian(file_block->first_data);
        if (first_data_block == 0 && size > 0) {
            // Need to allocate first data block
            first_data_block = allocate_block();
            if (first_data_block == 0) return -ENOSPC;
            
            file_block->first_data = endian::to_big_endian(first_data_block);
            update_checksum(file_block);
            
            // Initialize data block
            auto* data_block = get_block_writable<DataBlock>(first_data_block);
            if (data_block) {
                data_block->type = endian::to_big_endian(static_cast<uint32_t>(T_DATA));
                data_block->header_key = endian::to_big_endian(file_block_num);
                data_block->seq_num = endian::to_big_endian(1u);
                data_block->next_data = 0;
            }
        }
        
        // Navigate to correct data block and write
        uint32_t current_block = first_data_block;
        uint32_t prev_block = 0;
        size_t current_offset = 0;
        size_t bytes_written = 0;
        const uint8_t* data = static_cast<const uint8_t*>(buf);
        
        while (bytes_written < size) {
            if (current_block == 0) {
                // Need to allocate new data block
                current_block = allocate_block();
                if (current_block == 0) return bytes_written; // Partial write
                
                // Link from previous block
                if (prev_block != 0) {
                    auto* prev_data = get_block_writable<DataBlock>(prev_block);
                    if (prev_data) {
                        prev_data->next_data = endian::to_big_endian(current_block);
                        update_checksum(prev_data);
                    }
                }
                
                // Initialize new data block
                auto* new_data = get_block_writable<DataBlock>(current_block);
                if (new_data) {
                    new_data->type = endian::to_big_endian(static_cast<uint32_t>(T_DATA));
                    new_data->header_key = endian::to_big_endian(file_block_num);
                    new_data->seq_num = endian::to_big_endian(current_offset / 488 + 1);
                    new_data->next_data = 0;
                    new_data->data_size = 0;
                }
            }
            
            auto* data_block = get_block_writable<DataBlock>(current_block);
            if (!data_block) return -EIO;
            
            uint32_t block_data_size = endian::from_big_endian(data_block->data_size);
            
            // Skip blocks until we reach the offset
            if (current_offset + 488 <= offset) {
                current_offset += 488;
                prev_block = current_block;
                current_block = endian::from_big_endian(data_block->next_data);
                continue;
            }
            
            // Calculate write position within this block
            size_t block_offset = (current_offset < offset) ? (offset - current_offset) : 0;
            size_t write_size = std::min(size - bytes_written, 488 - block_offset);
            
            // Write data
            std::memcpy(data_block->data + block_offset, data + bytes_written, write_size);
            
            // Update block size if needed
            uint32_t new_block_size = block_offset + write_size;
            if (new_block_size > block_data_size) {
                data_block->data_size = endian::to_big_endian(new_block_size);
            }
            
            update_checksum(data_block);
            
            bytes_written += write_size;
            current_offset += 488;
            prev_block = current_block;
            current_block = endian::from_big_endian(data_block->next_data);
        }
        
        return bytes_written;
    }
    
    int create_file(const std::string& path, mode_t mode) {
        if (read_only_) return -EROFS;
        
        size_t last_slash = path.find_last_of('/');
        std::string parent_path = (last_slash == 0) ? "/" : path.substr(0, last_slash);
        std::string filename = path.substr(last_slash + 1);
        
        if (filename.length() > BCPL_STRING_MAX) return -ENAMETOOLONG;
        
        // Check if file already exists
        if (get_entry(path).has_value()) return -EEXIST;
        
        // Find parent directory block
        uint32_t parent_block = find_directory_block(parent_path);
        if (parent_block == 0) return -ENOENT;
        
        // Allocate new file block
        uint32_t file_block = allocate_block();
        if (file_block == 0) return -ENOSPC;
        
        // Initialize file block
        auto* file = get_block_writable<FileBlock>(file_block);
        if (!file) {
            free_block(file_block);
            return -EIO;
        }
        
        std::memset(file, 0, BLOCK_SIZE);
        file->type = endian::to_big_endian(static_cast<uint32_t>(T_HEADER));
        file->header_key = endian::to_big_endian(file_block);
        file->parent = endian::to_big_endian(parent_block);
        file->sec_type = endian::to_big_endian(ST_FILE);
        file->file_size = 0;
        file->first_data = 0;
        
        // Set filename
        BcplString::write(file->filename, filename);
        
        // Set timestamps
        auto [days, mins, ticks] = unix_to_amiga_time(time(nullptr));
        file->days = endian::to_big_endian(days);
        file->mins = endian::to_big_endian(mins);
        file->ticks = endian::to_big_endian(ticks);
        
        update_checksum(file);
        
        // Add to parent directory hash table
        add_to_directory(parent_block, file_block, filename);
        
        // Clear directory cache
        dir_cache_.clear();
        
        return 0;
    }
    
    int delete_file(const std::string& path) {
        if (read_only_) return -EROFS;
        
        auto entry = get_entry(path);
        if (!entry) return -ENOENT;
        if (entry->is_directory) return -EISDIR;
        
        size_t last_slash = path.find_last_of('/');
        std::string parent_path = (last_slash == 0) ? "/" : path.substr(0, last_slash);
        uint32_t parent_block = find_directory_block(parent_path);
        
        // Remove from parent directory
        remove_from_directory(parent_block, entry->block_num, entry->name);
        
        // Free data blocks
        auto* file = get_block<FileBlock>(entry->block_num);
        if (file) {
            uint32_t data_block = endian::from_big_endian(file->first_data);
            while (data_block != 0) {
                auto* data = get_block<DataBlock>(data_block);
                uint32_t next = data ? endian::from_big_endian(data->next_data) : 0;
                free_block(data_block);
                data_block = next;
            }
        }
        
        // Free file block
        free_block(entry->block_num);
        
        // Clear cache
        dir_cache_.clear();
        
        return 0;
    }
    
    int truncate_file(const std::string& path, off_t size) {
        if (read_only_) return -EROFS;
        
        auto entry = get_entry(path);
        if (!entry) return -ENOENT;
        if (entry->is_directory) return -EISDIR;
        
        auto* file = get_block_writable<FileBlock>(entry->block_num);
        if (!file) return -EIO;
        
        uint32_t current_size = endian::from_big_endian(file->file_size);
        
        if (size == current_size) return 0;
        
        if (size < current_size) {
            // Truncate - free excess data blocks
            uint32_t blocks_needed = (size + 487) / 488;
            uint32_t current_blocks = (current_size + 487) / 488;
            
            if (blocks_needed < current_blocks) {
                // Find and free excess blocks
                uint32_t block_count = 0;
                uint32_t data_block = endian::from_big_endian(file->first_data);
                uint32_t prev_block = 0;
                
                while (data_block != 0 && block_count < blocks_needed) {
                    prev_block = data_block;
                    auto* data = get_block<DataBlock>(data_block);
                    data_block = data ? endian::from_big_endian(data->next_data) : 0;
                    block_count++;
                }
                
                // Free remaining blocks
                while (data_block != 0) {
                    auto* data = get_block<DataBlock>(data_block);
                    uint32_t next = data ? endian::from_big_endian(data->next_data) : 0;
                    free_block(data_block);
                    data_block = next;
                }
                
                // Update last block's next pointer
                if (prev_block != 0) {
                    auto* data = get_block_writable<DataBlock>(prev_block);
                    if (data) {
                        data->next_data = 0;
                        update_checksum(data);
                    }
                }
            }
        }
        
        // Update file size
        file->file_size = endian::to_big_endian(static_cast<uint32_t>(size));
        update_checksum(file);
        
        return 0;
    }
    
    int create_directory(const std::string& path, mode_t mode) {
        if (read_only_) return -EROFS;
        
        size_t last_slash = path.find_last_of('/');
        std::string parent_path = (last_slash == 0) ? "/" : path.substr(0, last_slash);
        std::string dirname = path.substr(last_slash + 1);
        
        if (dirname.length() > BCPL_STRING_MAX) return -ENAMETOOLONG;
        
        // Check if already exists
        if (get_entry(path).has_value()) return -EEXIST;
        
        // Find parent directory
        uint32_t parent_block = find_directory_block(parent_path);
        if (parent_block == 0) return -ENOENT;
        
        // Allocate new directory block
        uint32_t dir_block = allocate_block();
        if (dir_block == 0) return -ENOSPC;
        
        // Initialize directory block
        auto* dir = get_block_writable<FileBlock>(dir_block);
        if (!dir) {
            free_block(dir_block);
            return -EIO;
        }
        
        std::memset(dir, 0, BLOCK_SIZE);
        dir->type = endian::to_big_endian(static_cast<uint32_t>(T_HEADER));
        dir->header_key = endian::to_big_endian(dir_block);
        dir->parent = endian::to_big_endian(parent_block);
        dir->sec_type = endian::to_big_endian(ST_DIR);
        
        // Set directory name
        BcplString::write(dir->filename, dirname);
        
        // Set timestamps
        auto [days, mins, ticks] = unix_to_amiga_time(time(nullptr));
        dir->days = endian::to_big_endian(days);
        dir->mins = endian::to_big_endian(mins);
        dir->ticks = endian::to_big_endian(ticks);
        
        update_checksum(dir);
        
        // Add to parent directory
        add_to_directory(parent_block, dir_block, dirname);
        
        // Clear cache
        dir_cache_.clear();
        
        return 0;
    }
    
    int delete_directory(const std::string& path) {
        if (read_only_) return -EROFS;
        if (path == "/" || path.empty()) return -EINVAL;
        
        auto entry = get_entry(path);
        if (!entry) return -ENOENT;
        if (!entry->is_directory) return -ENOTDIR;
        
        // Check if directory is empty
        auto contents = list_directory(path);
        if (contents && !contents->empty()) return -ENOTEMPTY;
        
        size_t last_slash = path.find_last_of('/');
        std::string parent_path = (last_slash == 0) ? "/" : path.substr(0, last_slash);
        uint32_t parent_block = find_directory_block(parent_path);
        
        // Remove from parent directory
        remove_from_directory(parent_block, entry->block_num, entry->name);
        
        // Free directory block
        free_block(entry->block_num);
        
        // Clear cache
        dir_cache_.clear();
        
        return 0;
    }
    
    const std::string& volume_name() const { return volume_name_; }
    bool is_ffs() const { return is_ffs_; }
    
    void clear_cache() {
        dir_cache_.clear();
    }
    
    size_t get_actual_file_size(uint32_t file_block_num) {
        const auto* file_block = get_block<FileBlock>(file_block_num);
        if (!file_block) return 0;
        
        return static_cast<size_t>(endian::from_big_endian(file_block->file_size));
    }
    
private:
    std::optional<std::vector<Entry>> get_cached_dir(const std::string& path) {
        auto it = dir_cache_.find(path);
        if (it != dir_cache_.end()) {
            return it->second;
        }
        return std::nullopt;
    }
    
    void cache_directory(const std::string& path, const std::vector<Entry>& entries) {
        dir_cache_[path] = entries;
    }
    
    uint32_t find_directory_block(const std::string& path) {
        if (path == "/" || path.empty()) {
            return root_block_num_;
        }
        
        auto entry = get_entry(path);
        if (entry && entry->is_directory) {
            return entry->block_num;
        }
        
        return 0;
    }
    
    void add_to_directory(uint32_t dir_block, uint32_t file_block, const std::string& name) {
        uint32_t hash = hash_name(name);
        
        if (dir_block == root_block_num_) {
            auto* root = get_block_writable<RootBlock>(dir_block);
            if (!root) return;
            
            uint32_t existing = endian::from_big_endian(root->hash_table[hash]);
            root->hash_table[hash] = endian::to_big_endian(file_block);
            
            // Link to existing chain
            auto* file = get_block_writable<FileBlock>(file_block);
            if (file) {
                file->hash_chain = endian::to_big_endian(existing);
                update_checksum(file);
            }
            
            update_checksum(root);
        } else {
            auto* dir = get_block_writable<FileBlock>(dir_block);
            if (!dir) return;
            
            uint32_t existing = endian::from_big_endian(dir->data_blocks[hash]);
            dir->data_blocks[hash] = endian::to_big_endian(file_block);
            
            // Link to existing chain
            auto* file = get_block_writable<FileBlock>(file_block);
            if (file) {
                file->hash_chain = endian::to_big_endian(existing);
                update_checksum(file);
            }
            
            update_checksum(dir);
        }
    }
    
    void remove_from_directory(uint32_t dir_block, uint32_t file_block, const std::string& name) {
        uint32_t hash = hash_name(name);
        
        if (dir_block == root_block_num_) {
            auto* root = get_block_writable<RootBlock>(dir_block);
            if (!root) return;
            
            uint32_t current = endian::from_big_endian(root->hash_table[hash]);
            if (current == file_block) {
                // First in chain
                auto* file = get_block<FileBlock>(file_block);
                root->hash_table[hash] = file ? file->hash_chain : 0;
                update_checksum(root);
            } else {
                // Search chain
                remove_from_chain(current, file_block);
            }
        } else {
            auto* dir = get_block_writable<FileBlock>(dir_block);
            if (!dir) return;
            
            uint32_t current = endian::from_big_endian(dir->data_blocks[hash]);
            if (current == file_block) {
                // First in chain
                auto* file = get_block<FileBlock>(file_block);
                dir->data_blocks[hash] = file ? file->hash_chain : 0;
                update_checksum(dir);
            } else {
                // Search chain
                remove_from_chain(current, file_block);
            }
        }
    }
    
    void remove_from_chain(uint32_t start_block, uint32_t target_block) {
        uint32_t current = start_block;
        
        while (current != 0) {
            auto* block = get_block_writable<FileBlock>(current);
            if (!block) break;
            
            uint32_t next = endian::from_big_endian(block->hash_chain);
            if (next == target_block) {
                // Found it - unlink
                auto* target = get_block<FileBlock>(target_block);
                block->hash_chain = target ? target->hash_chain : 0;
                update_checksum(block);
                break;
            }
            
            current = next;
        }
    }
    
    time_t amiga_to_unix_time(uint32_t days, uint32_t mins, uint32_t ticks) {
        // Amiga epoch is Jan 1, 1978
        // Unix epoch is Jan 1, 1970
        // Difference is 2922 days
        constexpr time_t AMIGA_EPOCH_OFFSET = 2922 * 24 * 60 * 60;
        
        time_t seconds = days * 24 * 60 * 60;
        seconds += mins * 60;
        seconds += ticks / 50; // 50 ticks per second
        
        return seconds + AMIGA_EPOCH_OFFSET;
    }
    
    std::tuple<uint32_t, uint32_t, uint32_t> unix_to_amiga_time(time_t unix_time) {
        constexpr time_t AMIGA_EPOCH_OFFSET = 2922 * 24 * 60 * 60;
        
        time_t amiga_time = unix_time - AMIGA_EPOCH_OFFSET;
        
        uint32_t days = amiga_time / (24 * 60 * 60);
        amiga_time %= (24 * 60 * 60);
        
        uint32_t mins = amiga_time / 60;
        amiga_time %= 60;
        
        uint32_t ticks = amiga_time * 50; // 50 ticks per second
        
        return {days, mins, ticks};
    }
};

// Global ADF image
static std::unique_ptr<AdfImage> g_adf_image;

// Standard FUSE operations with write support
namespace fuse_ops {

static int getattr(const char* path, struct stat* stbuf) {
    std::memset(stbuf, 0, sizeof(struct stat));
    
    if (!g_adf_image) return -EIO;
    
    auto entry = g_adf_image->get_entry(path);
    if (!entry) return -ENOENT;
    
    stbuf->st_ino = std::hash<std::string>{}(path);
    if (stbuf->st_ino <= 1) stbuf->st_ino = 2;
    
    if (entry->is_directory) {
        stbuf->st_mode = S_IFDIR | (g_adf_image->is_read_only() ? 0555 : 0755);
        stbuf->st_nlink = 2;
        stbuf->st_size = 0;
    } else {
        stbuf->st_mode = S_IFREG | (g_adf_image->is_read_only() ? 0444 : 0644);
        stbuf->st_nlink = 1;
        
        // Get actual file size from file block, not cached directory entry
        size_t actual_size = g_adf_image->get_actual_file_size(entry->block_num);
        stbuf->st_size = static_cast<off_t>(actual_size);
    }
    
    stbuf->st_uid = getuid();
    stbuf->st_gid = getgid();
    
    time_t mtime = entry->mtime;
    stbuf->st_atime = mtime;
    stbuf->st_mtime = mtime;
    stbuf->st_ctime = mtime;
    
    stbuf->st_blocks = (stbuf->st_size + 511) / 512;
    stbuf->st_blksize = 512;
    
    return 0;
}

static int readdir(const char* path, void* buf, fuse_fill_dir_t filler,
                   off_t, struct fuse_file_info*) {
    if (!g_adf_image) return -EIO;
    
    auto entries = g_adf_image->list_directory(path);
    if (!entries) return -ENOENT;
    
    if (filler(buf, ".", nullptr, 0) || filler(buf, "..", nullptr, 0)) {
        return -ENOMEM;
    }
    
    for (const auto& entry : *entries) {
        if (filler(buf, entry.name.c_str(), nullptr, 0)) {
            break;
        }
    }
    
    return 0;
}

static int open(const char* path, struct fuse_file_info* fi) {
    if (!g_adf_image) return -EIO;
    
    auto entry = g_adf_image->get_entry(path);
    if (!entry) return -ENOENT;
    if (entry->is_directory) return -EISDIR;
    
    fi->fh = entry->block_num;
    return 0;
}

static int read(const char* path, char* buf, size_t size, off_t offset,
                struct fuse_file_info* fi) {
    if (!g_adf_image) return -EIO;
    
    uint32_t block_num = static_cast<uint32_t>(fi->fh);
    if (block_num == 0) {
        auto entry = g_adf_image->get_entry(path);
        if (!entry || entry->is_directory) return -ENOENT;
        block_num = entry->block_num;
    }
    
    auto data = g_adf_image->read_file(block_num, static_cast<size_t>(offset), size);
    if (data.empty() && size > 0) return 0;
    
    std::memcpy(buf, data.data(), data.size());
    return static_cast<int>(data.size());
}

static int write(const char* path, const char* buf, size_t size, off_t offset,
                 struct fuse_file_info* fi) {
    if (!g_adf_image) return -EIO;
    
    uint32_t block_num = static_cast<uint32_t>(fi->fh);
    if (block_num == 0) {
        auto entry = g_adf_image->get_entry(path);
        if (!entry || entry->is_directory) return -ENOENT;
        block_num = entry->block_num;
    }
    
    int result = g_adf_image->write_file(block_num, buf, size, static_cast<size_t>(offset));
    
    // Clear directory cache so getattr reports updated file size
    if (result > 0) {
        g_adf_image->clear_cache();
    }
    
    return result;
}

static int create(const char* path, mode_t mode, struct fuse_file_info* fi) {
    if (!g_adf_image) return -EIO;
    
    int result = g_adf_image->create_file(path, mode);
    if (result == 0) {
        auto entry = g_adf_image->get_entry(path);
        if (entry) {
            fi->fh = entry->block_num;
        }
    }
    
    return result;
}

static int unlink(const char* path) {
    if (!g_adf_image) return -EIO;
    return g_adf_image->delete_file(path);
}

static int truncate(const char* path, off_t size) {
    if (!g_adf_image) return -EIO;
    int result = g_adf_image->truncate_file(path, size);
    if (result == 0) {
        g_adf_image->clear_cache();
    }
    return result;
}

static int mkdir(const char* path, mode_t mode) {
    if (!g_adf_image) return -EIO;
    return g_adf_image->create_directory(path, mode);
}

static int rmdir(const char* path) {
    if (!g_adf_image) return -EIO;
    return g_adf_image->delete_directory(path);
}

} // namespace fuse_ops

// FUSE operations structure with write support
static struct fuse_operations amiga_fuse_operations = {
    .getattr = fuse_ops::getattr,
    .readdir = fuse_ops::readdir,
    .open = fuse_ops::open,
    .read = fuse_ops::read,
    .write = fuse_ops::write,
    .create = fuse_ops::create,
    .unlink = fuse_ops::unlink,
    .truncate = fuse_ops::truncate,
    .mkdir = fuse_ops::mkdir,
    .rmdir = fuse_ops::rmdir,
};

} // namespace amiga_fuse

int main(int argc, char* argv[]) {
    using namespace amiga_fuse;
    
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <adf_file> <mount_point> [fuse_options]\n";
        return 1;
    }
    
    g_adf_image = std::make_unique<AdfImage>(argv[1]);
    if (!g_adf_image->open(true)) {  // true = try to open with write support
        std::cerr << "Failed to open ADF file: " << argv[1] << "\n";
        return 1;
    }
    
    std::cout << "Mounted ADF volume: " << g_adf_image->volume_name();
    if (g_adf_image->is_ffs()) {
        std::cout << " (FFS)";
    }
    if (g_adf_image->is_read_only()) {
        std::cout << " [READ-ONLY]";
    } else {
        std::cout << " [READ-WRITE]";
    }
    std::cout << "\n";
    
    // Adjust arguments for FUSE
    argv[1] = argv[2];
    argc--;
    
    return fuse_main(argc, argv, &amiga_fuse_operations, nullptr);
}
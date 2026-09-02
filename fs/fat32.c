#include "fat32.h"
#include "../drivers/disk.h"
#include "../drivers/screen.h"
#include "../kernel/heap.h"
#include "../libc/mem.h"

// Read/write FAT32. See fat32.h for the scope and docs/reference/fat32.md for the
// on-disk layout this parses and the write path it adds.

// The boot sector is block 0 of the volume. The image is formatted as a
// "superfloppy" (the FAT32 volume starts at block 0, no partition table), so
// there is no partition entry to walk first.
#define FAT32_BOOT_BLOCK 0

// Offset 510 of the boot sector holds 0xAA55, little-endian. Every FAT volume
// has it; its absence means block 0 is not a boot sector at all.
#define FAT32_SIGNATURE_OFFSET 510
#define FAT32_SIGNATURE        0xAA55

// A cluster is a run of consecutive blocks treated as one unit. disk_read takes
// a uint8_t count, so one call moves at most 255 blocks, and a cluster read has
// to respect that. It does so by construction: sectors_per_cluster is itself a
// single byte on disk, so it cannot ask for more blocks than one disk_read can
// carry. The guard below pins that reasoning down at compile time, because it is
// the only thing keeping every cluster read to a single call.
#define FAT32_MAX_SECTORS_PER_CLUSTER 255

// FAT slots 0 and 1 are reserved by the format and never describe file data, so
// the first cluster that can hold data is cluster 2. See cluster_to_block below.
#define FAT32_FIRST_DATA_CLUSTER 2

// ---------------------------------------------------------------------------
// FAT entries.
// ---------------------------------------------------------------------------
// One 32-bit slot per cluster, holding the number of the next cluster in the
// file. A slot holding zero means the cluster is free, so the same table is both
// the chain map and the free list.
#define FAT32_ENTRY_BYTES 4

// Only the low 28 bits of an entry are meaningful; the top 4 are reserved and
// may hold anything. Every entry must be masked before it is compared against
// anything below. Skipping the mask is the classic FAT32 bug: end-of-chain
// detection then fails intermittently, depending on what the formatter happened
// to leave in the reserved bits.
#define FAT32_ENTRY_MASK 0x0FFFFFFF

#define FAT32_CLUSTER_FREE 0x00000000
#define FAT32_CLUSTER_BAD  0x0FFFFFF7
// Any masked value at or above this marks the last cluster of a file. It is a
// range, not a single value, because different formatters write different
// end-of-chain markers (0x0FFFFFF8 and 0x0FFFFFFF are both common).
#define FAT32_CLUSTER_END  0x0FFFFFF8

// fat32_next_cluster's three outcomes, kept distinct because "the chain ended"
// and "the read failed" are different facts and callers act on them differently.
#define FAT32_CHAIN_NEXT  0
#define FAT32_CHAIN_END   1
#define FAT32_CHAIN_ERROR (-1)

// ---------------------------------------------------------------------------
// Directory entries.
// ---------------------------------------------------------------------------
#define FAT32_DIRENT_BYTES 32

// The first byte of an entry doubles as a marker.
#define FAT32_DIRENT_FREE    0x00  // never used: no more entries in this directory
#define FAT32_DIRENT_DELETED 0xE5  // was used, since deleted: skip it

// Attribute byte bits.
#define FAT32_ATTR_READ_ONLY 0x01
#define FAT32_ATTR_HIDDEN    0x02
#define FAT32_ATTR_SYSTEM    0x04
#define FAT32_ATTR_VOLUME_ID 0x08
#define FAT32_ATTR_DIRECTORY 0x10
#define FAT32_ATTR_ARCHIVE   0x20

// An entry whose whole attribute byte equals this is not a file at all: it is
// one fragment of a long filename, bolted onto the format later and deliberately
// given an attribute combination old software would ignore. Out of scope here,
// so skip these; the 8.3 entry the fragments describe follows them.
#define FAT32_ATTR_LONG_NAME 0x0F

// The 8.3 name is 11 bytes with no dot stored: 8 bytes of base name then 3 of
// extension, both space-padded, uppercase. HELLO.TXT is stored as "HELLO   TXT".
#define FAT32_NAME_LENGTH 11
#define FAT32_BASE_LENGTH 8
#define FAT32_EXT_LENGTH  3
// Longest display form: 8 base + '.' + 3 extension + terminator.
#define FAT32_DISPLAY_NAME_MAX 13

// ---------------------------------------------------------------------------
// The FSInfo sector.
// ---------------------------------------------------------------------------
// A single sector (its number is named by the BPB) caching two things a writer
// would otherwise recompute: the free-cluster count and a next-free hint. TownOS
// does not maintain them — keeping them correct is a caching problem, and getting
// it subtly wrong is worse than not having them — so on every write it sets both
// to the format's "unknown, recount" value and lets anything that cares recount.
// See docs/decisions/0020-writable-fat32.md.
//
// Three signatures fence the sector. All must match before a byte is written into
// it: the sector number came from a boot sector that might be corrupt, and writing
// into the wrong sector on that guess is how a volume gets destroyed.
#define FAT32_FSINFO_LEAD_SIG_OFFSET    0
#define FAT32_FSINFO_LEAD_SIG           0x41615252
#define FAT32_FSINFO_STRUCT_SIG_OFFSET  484
#define FAT32_FSINFO_STRUCT_SIG         0x61417272
#define FAT32_FSINFO_TRAIL_SIG_OFFSET   508
#define FAT32_FSINFO_TRAIL_SIG          0xAA550000
#define FAT32_FSINFO_FREE_COUNT_OFFSET  488
#define FAT32_FSINFO_NEXT_FREE_OFFSET   492
// The legal "unknown" value for both cached fields: recount before trusting.
#define FAT32_FSINFO_UNKNOWN            0xFFFFFFFF

// ---------------------------------------------------------------------------
// The BIOS Parameter Block: the filesystem describing its own shape.
// ---------------------------------------------------------------------------
// __attribute__((packed)) is load-bearing, not decoration. These fields sit at
// unaligned offsets on disk (bytes_per_sector is at offset 11, a 16-bit field at
// an odd address). Without packed, the compiler inserts padding to align them
// and every field after the first misalignment silently reads the wrong bytes.
// This is the same trap as the Multiboot mmap entry. If every parsed field after
// the first looks like garbage, check that this attribute is still here.
//
// The struct stops at root_cluster because nothing past it is needed for
// reading. The trailing boot code and the 0xAA55 signature are checked straight
// out of the raw block instead.
struct fat32_bpb {
    uint8_t  jump[3];                  // 0:  jump over the BPB to the boot code
    uint8_t  oem_name[8];              // 3:  formatter's name, informational
    uint16_t bytes_per_sector;         // 11: block size, 512 on this platform
    uint8_t  sectors_per_cluster;      // 13: blocks glued into one cluster
    uint16_t reserved_sector_count;    // 14: blocks before the first FAT
    uint8_t  num_fats;                 // 16: how many copies of the FAT follow
    uint16_t root_entry_count;         // 17: FAT12/16 only, 0 here
    uint16_t total_sectors_16;         // 19: FAT12/16 only, 0 here
    uint8_t  media;                    // 21: media descriptor byte
    uint16_t sectors_per_fat_16;       // 22: FAT12/16 only, 0 here (see below)
    uint16_t sectors_per_track;        // 24: legacy CHS geometry, unused
    uint16_t num_heads;                // 26: legacy CHS geometry, unused
    uint32_t hidden_sectors;           // 28: blocks before this volume
    uint32_t total_sectors_32;         // 32: size of the volume in blocks
    uint32_t sectors_per_fat_32;       // 36: blocks in one FAT copy
    uint16_t ext_flags;                // 40: FAT mirroring flags
    uint16_t fs_version;               // 42: format version
    uint32_t root_cluster;             // 44: first cluster of the root directory
    uint16_t fs_info;                  // 48: sector number of the FSInfo structure
} __attribute__((packed));

// Compile-time guard on the above. If padding ever creeps back in, the array
// size goes negative and the build fails here rather than at runtime with
// nonsense geometry. 50 is where fs_info ends (offset 48 plus 2 bytes).
#define FAT32_BPB_SIZE 50
typedef char fat32_bpb_is_packed[(sizeof(struct fat32_bpb) == FAT32_BPB_SIZE) ? 1 : -1];

// One cluster is always one disk_read: an 8-bit block count can hold any value
// an 8-bit sectors_per_cluster field can produce. Widen either and the cluster
// read must be split into several calls, so fail the build here instead.
typedef char fat32_cluster_fits_one_read[
    (sizeof(((struct fat32_bpb *)0)->sectors_per_cluster) == 1 &&
     FAT32_MAX_SECTORS_PER_CLUSTER >= 255) ? 1 : -1];

// ---------------------------------------------------------------------------
// A directory entry: the mapping from a name to a starting cluster and a size.
// ---------------------------------------------------------------------------
// A directory is just a file whose contents are a run of these, 32 bytes each.
// Packed for the same reason as the BPB: these offsets are fixed by the format,
// not by the compiler's alignment preferences.
//
// first_cluster is split across two 16-bit fields at opposite ends of the entry
// (a FAT16 layout with the high half bolted into a gap later). They must be
// recombined as (high << 16) | low. Reading only the low half is easy to do by
// accident and yields a wildly wrong cluster number on any volume large enough
// for the high half to be non-zero.
struct fat32_dirent {
    uint8_t  name[FAT32_NAME_LENGTH];  // 0:  8.3, space-padded, no dot stored
    uint8_t  attr;                     // 11: the attribute bits above
    uint8_t  nt_reserved;              // 12: reserved
    uint8_t  create_time_tenths;       // 13: creation time, tenths of a second
    uint16_t create_time;              // 14: creation time
    uint16_t create_date;              // 16: creation date
    uint16_t access_date;              // 18: last access date
    uint16_t first_cluster_high;       // 20: high 16 bits of the start cluster
    uint16_t write_time;               // 22: last write time
    uint16_t write_date;               // 24: last write date
    uint16_t first_cluster_low;        // 26: low 16 bits of the start cluster
    uint32_t size;                     // 28: file length in bytes
} __attribute__((packed));

typedef char fat32_dirent_is_packed[
    (sizeof(struct fat32_dirent) == FAT32_DIRENT_BYTES) ? 1 : -1];

// ---------------------------------------------------------------------------
// Cached volume geometry, filled in by fat32_init.
// ---------------------------------------------------------------------------
// Parsed once from the boot sector because every read needs it and re-reading
// block 0 per operation would be pointless I/O.
static uint32_t fs_bytes_per_sector;
static uint32_t fs_sectors_per_cluster;
static uint32_t fs_bytes_per_cluster;
static uint32_t fs_first_fat_block;    // block of the first FAT copy
static uint32_t fs_sectors_per_fat;
static uint32_t fs_num_fats;           // how many identical FAT copies to keep in step
static uint32_t fs_first_data_block;   // block where cluster 2 begins
static uint32_t fs_root_cluster;
static uint32_t fs_total_clusters;     // count of data clusters on the volume
static uint32_t fs_fsinfo_block;       // LBA of the FSInfo sector, or 0/0xFFFF if none
static int      fs_ready;              // 0 until a successful fat32_init

// Where the next free-cluster scan starts, so allocation does not restart from
// cluster 2 every time and re-examine a long prefix it already knows is full.
// This is the in-memory twin of the FSInfo sector's next-free field, and exists
// for the same reason. It is only a hint: a stale value costs at most one wasted
// wrap of the scan, never a wrong answer, because every candidate is still
// checked against the FAT itself.
static uint32_t fs_next_free_hint = FAT32_FIRST_DATA_CLUSTER;

// A power of two has exactly one bit set, so n & (n - 1) clears it to zero.
static int is_power_of_two(uint32_t n) {
    return n != 0 && (n & (n - 1)) == 0;
}

int fat32_init(void) {
    fs_ready = 0;

    // The boot sector is one block, small enough for the stack.
    uint8_t block[DISK_SECTOR_SIZE];
    if (disk_read(FAT32_BOOT_BLOCK, 1, block) != 0) {
        print_string("FAT32: cannot read boot sector\n");
        return -1;
    }

    // Read the signature straight out of the raw block, little-endian. This is
    // the cheapest "is this even a filesystem" check, so do it before trusting
    // any other field.
    uint16_t signature = (uint16_t)(block[FAT32_SIGNATURE_OFFSET] |
                                    (block[FAT32_SIGNATURE_OFFSET + 1] << 8));
    if (signature != FAT32_SIGNATURE) {
        print_string("FAT32: no 0xAA55 boot signature, not a FAT volume\n");
        return -1;
    }

    struct fat32_bpb *bpb = (struct fat32_bpb *)block;

    // Sanity-check the geometry before computing anything from it. Proceeding
    // with garbage here would produce block numbers pointing anywhere on the
    // disk, and the failure would surface later as unreadable file contents
    // rather than as the parse error it actually is.
    if (bpb->bytes_per_sector != DISK_SECTOR_SIZE) {
        print_string("FAT32: bytes per sector is not 512\n");
        return -1;
    }
    if (!is_power_of_two(bpb->sectors_per_cluster)) {
        print_string("FAT32: sectors per cluster is not a power of two\n");
        return -1;
    }
    if (bpb->num_fats == 0 || bpb->reserved_sector_count == 0) {
        print_string("FAT32: no FAT copies or no reserved area\n");
        return -1;
    }
    // sectors_per_fat_16 is zero on FAT32 and the 32-bit field carries the real
    // value. Reading the 16-bit one is a classic mistake: it yields a FAT length
    // of zero, so the data area appears to start on top of the FAT.
    if (bpb->sectors_per_fat_32 == 0) {
        print_string("FAT32: FAT length is zero, volume is not FAT32\n");
        return -1;
    }
    if (bpb->root_cluster < FAT32_FIRST_DATA_CLUSTER) {
        print_string("FAT32: root cluster below 2\n");
        return -1;
    }

    fs_bytes_per_sector    = bpb->bytes_per_sector;
    fs_sectors_per_cluster = bpb->sectors_per_cluster;
    fs_bytes_per_cluster   = fs_bytes_per_sector * fs_sectors_per_cluster;
    fs_sectors_per_fat     = bpb->sectors_per_fat_32;
    fs_num_fats            = bpb->num_fats;
    fs_root_cluster        = bpb->root_cluster;
    // The volume starts at block 0 (a superfloppy), so the FSInfo sector number
    // from the BPB is already an LBA. Nothing is read from it at mount; it is only
    // written, to invalidate it, and only after its signatures are verified there.
    fs_fsinfo_block        = bpb->fs_info;

    // The volume is laid out as: reserved area, then num_fats copies of the FAT
    // back to back, then the data area. So the first FAT starts right after the
    // reserved area, and the data starts right after the last FAT copy. The FAT
    // copy count is cached (fs_num_fats), not just used here and discarded: a
    // writer has to patch every copy on every edit, so it needs it later too.
    fs_first_fat_block  = bpb->reserved_sector_count;
    fs_first_data_block = bpb->reserved_sector_count +
                          fs_num_fats * fs_sectors_per_fat;

    if (bpb->total_sectors_32 <= fs_first_data_block) {
        print_string("FAT32: volume smaller than its own metadata\n");
        return -1;
    }
    // Data clusters are whatever is left after the metadata. This count bounds
    // chain following (see fat32_next_cluster's callers): a cluster number past
    // the end of the volume is corruption, not data.
    fs_total_clusters = (bpb->total_sectors_32 - fs_first_data_block) /
                        fs_sectors_per_cluster;

    fs_ready = 1;

    print_string("FAT32: ");
    print_int(fs_bytes_per_sector);
    print_string(" B/sector, ");
    print_int(fs_sectors_per_cluster);
    print_string(" sectors/cluster, first data block ");
    print_int(fs_first_data_block);
    print_string(", root cluster ");
    print_int(fs_root_cluster);
    print_string("\n");

    return 0;
}

// ---------------------------------------------------------------------------
// Clusters and chains.
// ---------------------------------------------------------------------------

// Where a cluster's blocks actually live.
//
// The `- 2` is not a bug. FAT slots 0 and 1 are reserved by the format and never
// describe data, so the first cluster that can hold file contents is cluster 2,
// and cluster 2 sits at offset 0 of the data area. Cluster 3 sits one cluster
// in, and so on. If file contents come back shifted by exactly two clusters'
// worth of bytes, this subtraction is missing or wrong.
static uint32_t cluster_to_block(uint32_t cluster) {
    return fs_first_data_block +
           (cluster - FAT32_FIRST_DATA_CLUSTER) * fs_sectors_per_cluster;
}

// Is this a cluster number the volume could actually contain? Data clusters are
// numbered from 2 up to total_clusters + 1. Anything else is corruption, and
// following it would read blocks belonging to something else, or off the end of
// the disk entirely.
static int cluster_in_range(uint32_t cluster) {
    return cluster >= FAT32_FIRST_DATA_CLUSTER &&
           cluster < fs_total_clusters + FAT32_FIRST_DATA_CLUSTER;
}

// Locate one cluster's FAT entry and hand back its value, with no interpretation
// of what that value means. This is the raw accessor shared by the chain reader
// (fat32_next_cluster) and, in stage 3 onward, the allocators.
//
// The FAT is a flat array of 32-bit entries starting at the first FAT block, so
// finding an entry is a division: which block of the table holds it, and where
// in that block it sits.
//
// Only the first FAT copy is read. The format keeps num_fats identical copies
// for redundancy, and reading needs just one of them; fat32_set_entry is what
// keeps them identical. *out_value is masked to the low 28 bits, because the read
// side does not care about the reserved top four; a writer that owns those bits
// preserves them itself. Returns 0 on success, -1 on a read failure.
static int fat32_get_entry(uint32_t cluster, uint32_t *out_value) {
    uint32_t entries_per_block = fs_bytes_per_sector / FAT32_ENTRY_BYTES;
    uint32_t block  = fs_first_fat_block + (cluster / entries_per_block);
    uint32_t offset = (cluster % entries_per_block) * FAT32_ENTRY_BYTES;

    // One block, so this stays on the stack. Cluster buffers are heap-allocated
    // (see fat32_read_file) because a cluster can be far larger than a block.
    uint8_t block_buf[DISK_SECTOR_SIZE];
    if (disk_read(block, 1, block_buf) != 0) {
        return -1;
    }

    // Assembled byte by byte rather than cast through a uint32_t pointer: the
    // entry is little-endian on disk and its offset need not be 4-byte aligned.
    uint32_t entry = (uint32_t)block_buf[offset] |
                     ((uint32_t)block_buf[offset + 1] << 8) |
                     ((uint32_t)block_buf[offset + 2] << 16) |
                     ((uint32_t)block_buf[offset + 3] << 24);

    *out_value = entry & FAT32_ENTRY_MASK;   // top 4 bits are reserved, dropped here
    return 0;
}

// Look up one cluster's FAT entry and report what follows it.
//
// Returns FAT32_CHAIN_NEXT with *next set, FAT32_CHAIN_END at the end of the
// chain, or FAT32_CHAIN_ERROR on a read failure or a corrupt entry.
static int fat32_next_cluster(uint32_t cluster, uint32_t *next) {
    uint32_t entry;
    if (fat32_get_entry(cluster, &entry) != 0) {
        return FAT32_CHAIN_ERROR;
    }

    if (entry >= FAT32_CLUSTER_END) {
        return FAT32_CHAIN_END;
    }
    if (entry == FAT32_CLUSTER_FREE || entry == FAT32_CLUSTER_BAD ||
        !cluster_in_range(entry)) {
        // A live chain never points at a free or bad cluster, and never off the
        // volume. Refuse it rather than read whatever happens to be there.
        return FAT32_CHAIN_ERROR;
    }

    *next = entry;
    return FAT32_CHAIN_NEXT;
}

// Write `value` into cluster's FAT slot, in every copy of the table.
//
// THE SINGLE MOST LIKELY BUG IN THE ENTIRE WRITE PATH IS UPDATING ONLY THE FIRST
// COPY. It is invisible from inside QEMU: TownOS reads the first copy (see
// fat32_get_entry), so the volume stays self-consistent to itself and every
// in-kernel check keeps passing. The damage only surfaces off-machine, when the
// host's tools (mtools, or any real OS) read the volume, consult a different copy
// or cross-check them, and find them disagreeing. So this loops over all
// fs_num_fats copies, and the loop is not optional.
//
// Copy n of the FAT begins at fs_first_fat_block + n * fs_sectors_per_fat, and
// the slot for `cluster` sits at the same block-and-offset within each copy. The
// write is read-modify-write: the disk's unit is a 512-byte block and one entry
// is four bytes, so the block holding the slot is read, four bytes are patched
// little-endian, and the whole block is written back.
//
// Only the low 28 bits belong to us. The top four are reserved by the format, so
// the value on disk keeps its existing top four bits and takes only the low 28
// of `value`. Zeroing the reserved bits would be modifying fields this code does
// not own, and is exactly what makes a host fsck call the volume damaged.
//
// Returns 0 once every copy is written, -1 the moment any read or write fails.
static int fat32_set_entry(uint32_t cluster, uint32_t value) {
    uint32_t entries_per_block = fs_bytes_per_sector / FAT32_ENTRY_BYTES;
    uint32_t block_in_fat = cluster / entries_per_block;
    uint32_t offset       = (cluster % entries_per_block) * FAT32_ENTRY_BYTES;

    for (uint32_t copy = 0; copy < fs_num_fats; copy++) {
        uint32_t block = fs_first_fat_block + copy * fs_sectors_per_fat + block_in_fat;

        uint8_t block_buf[DISK_SECTOR_SIZE];
        if (disk_read(block, 1, block_buf) != 0) {
            return -1;
        }

        // Reassemble the entry already on disk so its reserved top four bits can
        // be carried over, then splice in only the low 28 bits of the new value.
        uint32_t old = (uint32_t)block_buf[offset] |
                       ((uint32_t)block_buf[offset + 1] << 8) |
                       ((uint32_t)block_buf[offset + 2] << 16) |
                       ((uint32_t)block_buf[offset + 3] << 24);
        uint32_t merged = (old & ~FAT32_ENTRY_MASK) | (value & FAT32_ENTRY_MASK);

        block_buf[offset]     = (uint8_t)(merged & 0xFF);
        block_buf[offset + 1] = (uint8_t)((merged >> 8) & 0xFF);
        block_buf[offset + 2] = (uint8_t)((merged >> 16) & 0xFF);
        block_buf[offset + 3] = (uint8_t)((merged >> 24) & 0xFF);

        if (disk_write(block, 1, block_buf) != 0) {
            return -1;
        }
    }
    return 0;
}

// Read one whole cluster into buf, which must hold bytes_per_cluster bytes.
// A single disk_read call by construction; see FAT32_MAX_SECTORS_PER_CLUSTER.
static int read_cluster(uint32_t cluster, uint8_t *buf) {
    if (!cluster_in_range(cluster)) {
        return -1;
    }
    return disk_read(cluster_to_block(cluster),
                     (uint8_t)fs_sectors_per_cluster, buf);
}

// ---------------------------------------------------------------------------
// Free space.
// ---------------------------------------------------------------------------

// Find one free cluster and hand back its number, marking nothing: the caller
// claims it by writing its FAT entry. A cluster is free when its FAT entry is
// FAT32_CLUSTER_FREE (zero), so the same table that chains files is also the free
// list.
//
// Scanned a block at a time, not a cluster at a time. One 512-byte FAT block
// holds 128 entries, so reading the block and scanning it in memory costs one
// disk read per 128 clusters examined. A loop calling fat32_get_entry per cluster
// would read a whole block for each one, turning a scan of this volume into
// ~129000 reads; the same scan here is ~1009.
//
// The scan starts at fs_next_free_hint and wraps to the front exactly once, so a
// volume with a full prefix is not re-walked from cluster 2 every allocation. If
// it returns to where it began without finding a zero, the volume is full and
// this returns -1. Entries 0 and 1 are reserved (they hold a media descriptor and
// an end-of-chain marker, never free space), so the range scanned is the data
// clusters only, [FAT32_FIRST_DATA_CLUSTER, fs_total_clusters + 2).
static int find_free_cluster(uint32_t *out_cluster) {
    uint32_t first = FAT32_FIRST_DATA_CLUSTER;
    uint32_t limit = fs_total_clusters + FAT32_FIRST_DATA_CLUSTER;   // exclusive

    // A hint left out of range (never set, or pointing past a shrunk volume)
    // falls back to the first data cluster rather than skipping the scan.
    uint32_t start = fs_next_free_hint;
    if (start < first || start >= limit) {
        start = first;
    }

    uint32_t entries_per_block = fs_bytes_per_sector / FAT32_ENTRY_BYTES;
    uint8_t block_buf[DISK_SECTOR_SIZE];

    // Two spans cover the whole table starting at the hint and wrapping once:
    // [start, limit) then [first, start). If start == first the second span is
    // empty, so nothing is scanned twice.
    for (int pass = 0; pass < 2; pass++) {
        uint32_t lo = (pass == 0) ? start : first;
        uint32_t hi = (pass == 0) ? limit : start;

        uint32_t cluster = lo;
        while (cluster < hi) {
            uint32_t block = fs_first_fat_block + cluster / entries_per_block;
            if (disk_read(block, 1, block_buf) != 0) {
                return -1;
            }
            // Scan from this cluster's slot to the end of the block, in memory.
            for (uint32_t idx = cluster % entries_per_block;
                 idx < entries_per_block && cluster < hi; idx++, cluster++) {
                uint32_t offset = idx * FAT32_ENTRY_BYTES;
                uint32_t entry = (uint32_t)block_buf[offset] |
                                 ((uint32_t)block_buf[offset + 1] << 8) |
                                 ((uint32_t)block_buf[offset + 2] << 16) |
                                 ((uint32_t)block_buf[offset + 3] << 24);
                if ((entry & FAT32_ENTRY_MASK) == FAT32_CLUSTER_FREE) {
                    // Advance the hint past what we hand out, wrapping at the end.
                    fs_next_free_hint = (cluster + 1 < limit) ? cluster + 1 : first;
                    *out_cluster = cluster;
                    return 0;
                }
            }
        }
    }
    return -1;   // scanned the whole table without a free cluster: volume is full
}

// Count every free cluster on the volume. A full recount, deliberately: it walks
// the entire FAT rather than trusting any cached total, because its whole purpose
// is to be the independent check that catches cluster leaks. It is not on the
// allocation path (that is find_free_cluster with its hint), so its cost does not
// matter; it is still scanned a block at a time so the leak test does not freeze
// the machine for the length of ~129000 single-sector reads.
uint32_t fat32_free_count(void) {
    if (!fs_ready) {
        return 0;
    }
    uint32_t first = FAT32_FIRST_DATA_CLUSTER;
    uint32_t limit = fs_total_clusters + FAT32_FIRST_DATA_CLUSTER;   // exclusive
    uint32_t entries_per_block = fs_bytes_per_sector / FAT32_ENTRY_BYTES;
    uint8_t block_buf[DISK_SECTOR_SIZE];

    uint32_t free = 0;
    uint32_t cluster = first;
    while (cluster < limit) {
        uint32_t block = fs_first_fat_block + cluster / entries_per_block;
        if (disk_read(block, 1, block_buf) != 0) {
            return free;   // no error channel here; stop counting on a bad read
        }
        for (uint32_t idx = cluster % entries_per_block;
             idx < entries_per_block && cluster < limit; idx++, cluster++) {
            uint32_t offset = idx * FAT32_ENTRY_BYTES;
            uint32_t entry = (uint32_t)block_buf[offset] |
                             ((uint32_t)block_buf[offset + 1] << 8) |
                             ((uint32_t)block_buf[offset + 2] << 16) |
                             ((uint32_t)block_buf[offset + 3] << 24);
            if ((entry & FAT32_ENTRY_MASK) == FAT32_CLUSTER_FREE) {
                free++;
            }
        }
    }
    return free;
}

// ---------------------------------------------------------------------------
// Allocating and freeing chains.
// ---------------------------------------------------------------------------

// Free every cluster in the chain beginning at first_cluster, returning them all
// to the free list. Used both to delete a file and to unwind a half-built chain.
//
// Read the next pointer before zeroing the current entry. The FAT slot IS the
// only record of what comes next, so freeing (zeroing) it first would lose the
// rest of the chain irrecoverably.
//
// The walk is bounded exactly like walk_directory and the file read: no valid
// chain is longer than the volume's data-cluster count, so a walk that runs past
// that bound is following a cycle, and a filesystem that hangs the machine on a
// corrupt FAT is worse than one that reports an error. Freeing the same chain
// twice is worse than a leak (it marks live clusters free), so callers must never
// call this on a chain whose start they have already freed.
//
// Returns 0 on success, -1 on a read/write failure or a detected cycle.
static int free_chain(uint32_t first_cluster) {
    uint32_t cluster = first_cluster;
    uint32_t lowest = 0;
    int found_lowest = 0;

    uint32_t steps;
    for (steps = 0; steps <= fs_total_clusters; steps++) {
        if (!cluster_in_range(cluster)) {
            break;   // clean end: an end-of-chain marker, a free slot, or off-volume
        }
        // Next pointer first, then free this slot: reverse the order and the tail
        // is gone the instant the slot is zeroed.
        uint32_t next;
        if (fat32_get_entry(cluster, &next) != 0) {
            return -1;
        }
        if (fat32_set_entry(cluster, FAT32_CLUSTER_FREE) != 0) {
            return -1;
        }
        if (!found_lowest || cluster < lowest) {
            lowest = cluster;
            found_lowest = 1;
        }
        cluster = next;
    }

    if (steps > fs_total_clusters) {
        print_string("FAT32: chain to free exceeds volume size (cycle?)\n");
        return -1;
    }

    // Freed space near the front of the volume is the best place to allocate from
    // next, so pull the hint back to the lowest cluster just freed.
    if (found_lowest) {
        fs_next_free_hint = lowest;
    }
    return 0;
}

// Allocate a chain long enough to hold `bytes` and return its first cluster.
// Must not be called with bytes == 0: a zero-length file owns no clusters, and
// the caller handles that case without allocating.
//
// Each cluster is claimed the moment it is found — its FAT entry is written
// before the next find_free_cluster runs — so the same cluster can never be
// handed out twice within one allocation. Clusters are linked as they are taken
// and the last is marked end-of-chain.
//
// On running out of space partway, everything already allocated is freed and -1
// returned. A half-linked orphan chain left behind would be leaked space that no
// file owns, the same teardown discipline as task_create_from_file unwinding a
// partial address space.
static int alloc_chain(uint32_t bytes, uint32_t *out_first) {
    if (bytes == 0) {
        return -1;   // caller's contract: zero-length files allocate nothing
    }
    uint32_t needed = (bytes + fs_bytes_per_cluster - 1) / fs_bytes_per_cluster;

    uint32_t first = 0;
    uint32_t prev  = 0;
    for (uint32_t i = 0; i < needed; i++) {
        uint32_t cluster;
        if (find_free_cluster(&cluster) != 0) {
            if (first != 0) {
                free_chain(first);   // undo the partial chain, leak nothing
            }
            return -1;
        }
        // Claim it immediately as the new tail. Writing the entry now (rather than
        // after the loop) is what stops a later find_free_cluster in this same
        // allocation from returning it again: its slot is no longer zero.
        if (fat32_set_entry(cluster, FAT32_CLUSTER_END) != 0) {
            if (first != 0) {
                free_chain(first);
            }
            return -1;
        }
        if (prev != 0) {
            // Repoint the old tail at the new cluster, extending the chain.
            if (fat32_set_entry(prev, cluster) != 0) {
                free_chain(first);
                return -1;
            }
        } else {
            first = cluster;
        }
        prev = cluster;
    }

    *out_first = first;
    return 0;
}

// ---------------------------------------------------------------------------
// 8.3 names.
// ---------------------------------------------------------------------------

static char to_upper(char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

// Convert a caller's name ("HELLO.TXT") into the 11-byte on-disk form
// ("HELLO   TXT"): base name padded to 8 with spaces, extension padded to 3, no
// dot stored, uppercase. Returns 0 on success, -1 if the name cannot be
// expressed in 8.3 (too long a base or extension, or empty). A name this
// rejects is one TownOS cannot see at all, since long-filename entries are
// skipped when a directory is walked.
static int name_to_83(const char *name, char out[FAT32_NAME_LENGTH]) {
    memset(out, ' ', FAT32_NAME_LENGTH);   // the 8.3 padding is spaces, not NULs

    int i = 0;
    int written = 0;
    while (name[i] != '\0' && name[i] != '.') {
        if (written >= FAT32_BASE_LENGTH) {
            return -1;   // base name too long for 8.3
        }
        out[written++] = to_upper(name[i]);
        i++;
    }
    if (written == 0) {
        return -1;       // no base name at all
    }

    if (name[i] == '.') {
        i++;
        written = 0;
        while (name[i] != '\0') {
            if (written >= FAT32_EXT_LENGTH) {
                return -1;   // extension too long for 8.3
            }
            out[FAT32_BASE_LENGTH + written] = to_upper(name[i]);
            written++;
            i++;
        }
    }
    return 0;
}

// Format an on-disk 11-byte name back for display: trim the padding from each
// half and put the dot back. A name with an empty extension (directories usually
// have one) gets no trailing dot.
static void name_from_83(const uint8_t raw[FAT32_NAME_LENGTH],
                         char out[FAT32_DISPLAY_NAME_MAX]) {
    int written = 0;

    for (int i = 0; i < FAT32_BASE_LENGTH && raw[i] != ' '; i++) {
        out[written++] = (char)raw[i];
    }
    if (raw[FAT32_BASE_LENGTH] != ' ') {
        out[written++] = '.';
        for (int i = 0; i < FAT32_EXT_LENGTH; i++) {
            uint8_t c = raw[FAT32_BASE_LENGTH + i];
            if (c == ' ') {
                break;
            }
            out[written++] = (char)c;
        }
    }
    out[written] = '\0';
}

static int name_equals_83(const uint8_t raw[FAT32_NAME_LENGTH],
                          const char wanted[FAT32_NAME_LENGTH]) {
    // Fixed-length and not NUL-terminated, so strcmp does not apply; memcmp is the
    // compare for exactly this shape of field.
    return memcmp(raw, wanted, FAT32_NAME_LENGTH) == 0;
}

// The start cluster is split across two 16-bit fields at opposite ends of the
// entry. Recombine them; using only the low half silently works on small volumes
// and then breaks on large ones.
static uint32_t dirent_first_cluster(const struct fat32_dirent *entry) {
    return ((uint32_t)entry->first_cluster_high << 16) |
           (uint32_t)entry->first_cluster_low;
}

// Which entries are real files or directories, as opposed to format bookkeeping.
// The long-filename check must come first: an LFN entry's attribute byte has the
// volume-id bit set too, so testing that bit first would misclassify it.
static int dirent_is_usable(const struct fat32_dirent *entry) {
    if (entry->attr == FAT32_ATTR_LONG_NAME) {
        return 0;   // one fragment of a long filename, out of scope
    }
    if (entry->attr & FAT32_ATTR_VOLUME_ID) {
        return 0;   // the volume label, not a file
    }
    return 1;
}

// ---------------------------------------------------------------------------
// Walking a directory.
// ---------------------------------------------------------------------------
// A directory is a file, so reading one means following its cluster chain like
// any other. Its contents are a run of 32-byte entries.
//
// A sink for collecting file names during a directory walk, used by
// fat32_list_names (which backs SYS_LIST). When walk_directory is handed one of
// these, each usable entry's display name is appended followed by '\n' and the
// buffer is kept NUL-terminated, instead of being printed. Truncation is silent
// but safe: once a name plus its separator and terminator would not fit, that name
// and every one after it is dropped, and `count` reflects only the names written.
struct fat32_name_sink {
    char    *buf;
    uint32_t size;    // capacity of buf in bytes, including the terminating NUL
    uint32_t used;    // bytes written so far, not counting the terminating NUL
    uint32_t count;   // names actually written into the buffer
};

// Append one name plus a '\n' to the sink, keeping it NUL-terminated. Drops the
// name if it would not fit with its separator and terminator (see the sink above).
static void fat32_name_sink_add(struct fat32_name_sink *sink, const char *name) {
    uint32_t len = 0;
    while (name[len] != '\0') {
        len++;
    }
    // Room needed: len name bytes, one '\n', one '\0'. Reject rather than overrun.
    if (sink->used + len + 2 > sink->size) {
        return;
    }
    memcpy(sink->buf + sink->used, name, len);
    sink->used += len;
    sink->buf[sink->used++] = '\n';
    sink->buf[sink->used] = '\0';   // valid C string after every append
    sink->count++;
}

// Both callers below (listing and lookup) share this walk. When wanted is NULL it
// visits every usable entry: appending each name to `sink` if one is given, else
// printing it. When wanted is non-NULL it stops at the first entry whose name
// matches and copies it to found. Returns 0 on success (for a lookup, 0 means
// found), -1 on a read error or a corrupt chain, and 1 when a lookup completed
// without finding the name.
#define FAT32_DIR_NOT_FOUND 1

static int walk_directory(uint32_t dir_cluster,
                          const char *wanted,
                          struct fat32_dirent *found,
                          struct fat32_name_sink *sink) {
    uint8_t *cluster_buf = (uint8_t *)kmalloc(fs_bytes_per_cluster);
    if (cluster_buf == NULL) {
        return -1;
    }

    uint32_t cluster = dir_cluster;
    int result = wanted ? FAT32_DIR_NOT_FOUND : 0;

    // Bounded, deliberately. A corrupt or self-referential chain would otherwise
    // spin here forever, and a filesystem that hangs the machine on bad data is
    // worse than one that reports an error. No valid chain can be longer than
    // the number of data clusters on the volume, so running past that bound is
    // proof of corruption (checked after the loop). Same reasoning as the disk
    // driver's bounded polling loops.
    uint32_t steps;
    for (steps = 0; steps <= fs_total_clusters; steps++) {
        if (read_cluster(cluster, cluster_buf) != 0) {
            result = -1;
            break;
        }

        uint32_t entries = fs_bytes_per_cluster / FAT32_DIRENT_BYTES;
        int done = 0;

        for (uint32_t i = 0; i < entries; i++) {
            struct fat32_dirent *entry =
                (struct fat32_dirent *)(cluster_buf + i * FAT32_DIRENT_BYTES);

            if (entry->name[0] == FAT32_DIRENT_FREE) {
                // Never-used entry: everything after it is unused too, so this
                // is the end of the directory, not just a gap.
                done = 1;
                break;
            }
            if (entry->name[0] == FAT32_DIRENT_DELETED) {
                continue;   // a hole left by a deleted file, keep scanning
            }
            if (!dirent_is_usable(entry)) {
                continue;
            }

            if (wanted) {
                if (name_equals_83(entry->name, wanted)) {
                    *found = *entry;
                    result = 0;
                    done = 1;
                    break;
                }
                continue;
            }

            char display[FAT32_DISPLAY_NAME_MAX];
            name_from_83(entry->name, display);
            if (sink) {
                fat32_name_sink_add(sink, display);
            } else {
                print_string(display);
                if (entry->attr & FAT32_ATTR_DIRECTORY) {
                    print_string("  <DIR>\n");
                } else {
                    print_string("  ");
                    print_int(entry->size);
                    print_string(" bytes\n");
                }
            }
        }

        if (done) {
            break;
        }

        uint32_t next;
        int step = fat32_next_cluster(cluster, &next);
        if (step == FAT32_CHAIN_END) {
            break;                  // directory ended on a cluster boundary
        }
        if (step == FAT32_CHAIN_ERROR) {
            result = -1;
            break;
        }
        cluster = next;
    }

    if (steps > fs_total_clusters) {
        // Followed more clusters than the volume holds, so the chain loops.
        print_string("FAT32: directory chain exceeds volume size\n");
        result = -1;
    }

    kfree(cluster_buf);
    return result;
}

int fat32_list_root(void) {
    if (!fs_ready) {
        print_string("FAT32: not initialised\n");
        return -1;
    }
    return walk_directory(fs_root_cluster, NULL, NULL, NULL);
}

// Buffer-filling sibling of fat32_list_root: instead of printing, collect the root
// directory's names into buf, one per line, NUL-terminated. This is what SYS_LIST
// hands back to a ring-3 program, which cannot see the screen the print path
// writes to. Names that do not all fit are dropped from the end (see the sink).
int fat32_list_names(char *buf, uint32_t bufsize, uint32_t *out_count) {
    if (!fs_ready || buf == NULL || bufsize == 0) {
        return -1;
    }
    struct fat32_name_sink sink = { buf, bufsize, 0, 0 };
    buf[0] = '\0';   // an empty directory yields an empty string, not garbage
    if (walk_directory(fs_root_cluster, NULL, NULL, &sink) != 0) {
        return -1;
    }
    if (out_count) {
        *out_count = sink.count;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Finding a file.
// ---------------------------------------------------------------------------

// Look up one name in the root directory and copy its entry out. Shared by
// fat32_stat and fat32_read_file so there is exactly one lookup path.
//
// Root directory only. The walk itself takes any starting cluster and would read
// a subdirectory's entries just as happily, but this interface takes a bare name
// with no path to split, so there is no way to say which directory. Path lookup
// and subdirectories are future work.
//
// Returns 0 on success, -1 if the filesystem is not ready, the name is not
// expressible in 8.3, the name is not present, or the entry is a directory
// rather than a file.
static int lookup_root_file(const char *name, struct fat32_dirent *out) {
    if (!fs_ready) {
        return -1;
    }

    char wanted[FAT32_NAME_LENGTH];
    if (name_to_83(name, wanted) != 0) {
        return -1;   // not expressible in 8.3, so nothing on disk can match it
    }

    if (walk_directory(fs_root_cluster, wanted, out, NULL) != 0) {
        return -1;   // not found, or the directory could not be read
    }
    if (out->attr & FAT32_ATTR_DIRECTORY) {
        return -1;   // a directory's contents are entries, not file bytes
    }
    return 0;
}

int fat32_stat(const char *name, uint32_t *out_size) {
    // Answers "how big is this file" without reading a byte of it. A caller that
    // wants a file's contents has to allocate a buffer first, which means knowing
    // the size first, and the only honest alternative (a fixed buffer that is
    // assumed to be big enough) is a size limit waiting to be exceeded quietly.
    // The size lives in the directory entry, so this walk costs the same as a
    // lookup and touches no file data at all.
    struct fat32_dirent entry;
    if (lookup_root_file(name, &entry) != 0) {
        return -1;
    }
    if (out_size) {
        *out_size = entry.size;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Reading a file.
// ---------------------------------------------------------------------------

int fat32_read_file(const char *name, void *buf, uint32_t bufsize,
                    uint32_t *out_size) {
    struct fat32_dirent entry;
    if (lookup_root_file(name, &entry) != 0) {
        return -1;
    }

    // The chain says which clusters hold the file; the directory entry's size is
    // what says where the real data ends inside the last one.
    uint32_t size = entry.size;
    if (size > bufsize) {
        return -1;
    }
    if (out_size) {
        *out_size = size;
    }
    if (size == 0) {
        return 0;    // an empty file may not have a start cluster at all
    }

    uint32_t cluster = dirent_first_cluster(&entry);
    if (!cluster_in_range(cluster)) {
        return -1;
    }

    // Heap, not stack: a cluster can be far larger than a block (up to 128KB at
    // the format's maximum), and the kernel stack is not big.
    uint8_t *cluster_buf = (uint8_t *)kmalloc(fs_bytes_per_cluster);
    if (cluster_buf == NULL) {
        return -1;
    }

    uint8_t *out = (uint8_t *)buf;
    uint32_t remaining = size;
    int result = 0;

    // Bounded for the same reason walk_directory is: a looping chain must fail,
    // not hang. Delivering `size` bytes normally ends this loop long before the
    // bound is reached.
    uint32_t steps;
    for (steps = 0; steps <= fs_total_clusters && remaining > 0; steps++) {
        if (read_cluster(cluster, cluster_buf) != 0) {
            result = -1;
            break;
        }

        // Copy whole clusters until the last one, which is usually only partly
        // real data. Trimming to `remaining` is what keeps the stale bytes after
        // the end of the file (whatever occupied those blocks before) out of the
        // caller's buffer.
        uint32_t chunk = remaining < fs_bytes_per_cluster ? remaining
                                                          : fs_bytes_per_cluster;
        memcpy(out, cluster_buf, chunk);
        out += chunk;
        remaining -= chunk;

        if (remaining == 0) {
            break;   // satisfied the size, so the rest of the chain is padding
        }

        uint32_t next;
        int step = fat32_next_cluster(cluster, &next);
        if (step != FAT32_CHAIN_NEXT) {
            // Either the chain ended early (size claims more data than the
            // chain holds) or an entry was unreadable. Both mean the volume
            // disagrees with itself, so refuse rather than return a short read
            // the caller would mistake for the whole file.
            result = -1;
            break;
        }
        cluster = next;
    }

    if (remaining > 0 && result == 0) {
        result = -1;   // ran past the cluster bound: the chain loops
    }

    kfree(cluster_buf);
    return result;
}

// ---------------------------------------------------------------------------
// Writing: directory entries.
// ---------------------------------------------------------------------------
// Everything below is the write side. It sits after the read code because it
// leans on the same primitives (read_cluster, cluster_to_block, the dirent
// helpers) and adds the ability to change what they read.

// Find `wanted` (an 11-byte 8.3 name) in the root directory and report not just
// the entry but where on disk it lives, so a caller can rewrite it in place. This
// is the lookup twin of walk_directory's search mode; it is kept separate rather
// than folded in because only the write path needs a slot's disk location, and
// threading that through the shared read walk would complicate the paths that do
// not. Returns 0 with *out/*out_block/*out_offset set on a match,
// FAT32_DIR_NOT_FOUND if the name is absent, -1 on a read error or corrupt chain.
static int locate_root_entry(const char wanted[FAT32_NAME_LENGTH],
                             struct fat32_dirent *out,
                             uint32_t *out_block, uint32_t *out_offset) {
    uint8_t *cluster_buf = (uint8_t *)kmalloc(fs_bytes_per_cluster);
    if (cluster_buf == NULL) {
        return -1;
    }

    uint32_t entries = fs_bytes_per_cluster / FAT32_DIRENT_BYTES;
    uint32_t cluster = fs_root_cluster;
    int result = FAT32_DIR_NOT_FOUND;

    uint32_t steps;
    for (steps = 0; steps <= fs_total_clusters; steps++) {
        if (read_cluster(cluster, cluster_buf) != 0) {
            result = -1;
            break;
        }

        int done = 0;
        for (uint32_t i = 0; i < entries; i++) {
            struct fat32_dirent *entry =
                (struct fat32_dirent *)(cluster_buf + i * FAT32_DIRENT_BYTES);

            if (entry->name[0] == FAT32_DIRENT_FREE) {
                done = 1;   // end of directory: nothing usable follows
                break;
            }
            if (entry->name[0] == FAT32_DIRENT_DELETED || !dirent_is_usable(entry)) {
                continue;
            }
            if (name_equals_83(entry->name, wanted)) {
                *out = *entry;
                uint32_t byte = i * FAT32_DIRENT_BYTES;
                *out_block  = cluster_to_block(cluster) + byte / fs_bytes_per_sector;
                *out_offset = byte % fs_bytes_per_sector;
                result = 0;
                done = 1;
                break;
            }
        }
        if (done) {
            break;
        }

        uint32_t next;
        int step = fat32_next_cluster(cluster, &next);
        if (step == FAT32_CHAIN_END) {
            break;
        }
        if (step == FAT32_CHAIN_ERROR) {
            result = -1;
            break;
        }
        cluster = next;
    }

    if (steps > fs_total_clusters) {
        print_string("FAT32: directory chain exceeds volume size\n");
        result = -1;
    }

    kfree(cluster_buf);
    return result;
}

// Find a directory slot that can hold a new entry, and report its disk location
// as a (block, offset-within-block) pair, the form write_dirent_at wants. A slot
// is available when its first name byte is 0x00 (never used) or 0xE5 (deleted).
//
// If the whole root directory is full, grow it: a directory is a file, so this is
// alloc_chain for one cluster plus one link onto the end of the chain, not new
// machinery. The new cluster MUST be zero-filled first: an un-zeroed cluster
// holds whatever used to be there, and every 32-byte window in it would be read
// as a bogus directory entry. The free slot is then the first entry of it.
//
// Returns 0 with *out_block/*out_offset set, -1 on any read/write/alloc failure
// or a corrupt directory chain.
static int find_free_dirent(uint32_t *out_block, uint32_t *out_offset) {
    uint8_t *cluster_buf = (uint8_t *)kmalloc(fs_bytes_per_cluster);
    if (cluster_buf == NULL) {
        return -1;
    }

    uint32_t entries = fs_bytes_per_cluster / FAT32_DIRENT_BYTES;
    uint32_t cluster = fs_root_cluster;
    uint32_t last_cluster = cluster;
    int grow = 0;

    uint32_t steps;
    for (steps = 0; steps <= fs_total_clusters; steps++) {
        last_cluster = cluster;
        if (read_cluster(cluster, cluster_buf) != 0) {
            kfree(cluster_buf);
            return -1;
        }

        for (uint32_t i = 0; i < entries; i++) {
            uint8_t first = cluster_buf[i * FAT32_DIRENT_BYTES];
            if (first == FAT32_DIRENT_FREE || first == FAT32_DIRENT_DELETED) {
                uint32_t byte = i * FAT32_DIRENT_BYTES;
                *out_block  = cluster_to_block(cluster) + byte / fs_bytes_per_sector;
                *out_offset = byte % fs_bytes_per_sector;
                kfree(cluster_buf);
                return 0;
            }
        }

        uint32_t next;
        int step = fat32_next_cluster(cluster, &next);
        if (step == FAT32_CHAIN_END) {
            grow = 1;   // every existing slot is taken; extend the directory
            break;
        }
        if (step == FAT32_CHAIN_ERROR) {
            kfree(cluster_buf);
            return -1;
        }
        cluster = next;
    }
    kfree(cluster_buf);

    if (!grow) {
        // Fell out of the loop by the cluster bound, not by reaching the end.
        print_string("FAT32: directory chain exceeds volume size\n");
        return -1;
    }

    // Grow: one new cluster, zero-filled, linked onto the end of the chain.
    uint32_t new_cluster;
    if (alloc_chain(fs_bytes_per_cluster, &new_cluster) != 0) {
        return -1;
    }

    uint8_t *zero = (uint8_t *)kmalloc(fs_bytes_per_cluster);
    if (zero == NULL) {
        free_chain(new_cluster);
        return -1;
    }
    memset(zero, 0, fs_bytes_per_cluster);
    // One cluster is one disk_write by construction (see read_cluster).
    if (disk_write(cluster_to_block(new_cluster),
                   (uint8_t)fs_sectors_per_cluster, zero) != 0) {
        kfree(zero);
        free_chain(new_cluster);
        return -1;
    }
    kfree(zero);

    // Link the old tail to the new cluster only after it is safely zeroed, so a
    // failure above never publishes a garbage-filled cluster into the directory.
    if (fat32_set_entry(last_cluster, new_cluster) != 0) {
        free_chain(new_cluster);
        return -1;
    }

    *out_block  = cluster_to_block(new_cluster);
    *out_offset = 0;
    return 0;
}

// Write a 32-byte directory entry into an existing slot. Read-modify-write,
// because the disk's unit is a 512-byte block and an entry is smaller than one:
// read the block, splice the entry in at its offset, write the block back.
// Returns 0 on success, -1 on a read or write failure.
static int write_dirent_at(uint32_t block, uint32_t offset,
                           const struct fat32_dirent *entry) {
    uint8_t block_buf[DISK_SECTOR_SIZE];
    if (disk_read(block, 1, block_buf) != 0) {
        return -1;
    }
    memcpy(block_buf + offset, entry, FAT32_DIRENT_BYTES);
    if (disk_write(block, 1, block_buf) != 0) {
        return -1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// FSInfo.
// ---------------------------------------------------------------------------

// Mark the FSInfo sector's cached free-cluster count and next-free hint as
// unknown, so nothing downstream trusts a value our writes have just made stale.
// This is called once per write or delete (batched, not once per cluster).
//
// It verifies all three FSInfo signatures before writing a single byte. The
// sector number was taken from the boot sector, which could be corrupt; if the
// signatures do not all match, this is not an FSInfo sector and gets left
// entirely alone rather than overwritten on a bad guess. Best effort throughout:
// a stale FSInfo is a performance hint at worst, never a correctness problem, so a
// read or write failure here is swallowed rather than failing the whole operation.
static void fat32_fsinfo_invalidate(void) {
    if (fs_fsinfo_block == 0 || fs_fsinfo_block == 0xFFFF) {
        return;   // the BPB declares no FSInfo sector (0 and 0xFFFF both mean none)
    }

    uint8_t block_buf[DISK_SECTOR_SIZE];
    if (disk_read(fs_fsinfo_block, 1, block_buf) != 0) {
        return;
    }

    uint32_t lead = (uint32_t)block_buf[FAT32_FSINFO_LEAD_SIG_OFFSET] |
        ((uint32_t)block_buf[FAT32_FSINFO_LEAD_SIG_OFFSET + 1] << 8) |
        ((uint32_t)block_buf[FAT32_FSINFO_LEAD_SIG_OFFSET + 2] << 16) |
        ((uint32_t)block_buf[FAT32_FSINFO_LEAD_SIG_OFFSET + 3] << 24);
    uint32_t struc = (uint32_t)block_buf[FAT32_FSINFO_STRUCT_SIG_OFFSET] |
        ((uint32_t)block_buf[FAT32_FSINFO_STRUCT_SIG_OFFSET + 1] << 8) |
        ((uint32_t)block_buf[FAT32_FSINFO_STRUCT_SIG_OFFSET + 2] << 16) |
        ((uint32_t)block_buf[FAT32_FSINFO_STRUCT_SIG_OFFSET + 3] << 24);
    uint32_t trail = (uint32_t)block_buf[FAT32_FSINFO_TRAIL_SIG_OFFSET] |
        ((uint32_t)block_buf[FAT32_FSINFO_TRAIL_SIG_OFFSET + 1] << 8) |
        ((uint32_t)block_buf[FAT32_FSINFO_TRAIL_SIG_OFFSET + 2] << 16) |
        ((uint32_t)block_buf[FAT32_FSINFO_TRAIL_SIG_OFFSET + 3] << 24);

    if (lead != FAT32_FSINFO_LEAD_SIG || struc != FAT32_FSINFO_STRUCT_SIG ||
        trail != FAT32_FSINFO_TRAIL_SIG) {
        return;   // not an FSInfo sector: do not write into it
    }

    // Both fields to 0xFFFFFFFF. That is eight contiguous 0xFF bytes covering the
    // free count at offset 488 and the next-free hint at 492.
    memset(block_buf + FAT32_FSINFO_FREE_COUNT_OFFSET, 0xFF, 8);
    disk_write(fs_fsinfo_block, 1, block_buf);   // best effort; a failure is harmless
}

// ---------------------------------------------------------------------------
// Deleting a file.
// ---------------------------------------------------------------------------

int fat32_delete(const char *name) {
    if (!fs_ready) {
        return -1;
    }

    char wanted[FAT32_NAME_LENGTH];
    if (name_to_83(name, wanted) != 0) {
        return -1;   // not expressible in 8.3, so nothing on disk can match it
    }

    struct fat32_dirent entry;
    uint32_t block, offset;
    if (locate_root_entry(wanted, &entry, &block, &offset) != 0) {
        return -1;   // not found, or the directory could not be read
    }
    if (entry.attr & FAT32_ATTR_DIRECTORY) {
        return -1;   // deleting a subdirectory (and its contents) is out of scope
    }

    // Order is the exact opposite of creation, and deliberately so. Free the data
    // first, then unpublish the name. A crash in between leaves a lost chain,
    // which is only wasted space. The other order would leave a live directory
    // entry pointing at freed clusters, which is corruption: a later file could be
    // handed those same clusters while this entry still claims them.
    uint32_t start = dirent_first_cluster(&entry);
    if (start != 0) {   // a zero-length file owns no clusters, so skip the free
        if (free_chain(start) != 0) {
            return -1;
        }
    }

    entry.name[0] = FAT32_DIRENT_DELETED;
    if (write_dirent_at(block, offset, &entry) != 0) {
        return -1;
    }

    // The free-cluster picture changed, so the FSInfo cache is now stale. One
    // invalidation for the whole delete, after the chain has been freed.
    fat32_fsinfo_invalidate();
    return 0;
}

// ---------------------------------------------------------------------------
// Writing a file.
// ---------------------------------------------------------------------------

// Create or wholly replace a file. The order of the seven steps below is the most
// important thing in this function, and it is not arbitrary: it is what makes a
// replacement crash-safe.
//
//   1. Convert the name to 8.3; reject it here if it will not fit.
//   2. Look the name up. Remember whether it already exists, and if so its old
//      start cluster and the disk location of its directory slot. Free NOTHING.
//   3. Allocate a fresh chain for the new contents (skipped for a 0-byte file).
//      If the volume is full this fails now, having changed nothing visible.
//   4. Write the data into the new clusters, zero-filling the last cluster past
//      len so no previous file's bytes leak into the slack.
//   5. Build the new directory entry pointing at the new chain.
//   6. Write that entry — reusing the old slot if the file existed, else a free
//      one. THIS SINGLE WRITE IS THE COMMIT POINT.
//   7. Only now free the old chain, if there was one.
//
// Why this order. Everything before step 6 is invisible: the new clusters are
// allocated and filled but no directory entry names them, and the old file (if
// any) is still completely intact. The one directory-entry write in step 6 flips
// the name from the old contents to the new, atomically as far as a reader is
// concerned — a directory entry fits in one 512-byte block, so the disk writes it
// whole or not at all. A crash before step 6 loses only the new, unreferenced
// clusters (garbage the next format or a scan reclaims); a crash after it has
// already fully succeeded. There is no instant where the name resolves to a
// half-written file. Freeing the old chain last (step 7, not step 3) is the other
// half of the same guarantee: the old data must outlive the commit, because until
// the commit it is still the file. The cost is that a replacement briefly needs
// room for both the old and the new chain at once; that is a deliberate trade
// (see docs/decisions/0020-writable-fat32.md).
int fat32_write_file(const char *name, const void *buf, uint32_t len) {
    if (!fs_ready) {
        return -1;
    }

    // Step 1: the name must be expressible in 8.3, or nothing on disk could name
    // this file anyway.
    char name83[FAT32_NAME_LENGTH];
    if (name_to_83(name, name83) != 0) {
        return -1;
    }

    // Step 2: does it already exist? Keep its slot location and old start cluster,
    // but free nothing yet — the old file has to stay whole until the new commits.
    struct fat32_dirent existing;
    uint32_t slot_block = 0, slot_offset = 0, old_start = 0;
    int exists = 0;
    int found = locate_root_entry(name83, &existing, &slot_block, &slot_offset);
    if (found == 0) {
        if (existing.attr & FAT32_ATTR_DIRECTORY) {
            return -1;   // refuse to replace a directory with a file
        }
        exists = 1;
        old_start = dirent_first_cluster(&existing);
    } else if (found != FAT32_DIR_NOT_FOUND) {
        return -1;   // a read error or corrupt directory, not a plain "absent"
    }

    // Step 3: allocate the new chain. A zero-length file owns no clusters, so its
    // start cluster stays 0 and nothing is allocated. Out of space fails here,
    // with the old file still untouched.
    uint32_t new_start = 0;
    if (len > 0) {
        if (alloc_chain(len, &new_start) != 0) {
            return -1;
        }
    }

    // Step 4: write the data into the new clusters, one cluster at a time, zero-
    // filling the tail of the last cluster past len. A reader never sees those
    // trailing bytes (the size field stops it), but leaving a previous file's
    // contents there would be an information leak, and it costs one memset.
    if (len > 0) {
        uint8_t *cluster_buf = (uint8_t *)kmalloc(fs_bytes_per_cluster);
        if (cluster_buf == NULL) {
            free_chain(new_start);
            return -1;
        }
        const uint8_t *src = (const uint8_t *)buf;
        uint32_t remaining = len;
        uint32_t cluster = new_start;
        int failed = 0;

        while (remaining > 0) {
            uint32_t chunk = remaining < fs_bytes_per_cluster ? remaining
                                                              : fs_bytes_per_cluster;
            memcpy(cluster_buf, src, chunk);
            if (chunk < fs_bytes_per_cluster) {
                memset(cluster_buf + chunk, 0, fs_bytes_per_cluster - chunk);
            }
            if (disk_write(cluster_to_block(cluster),
                           (uint8_t)fs_sectors_per_cluster, cluster_buf) != 0) {
                failed = 1;
                break;
            }
            src += chunk;
            remaining -= chunk;
            if (remaining == 0) {
                break;
            }
            uint32_t next;
            if (fat32_next_cluster(cluster, &next) != FAT32_CHAIN_NEXT) {
                failed = 1;   // chain shorter than its data: alloc_chain is broken
                break;
            }
            cluster = next;
        }
        kfree(cluster_buf);
        if (failed) {
            free_chain(new_start);
            return -1;
        }
    }

    // Step 5: build the entry. Archive attribute, size = len, start cluster split
    // across its two 16-bit halves (both 0 for a zero-length file). The rest,
    // including the create/write date and time fields, is left zero: TownOS keeps
    // no clock to stamp them with (see docs/decisions/0020-writable-fat32.md).
    struct fat32_dirent entry;
    memset(&entry, 0, sizeof(entry));
    memcpy(entry.name, name83, FAT32_NAME_LENGTH);
    entry.attr = FAT32_ATTR_ARCHIVE;
    entry.first_cluster_high = (uint16_t)(new_start >> 16);
    entry.first_cluster_low  = (uint16_t)(new_start & 0xFFFF);
    entry.size = len;

    // Step 6: publish. Reuse the old slot if the file existed, otherwise find a
    // free one (growing the directory if it is full). This write is the commit
    // point. If it cannot be placed, reclaim the new chain and fail — nothing
    // visible changed.
    uint32_t block, offset;
    if (exists) {
        block = slot_block;
        offset = slot_offset;
    } else if (find_free_dirent(&block, &offset) != 0) {
        if (new_start != 0) {
            free_chain(new_start);
        }
        return -1;
    }
    if (write_dirent_at(block, offset, &entry) != 0) {
        if (new_start != 0) {
            free_chain(new_start);
        }
        return -1;
    }

    // Step 7: the new entry is on disk, so the old chain is finally safe to free.
    // Doing it here and not at step 3 is what kept the old file intact across the
    // whole operation.
    if (exists && old_start != 0) {
        free_chain(old_start);
    }

    // Allocation and/or freeing changed the free-cluster picture, so the FSInfo
    // cache is stale. Invalidate once here, not once per cluster touched above.
    fat32_fsinfo_invalidate();
    return 0;
}

#define _XOPEN_SOURCE 500
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define FS_MAGIC 0x56534653U
#define JOURNAL_MAGIC 0x4A524E4CU

#define BLOCK_SIZE        4096U
#define INODE_SIZE         128U
#define JOURNAL_BLOCK_IDX    1U
#define JOURNAL_BLOCKS      16U
#define INODE_BLOCKS         2U
#define DATA_BLOCKS         64U
#define INODE_BMAP_IDX     (JOURNAL_BLOCK_IDX + JOURNAL_BLOCKS)
#define DATA_BMAP_IDX      (INODE_BMAP_IDX + 1U)
#define INODE_START_IDX    (DATA_BMAP_IDX + 1U)
#define DATA_START_IDX     (INODE_START_IDX + INODE_BLOCKS)
#define TOTAL_BLOCKS       (DATA_START_IDX + DATA_BLOCKS)
#define DEFAULT_IMAGE "vsfs.img"

#define REC_DATA   1
#define REC_COMMIT 2

#define INODES_PER_BLOCK   (BLOCK_SIZE / INODE_SIZE)
#define JOURNAL_BYTES      (JOURNAL_BLOCKS * BLOCK_SIZE)

#define MAX_DIRTY_BLOCKS     8   /* bitmap + up to 2 inode blocks + dir block, with room to spare */
#define MAX_PENDING_RECORDS 16   /* DATA records buffered within one in-flight transaction */

struct superblock {
    uint32_t magic;
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t inode_count;
    uint32_t journal_block;
    uint32_t inode_bitmap;
    uint32_t data_bitmap;
    uint32_t inode_start;
    uint32_t data_start;
    uint8_t  _pad[128 - 9 * 4];
};

struct inode {
    uint16_t type;
    uint16_t links;
    uint32_t size;
    uint32_t direct[8];
    uint32_t ctime;
    uint32_t mtime;
    uint8_t _pad[128 - (2 + 2 + 4 + 8 * 4 + 4 + 4)];
};

struct dirent {
    uint32_t inode;
    char name[28];
};

struct journal_header {
    uint32_t magic;
    uint32_t nbytes_used; /* total bytes used in the journal, HEADER INCLUDED */
};

struct rec_header {
    uint16_t type;
    uint16_t size; /* total size of this record, header included */
};

_Static_assert(sizeof(struct superblock) == 128, "superblock must be 128 bytes");
_Static_assert(sizeof(struct inode) == 128, "inode must be 128 bytes");
_Static_assert(sizeof(struct dirent) == 32, "dirent must be 32 bytes");
_Static_assert(sizeof(struct journal_header) == 8, "journal header must be 8 bytes");
_Static_assert(sizeof(struct rec_header) == 4, "record header must be 4 bytes");

#define JOURNAL_DATA_CAPACITY (JOURNAL_BYTES - sizeof(struct journal_header))

static void die(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

static void pread_block(int fd, uint32_t block_index, void *buf) {
    off_t offset = (off_t)block_index * BLOCK_SIZE;
    ssize_t n = pread(fd, buf, BLOCK_SIZE, offset);
    if (n != (ssize_t)BLOCK_SIZE) {
        die("pread");
    }
}

static void pwrite_block(int fd, uint32_t block_index, const void *buf) {
    off_t offset = (off_t)block_index * BLOCK_SIZE;
    ssize_t n = pwrite(fd, buf, BLOCK_SIZE, offset);
    if (n != (ssize_t)BLOCK_SIZE) {
        die("pwrite");
    }
}

static int find_free_bit(const uint8_t *bitmap, uint32_t max_bits) {
    for (uint32_t i = 0; i < max_bits; i++) {
        if (!(bitmap[i / 8] & (1U << (i % 8)))) {
            return (int)i;
        }
    }
    return -1;
}

static void set_bit(uint8_t *bitmap, uint32_t bit) {
    bitmap[bit / 8] |= (uint8_t)(1U << (bit % 8));
}

typedef struct {
    uint32_t block_no;
    uint8_t  data[BLOCK_SIZE];
} dirty_block_t;

/*
 * Walk the journal's data area (everything after the header) and, for every
 * *complete* transaction (a run of DATA records terminated by a COMMIT
 * record that is itself fully within data_len), invoke `apply` on each of
 * that transaction's DATA records in order.
 *
 * A transaction whose COMMIT record is missing or truncated is never
 * applied - its pending DATA records are simply discarded. This is what
 * makes replay crash-safe: a torn/partial transaction has no effect.
 */
typedef void (*record_apply_fn)(uint32_t block_no, const uint8_t *data, void *ctx);

static void replay_journal(const uint8_t *journal_data, uint32_t data_len,
                            record_apply_fn apply, void *ctx) {
    uint32_t offset = 0;
    uint32_t pending[MAX_PENDING_RECORDS];
    int npending = 0;

    while (offset + sizeof(struct rec_header) <= data_len) {
        struct rec_header rh;
        memcpy(&rh, journal_data + offset, sizeof(rh));

        if (rh.size < sizeof(rh) || offset + rh.size > data_len) {
            break; /* truncated / corrupt record: nothing further is trustworthy */
        }

        if (rh.type == REC_DATA) {
            if (npending < MAX_PENDING_RECORDS) {
                pending[npending++] = offset;
            }
        } else if (rh.type == REC_COMMIT) {
            for (int k = 0; k < npending; k++) {
                uint32_t off = pending[k];
                uint32_t block_no;
                memcpy(&block_no, journal_data + off + sizeof(struct rec_header), 4);
                const uint8_t *data = journal_data + off + sizeof(struct rec_header) + 4;
                apply(block_no, data, ctx);
            }
            npending = 0;
        } else {
            break; /* unknown record type: stop, don't guess */
        }

        offset += rh.size;
    }
    /* Any DATA records still pending here belong to an uncommitted
       transaction and are intentionally dropped. */
}

/* --- apply callback used by journal_install: write straight to disk --- */
static void apply_to_disk(uint32_t block_no, const uint8_t *data, void *ctx_) {
    int fd = *(int *)ctx_;
    pwrite_block(fd, block_no, data);
}

/* --- apply callback used by journal_create: fold into an in-memory
   "effective state" so that several creates queued before an install
   build on top of each other instead of colliding --- */
typedef struct {
    uint32_t inode_bitmap_blk;
    uint8_t *inode_bitmap;
    uint32_t inode_block_no[INODE_BLOCKS];
    uint8_t (*inode_blocks)[BLOCK_SIZE];
    uint32_t dir_block_no;
    uint8_t *dir_block;
} effective_ctx_t;

static void apply_to_effective_state(uint32_t block_no, const uint8_t *data, void *ctx_) {
    effective_ctx_t *ctx = ctx_;
    if (block_no == ctx->inode_bitmap_blk) {
        memcpy(ctx->inode_bitmap, data, BLOCK_SIZE);
        return;
    }
    for (uint32_t i = 0; i < INODE_BLOCKS; i++) {
        if (block_no == ctx->inode_block_no[i]) {
            memcpy(ctx->inode_blocks[i], data, BLOCK_SIZE);
            return;
        }
    }
    if (block_no == ctx->dir_block_no) {
        memcpy(ctx->dir_block, data, BLOCK_SIZE);
        return;
    }
    /* Not a block we're tracking - ignore. */
}

static void load_journal(int fd, const struct superblock *sb, uint8_t *journal /* JOURNAL_BYTES */) {
    for (uint32_t i = 0; i < JOURNAL_BLOCKS; i++) {
        pread_block(fd, sb->journal_block + i, journal + i * BLOCK_SIZE);
    }
}

static void write_journal(int fd, const struct superblock *sb, const uint8_t *journal) {
    for (uint32_t i = 0; i < JOURNAL_BLOCKS; i++) {
        pwrite_block(fd, sb->journal_block + i, journal + i * BLOCK_SIZE);
    }
}

static int journal_create(const char *filename) {
    if (filename[0] == '\0') {
        fprintf(stderr, "Filename must not be empty\n");
        return 1;
    }
    if (strlen(filename) > 27) {
        fprintf(stderr, "Filename too long (max 27 characters)\n");
        return 1;
    }

    int fd = open(DEFAULT_IMAGE, O_RDWR);
    if (fd < 0) {
        die("open");
    }

    struct superblock sb;
    pread_block(fd, 0, &sb);
    if (sb.magic != FS_MAGIC) {
        fprintf(stderr, "'%s' is not a valid VSFS image\n", DEFAULT_IMAGE);
        close(fd);
        return 1;
    }

    /* Home-location snapshots of every metadata block we might touch. */
    uint8_t inode_bitmap[BLOCK_SIZE];
    pread_block(fd, sb.inode_bitmap, inode_bitmap);

    uint8_t inode_blocks[INODE_BLOCKS][BLOCK_SIZE];
    uint32_t inode_block_no[INODE_BLOCKS];
    for (uint32_t i = 0; i < INODE_BLOCKS; i++) {
        inode_block_no[i] = sb.inode_start + i;
        pread_block(fd, inode_block_no[i], inode_blocks[i]);
    }

    /* Root's data pointer is set once by mkfs and never moves, so it's safe
       to read it from the home copy even before folding in pending journal
       transactions. */
    struct inode *root_home = (struct inode *)inode_blocks[0];
    uint32_t dir_block_no = root_home[0].direct[0];
    if (dir_block_no == 0) {
        fprintf(stderr, "Root directory has no data block\n");
        close(fd);
        return 1;
    }

    uint8_t dir_block[BLOCK_SIZE];
    pread_block(fd, dir_block_no, dir_block);

    uint8_t journal[JOURNAL_BYTES];
    load_journal(fd, &sb, journal);

    struct journal_header jh;
    memcpy(&jh, journal, sizeof(jh));
    if (jh.magic != JOURNAL_MAGIC ||
        jh.nbytes_used < sizeof(jh) || jh.nbytes_used > JOURNAL_BYTES) {
        /* Journal doesn't exist yet (or is corrupt) - initialize it fresh. */
        jh.magic = JOURNAL_MAGIC;
        jh.nbytes_used = sizeof(jh);
    }

    uint32_t data_len = jh.nbytes_used - sizeof(jh);
    uint8_t *journal_data = journal + sizeof(jh);

    /* Fold in every already-committed-but-not-yet-installed transaction so
       that this create sees the up-to-date "effective" metadata rather than
       stale home-location state. Without this, two `create`s issued before
       an `install` would race for the same free inode / directory slot and
       the first one would be silently lost on install. */
    effective_ctx_t ectx = {
        .inode_bitmap_blk = sb.inode_bitmap,
        .inode_bitmap = inode_bitmap,
        .dir_block_no = dir_block_no,
        .dir_block = dir_block,
        .inode_blocks = inode_blocks,
    };
    for (uint32_t i = 0; i < INODE_BLOCKS; i++) {
        ectx.inode_block_no[i] = inode_block_no[i];
    }
    replay_journal(journal_data, data_len, apply_to_effective_state, &ectx);

    int free_inode = find_free_bit(inode_bitmap, sb.inode_count);
    if (free_inode < 0) {
        fprintf(stderr, "No free inodes\n");
        close(fd);
        return 1;
    }

    uint32_t max_entries = BLOCK_SIZE / sizeof(struct dirent);
    struct dirent *entries = (struct dirent *)dir_block;
    int free_entry = -1;
    for (uint32_t i = 0; i < max_entries; i++) {
        if (entries[i].inode == 0 && entries[i].name[0] == '\0') {
            free_entry = (int)i;
            break;
        }
        if (strcmp(entries[i].name, filename) == 0) {
            fprintf(stderr, "A file named '%s' already exists\n", filename);
            close(fd);
            return 1;
        }
    }
    if (free_entry < 0) {
        fprintf(stderr, "Directory full\n");
        close(fd);
        return 1;
    }

    uint32_t new_block_idx = free_inode / INODES_PER_BLOCK;
    uint32_t new_offset_idx = free_inode % INODES_PER_BLOCK;
    time_t now = time(NULL);

    dirty_block_t dirty[MAX_DIRTY_BLOCKS];
    int ndirty = 0;

    /* 1. Inode bitmap: mark the new inode allocated. */
    set_bit(inode_bitmap, (uint32_t)free_inode);
    dirty[ndirty].block_no = sb.inode_bitmap;
    memcpy(dirty[ndirty].data, inode_bitmap, BLOCK_SIZE);
    ndirty++;

    /* 2. Root inode's size/mtime (root always lives in inode block 0). */
    struct inode *inodes0 = (struct inode *)inode_blocks[0];
    inodes0[0].size += (uint32_t)sizeof(struct dirent);
    inodes0[0].mtime = (uint32_t)now;

    /* 3. The new inode itself (may or may not be the same block as root's). */
    struct inode *target_block_inodes = (struct inode *)inode_blocks[new_block_idx];
    struct inode new_inode = {0};
    new_inode.type = 1; /* regular file */
    new_inode.links = 1;
    new_inode.size = 0;
    new_inode.ctime = (uint32_t)now;
    new_inode.mtime = (uint32_t)now;
    target_block_inodes[new_offset_idx] = new_inode;

    dirty[ndirty].block_no = inode_block_no[0];
    memcpy(dirty[ndirty].data, inode_blocks[0], BLOCK_SIZE);
    ndirty++;
    if (new_block_idx != 0) {
        dirty[ndirty].block_no = inode_block_no[new_block_idx];
        memcpy(dirty[ndirty].data, inode_blocks[new_block_idx], BLOCK_SIZE);
        ndirty++;
    }

    /* 4. Root directory data block: add the new entry. */
    entries[free_entry].inode = (uint32_t)free_inode;
    strncpy(entries[free_entry].name, filename, sizeof(entries[free_entry].name) - 1);
    entries[free_entry].name[sizeof(entries[free_entry].name) - 1] = '\0';

    dirty[ndirty].block_no = dir_block_no;
    memcpy(dirty[ndirty].data, dir_block, BLOCK_SIZE);
    ndirty++;

    uint32_t rec_size = sizeof(struct rec_header) + 4 + BLOCK_SIZE;
    uint32_t needed = (uint32_t)ndirty * rec_size + sizeof(struct rec_header);

    if (data_len + needed > JOURNAL_DATA_CAPACITY) {
        fprintf(stderr, "Journal full, run install first\n");
        close(fd);
        return 1;
    }

    /* Append the transaction: one DATA record per dirty block, then COMMIT.
       Nothing outside the journal blocks is written - home locations for
       the inode bitmap / inode table / directory block are left untouched,
       as required. */
    uint32_t off = data_len;
    for (int i = 0; i < ndirty; i++) {
        struct rec_header rh = { .type = REC_DATA, .size = (uint16_t)rec_size };
        memcpy(journal_data + off, &rh, sizeof(rh));
        off += sizeof(rh);
        memcpy(journal_data + off, &dirty[i].block_no, 4);
        off += 4;
        memcpy(journal_data + off, dirty[i].data, BLOCK_SIZE);
        off += BLOCK_SIZE;
    }
    struct rec_header commit_rh = { .type = REC_COMMIT, .size = sizeof(struct rec_header) };
    memcpy(journal_data + off, &commit_rh, sizeof(commit_rh));
    off += sizeof(commit_rh);

    jh.nbytes_used = sizeof(jh) + off;
    memcpy(journal, &jh, sizeof(jh));

    write_journal(fd, &sb, journal);
    if (fsync(fd) < 0) {
        die("fsync");
    }

    if (close(fd) < 0) {
        die("close");
    }

    printf("Queued creation of '%s' (inode %d) in the journal - run './journal install' to apply it.\n",
           filename, free_inode);
    return 0;
}

static int journal_install(void) {
    int fd = open(DEFAULT_IMAGE, O_RDWR);
    if (fd < 0) {
        die("open");
    }

    struct superblock sb;
    pread_block(fd, 0, &sb);
    if (sb.magic != FS_MAGIC) {
        fprintf(stderr, "'%s' is not a valid VSFS image\n", DEFAULT_IMAGE);
        close(fd);
        return 1;
    }

    uint8_t journal[JOURNAL_BYTES];
    load_journal(fd, &sb, journal);

    struct journal_header jh;
    memcpy(&jh, journal, sizeof(jh));

    if (jh.magic != JOURNAL_MAGIC) {
        fprintf(stderr, "Journal does not exist\n");
        close(fd);
        return 1;
    }
    if (jh.nbytes_used < sizeof(jh) || jh.nbytes_used > JOURNAL_BYTES) {
        fprintf(stderr, "Journal header is corrupt\n");
        close(fd);
        return 1;
    }

    uint32_t data_len = jh.nbytes_used - sizeof(jh);
    uint8_t *journal_data = journal + sizeof(jh);

    /* Replay every fully-committed transaction straight to its home block.
       Any trailing incomplete transaction (no COMMIT reached) is discarded
       automatically by replay_journal. */
    replay_journal(journal_data, data_len, apply_to_disk, &fd);
    if (fsync(fd) < 0) {
        die("fsync");
    }

    /* Checkpoint: the journal becomes empty again. */
    jh.nbytes_used = sizeof(jh);
    memcpy(journal, &jh, sizeof(jh));
    pwrite_block(fd, sb.journal_block, journal); /* header lives in the first journal block */
    if (fsync(fd) < 0) {
        die("fsync");
    }

    if (close(fd) < 0) {
        die("close");
    }

    printf("Installed journaled metadata updates and cleared the journal.\n");
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <create|install> [name]\n", argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "create") == 0) {
        if (argc != 3) {
            fprintf(stderr, "Usage: %s create <filename>\n", argv[0]);
            return 1;
        }
        return journal_create(argv[2]);
    } else if (strcmp(argv[1], "install") == 0) {
        return journal_install();
    } else {
        fprintf(stderr, "Unknown command: %s\n", argv[1]);
        return 1;
    }
}

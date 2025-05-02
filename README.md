# Operating System Project 2 — Metadata Journaling

A C implementation of **metadata journaling** for a small VSFS-like file system. The project works directly on a pre-built disk image and implements crash-consistent metadata updates using an append-only journal.

## Project Goal

The file system stores metadata updates in a journal before applying them to their home locations. This allows committed metadata changes to be replayed safely and keeps the file system consistent across interruptions.

The disk layout uses **4 KB blocks** and contains:

- 1 superblock
- 16 journal blocks
- 1 inode bitmap block
- 1 data bitmap block
- 2 inode-table blocks
- 64 data blocks

## Main Programs

- `journal.c` — implements metadata journaling and the required commands
- `mkfs.c` — creates/recreates the VSFS disk image
- `validator.c` — checks file-system consistency
- `Term Project_ Metadata Journaling.pdf` — project specification

## Required Commands

### Create

```bash
./journal create <name>
```

Creates a file entry by **logging metadata changes only**. It initializes the journal when needed, finds a free inode and root-directory entry slot, computes new metadata in memory, appends complete modified metadata blocks as DATA records, and finishes the transaction with a COMMIT record. The home metadata blocks are not modified during `create`.

### Install

```bash
./journal install
```

Replays committed DATA records to their target home blocks and then clears/checkpoints the journal.

## Journal Design

The journal is an append-only byte array. Each transaction contains one or more **DATA records** followed by a **COMMIT record**. A DATA record stores the target home block number and the complete 4096-byte block image. Only committed transactions are replayed.

## Metadata Structures

The implementation works with the superblock, inode bitmap, data bitmap, inode table, directory entries, journal header, and journal records. Only metadata blocks are journaled.

## Validation

The supplied validator can be run with:

```bash
./validator
```

It checks inode allocation consistency, directory references, block validity, duplicate references, and dangling references.

## Typical Workflow

```bash
gcc -o mkfs mkfs.c
gcc -o journal journal.c
gcc -o validator validator.c

./mkfs
./validator
./journal create <name>
./validator
./journal install
./validator
```

Compiler flags can be adjusted to match the course environment.

## Repository Structure

```text
Operating-System-Project-2/
├── journal.c
├── mkfs.c
├── validator.c
├── Term Project_ Metadata Journaling.pdf
└── README.md
```

## Course Project

**Project:** Operating System Project 2  
**Topic:** Metadata Journaling  
**Completion date:** May 2, 2025

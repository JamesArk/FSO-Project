#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <assert.h>

#include "disk.h"
#include "fs.h"

#define MIN(X, Y) ((X) > (Y) ? (Y) : (X))

/*******
 * FSO OFS layout (there is no bootBlock)
 * FS block size = disk block size (1KB)
 * block#	| content
 * 0		| super block (with list of dir blocks)
 * 1		| first data block (usualy with 1st block of dir entries)
 * ...      | other dir blocks and files data blocks
 */

#define BLOCKSZ (DISK_BLOCK_SIZE)
#define SBLOCK 0          // superblock is at disk block 0
#define FS_MAGIC (0xf0f0) // for OFS
#define FNAMESZ 11        // file name size
#define LABELSZ 12        // disk label size
#define MAXDIRSZ 504      // max entries in the directory (1024-4-LABELSZ)/2

#define DIRENTS_PER_BLOCK (BLOCKSZ / sizeof(struct fs_dirent))
#define FBLOCKS 8 // 8 block indexes in each dirent

/* dirent .st field values: */
#define TFILE 0x10  // is file dirent
#define TEMPTY 0x00 // not used/free
#define TEXT 0xff   // is extent

#define FALSE 0
#define TRUE 1

#define FREE 0
#define NOT_FREE 1

/*** FSO Old/Our FileSystem disk layout ***/

struct fs_dirent { // a directory entry (dirent/extent)
    uint8_t st;
    char name[FNAMESZ];
    uint16_t ex; // numb of extra extents or id of this extent
    uint16_t ss; // number of bytes in the last extent (can be this dirent)
    uint16_t
        blocks[FBLOCKS]; // disk blocks with file content (zero value = empty)
};

struct fs_sblock { // the super block
    uint16_t magic;
    uint16_t fssize;        // total number of blocks (including this sblock)
    char label[LABELSZ];    // disk label
    uint16_t dir[MAXDIRSZ]; // directory blocks (zero value = empty)
};

/**
 * Nota: considerando o numero de ordem dos dirent em todos os blocos da
 * directoria um ficheiro pode ser identificado pelo numero do seu dirent. Tal
 * e' usado pelo open, create, read e write.
 */

union fs_block { // generic fs block. Can be seen with all these formats
    struct fs_sblock super;
    struct fs_dirent dirent[DIRENTS_PER_BLOCK];
    char data[BLOCKSZ];
};

/*******************************************/

struct fs_sblock superB; // superblock of the mounted disk

uint8_t *blockBitMap; // Map of used blocks (not a real bitMap, more a byteMap)
                      // this is build by mount operation, reading all the directory

/*******************************************/
/* The following functions may be usefull
 * change these and implement others that you need
 */

/**
 * allocBlock: allocate a new disk block
 * return: block number
 */
int allocBlock() {
    int i;

    for (i = 0; i < superB.fssize && blockBitMap[i] == NOT_FREE; i++)
        ;
    if (i < superB.fssize) {
        blockBitMap[i] = NOT_FREE;
        return i;
    } else
        return -1; // no disk space
}

/**
 */
void freeBlock(int nblock) {
    blockBitMap[nblock] = FREE;
}

/**
 * copy str to dst, converting from C string to FS string:
 *   - uppercase letters and ending with spaces
 * dst and str must exist with at least len chars
 */
void strEncode(char *dst, char *str, int len) {
    int i;
    for (i = 0; i < len && str[i] != '\0'; i++)
        if (isalpha(str[i]))
            dst[i] = toupper(str[i]);
        else if (isdigit(str[i]) || str[i] == '_' || str[i] == '.')
            dst[i] = str[i];
        else
            dst[i] = '?'; // invalid char?
    for (; i < len; i++)
        dst[i] = ' '; // fill with space

}

/**
 * copy str to dst, converting from FS string to C string
 * dst must exist with at least len+1 chars
 */
void strDecode(char *dst, char *str, int len) {
    int i;
    for (i = len - 1; i > 0 && str[i] == ' '; i--)
        ;
    dst[i + 1] = '\0';
    for (; i >= 0; i--)
        dst[i] = str[i];
}

/**
 * print super block content to stdout (for debug)
 */
void dumpSB() {
    union fs_block block;
    char label[LABELSZ + 1];

    disk_read(SBLOCK, block.data);
    printf("superblock:\n");
    printf("    magic = %x\n", block.super.magic);
    printf("    %d blocks\n", block.super.fssize);
    printf("    dir_size: %d\n", MAXDIRSZ);
    printf("    first dir block: %d\n", block.super.dir[0]);
    strDecode(label, block.super.label, LABELSZ);
    printf("    disk label: %s\n", label);

    printf("dir blocks: ");
    for (int i = 0; block.super.dir[i] != 0; i++)
        printf("%d ", block.super.dir[i]);
    putchar('\n');
}

/**
 * search and read file dirent/extent:
 * 	if ext==0: find 1st entry (with .st=TFILE)
 * 	if ext>0:  find extent (with .st=TEXT) and .ex==ext
 *  if ent!=NULL fill it with a copy of the dirent/extent
 *  return dirent index in the directory (or -1 if not found)
 */
int readFileEntry(char *name, uint16_t ext, struct fs_dirent *ent) {
    union fs_block block;

    for (int dirblk = 0; dirblk < MAXDIRSZ && superB.dir[dirblk]; dirblk++) {
        int b = superB.dir[dirblk];
        disk_read((unsigned int) b, block.data);
        for (int j = 0; j < DIRENTS_PER_BLOCK; j++)
            if ((((ext == 0 && block.dirent[j].st == TFILE)) &&
                 strncmp(block.dirent[j].name, name, FNAMESZ) == 0) ||
                (((block.dirent[j].st == TEXT && block.dirent[j].ex == ext)) &&
                 strncmp(block.dirent[j].name, name, FNAMESZ) == 0)) {
                if (ent != NULL)
                    *ent = block.dirent[j];
                return dirblk * DIRENTS_PER_BLOCK + j; // this dirent index
            }
    }

    return -1;
}

/**
 * update dirent at idx with 'entry' or, if idx==-1, add a new dirent to
 * directory with 'entry' content.
 * return: idx used/allocated, -1 if error (no space in directory)
 */
int writeFileEntry(int idx, struct fs_dirent entry) {

    // TODO: (if needed)

    // update dirent idx or allocate a new one
    // and write to directory on disk

    // notice: directory may need to grow!!
    union fs_block block;
    int blockNumber;
    if(idx == -1) {
        for(int i = 0; i < MAXDIRSZ && superB.dir[i]; i++) {
            int b = superB.dir[i];
            disk_read((unsigned int) b, block.data);
            for(int j = 0; j < DIRENTS_PER_BLOCK; j++) {
                if(block.dirent[j].st == TEMPTY) {
                    block.dirent[j] = entry;
                    disk_write((unsigned int) b, block.data);
                    return i * DIRENTS_PER_BLOCK + j;
                }
            }
        }
        blockNumber = allocBlock();
        disk_read((unsigned int) blockNumber, block.data);
        block.dirent[0] = entry;
        int i;
        for(i = 0; i < MAXDIRSZ && superB.dir[i]; i++);
        superB.dir[i] =(uint16_t) blockNumber;
        disk_write((unsigned int) blockNumber, block.data);
        return readFileEntry(entry.name,entry.ex,&entry);
    } else {
        int numberBlock = idx / DIRENTS_PER_BLOCK;
        disk_read(superB.dir[numberBlock],block.data);
        block.dirent[idx-numberBlock*DIRENTS_PER_BLOCK] = entry;
        disk_write(superB.dir[numberBlock],block.data);
        return idx;
    }
}

/****************************************************************/

int fs_delete(char *name) {

    if (superB.magic != FS_MAGIC) {
        printf("disc not mounted\n");
        return -1;
    }
    char fname[FNAMESZ];
    strEncode(fname, name, FNAMESZ);

    // TODO: delete file: free it's dirent, extents and data blocks
    union fs_block block;
    memset(block.data,FREE,BLOCKSZ);
    struct fs_dirent* entry = &block.dirent[0];
    int result = 1;
    int idx;
    if((idx =readFileEntry(fname,0,entry)) != -1) {
        result = 0;
        entry->st = TEMPTY;
        for(int j = 0; j < FBLOCKS && entry->blocks[j]; j++)
            freeBlock(entry->blocks[j]);
        writeFileEntry(idx,*(entry));
    } else
        return result;

    return result;
}

/*****************************************************/

void fs_dir() {

    if (superB.magic != FS_MAGIC) {
        printf("disc not mounted\n");
        return;
    }

    // TODO: list files
    // printf( "%u: %s, size: %u bytes\n", dirent_number, file_name, file_size)

    union fs_block block;
    for(unsigned int i = 0; i < MAXDIRSZ && superB.dir[i]; i++) {
        disk_read(superB.dir[i],block.data);
        for(int j = 0; j < DIRENTS_PER_BLOCK; j++) {
          struct fs_dirent dirent = block.dirent[j];
          char file_name[FNAMESZ+1];
          strDecode(file_name, dirent.name, FNAMESZ);
          uint16_t file_size = dirent.ss;
          if(file_size != 0) {
              uint16_t dirent_number = (uint16_t) j;
              printf("%u: %s, size: %u bytes\n", dirent_number, file_name, file_size);
          }
      }
    }
}

/*****************************************************/

void fs_debug() {
    union fs_block block;

    disk_read(SBLOCK, block.data);

    if (block.super.magic != FS_MAGIC) {
        printf("disk unformatted !\n");
        return;
    }
    dumpSB();

    printf("**************************************\n");
    if (superB.magic == FS_MAGIC) {
        printf("Used blocks: ");
        for (int i = 0; i < superB.fssize; i++) {
            if (blockBitMap[i] == NOT_FREE)
                printf(" %d", i);
        }
        puts("\nFiles:\n");
        fs_dir();
    }
    printf("**************************************\n");
}

/*****************************************************/

int fs_format(char *disklabel) {
    union fs_block block;
    int nblocks;

    assert(sizeof(struct fs_dirent) == 32);
    assert(sizeof(union fs_block) == BLOCKSZ);

    if (superB.magic == FS_MAGIC) {
        printf("Cannot format a mounted disk!\n");
        return 0;
    }
    if (sizeof(block) != DISK_BLOCK_SIZE) {
        printf("Disk block and FS block mismatch\n");
        return 0;
    }
    memset(&block, 0, sizeof(block));
    disk_write(1, block.data); // write 1st dir block all zeros

    nblocks = disk_size();
    block.super.magic = FS_MAGIC;
    block.super.fssize = nblocks;
    strEncode(block.super.label, disklabel, LABELSZ);
    block.super.dir[0] = 1; // block 1 is first dir block

    disk_write(0, block.data);  // write superblock
    dumpSB(); // debug

    return 1;
}

/*****************************************************************/

int fs_mount() {
    union fs_block block;

    if (superB.magic == FS_MAGIC) {
        printf("One disc is already mounted!\n");
        return 0;
    }
    disk_read(0, block.data);
    superB = block.super;

    if (superB.magic != FS_MAGIC) {
        printf("cannot mount an unformatted disc!\n");
        return 0;
    }
    if (superB.fssize != disk_size()) {
        printf("file system size and disk size differ!\n");
        return 0;
    }

    // build used blocks map
    blockBitMap = malloc(superB.fssize * sizeof(uint16_t));
    memset(blockBitMap,FREE,superB.fssize * sizeof(uint16_t));
    blockBitMap[0] = NOT_FREE; // 0 is used by superblock

    // TODO: blockBitMap[i]=NOT_FREE if block i is in use
    //       check all directory

    union fs_block tempBlock;
    for(int i = 0; i < MAXDIRSZ && superB.dir[i]; i++) {
        blockBitMap[superB.dir[i]] = NOT_FREE;
        disk_read(superB.dir[i], tempBlock.data);
        for (unsigned int j = 0; j < DIRENTS_PER_BLOCK; j++) {
            if (tempBlock.dirent[j].st != TEMPTY)
                for (int k = 0; k < FBLOCKS && tempBlock.dirent[j].blocks[k]; k++)
                    blockBitMap[tempBlock.dirent[j].blocks[k]] = NOT_FREE;
        }
    }
    return 1;
}

/************************************************************/

int fs_read(char *name, char *data, int length, int offset) {

    if (superB.magic != FS_MAGIC) {
        printf("disc not mounted\n");
        return -1;
    }
    char fname[FNAMESZ];
    strEncode(fname, name, FNAMESZ);

    // TODO: read file data

    int blockNOfFile = offset/BLOCKSZ;                                                          // numero do bloco do ficheiro (como se houvesse um vetor de blocos de ficheiro).
    int blockNumberEntry = blockNOfFile % FBLOCKS;                                              // numero do bloco da entrada a ser lida (dirent.blocks[]).
    int firstBlockOffset = offset % BLOCKSZ;                                                    // o offset do primeiro bloco a ser lido.
    int numberOfBlocksToRead = (length + firstBlockOffset)/BLOCKSZ +1;                          // numero de blocos a ler (regras do disco: so pode ser lido um bloco de cada vez).
    int lastBlockOffset = (length + offset) - BLOCKSZ*(blockNOfFile + numberOfBlocksToRead -1); // o offset do ultimo bloco (numero de bytes a ler do ultimo bloco).
    int incompleteBlockIdx;                                                                     // index do primeiro bloco da entry incompleto
    union fs_block block;                                                                       // bloco geral pra dados
    memset(block.data,FREE,BLOCKSZ);
    struct fs_dirent entry = block.dirent[0];                                                   // entrada geral pra leitura de entradas
    int bytesRead = 0;
    int numberOfBlocksRead = 0;
    if(!length)                                                                                 // Sneaky length is sneaky
        return 0;
    if(readFileEntry(fname,0,&entry) == -1)                                                     // FILE DOES NOT EXIST
        return -1;
    disk_read(entry.blocks[blockNumberEntry], block.data);
    incompleteBlockIdx = entry.ss/BLOCKSZ;
    int fileFinalOffset = entry.ss - (incompleteBlockIdx)*BLOCKSZ;                              // LastBlockOffset mas do ficheiro em vez do ultimo bloco para ser lido.
    if(offset >= entry.ss)                                                                      // SOME WEIRD STUFF HAPPENED CHECK IT
        return 0;
    if(incompleteBlockIdx == 0 || incompleteBlockIdx == blockNumberEntry){                      // FIRST BLOCK TO READ IS INCOMPLETE OR ENTRY ONLY HAS ONE BLOCK
        int i;
        for(i = 0; i<fileFinalOffset && i < lastBlockOffset && bytesRead+offset <entry.ss; i++) {   // READ UNTIL THE FILE ENDS OR THE BYTES ASKED TO BE WRITTEN ENDS
            data[i] = block.data[firstBlockOffset + i];
            bytesRead++;
        }
        if(bytesRead == fileFinalOffset)        // FILE IS FINISHED
            return 0;
        else
            return bytesRead;                   // FILE IS NOT FINISHED
    }
    int i;
    for(i = 0; i< BLOCKSZ-firstBlockOffset && bytesRead<length; i++)        // FIRST BLOCK TO READ
        data[i] = block.data[firstBlockOffset+i];
    bytesRead += i;
    blockNumberEntry++;
    numberOfBlocksRead++;
    for (int k = blockNumberEntry; k < FBLOCKS && numberOfBlocksRead< numberOfBlocksToRead; k++) {  // EVERY BLOCK TO READ UNTIL BYTES == LENGTH OR FILE ENDS
        if(entry.blocks[k] == 0)    // BLOCK NON EXISTENT FILE ENDS HERE
            return 0;
        disk_read(entry.blocks[k], block.data);
        int j;
        if(k != incompleteBlockIdx)                             // BLOCK TO BE READ IS INCOMPLETE
            for (j = 0; j < BLOCKSZ && bytesRead < length; j++)
                data[bytesRead++] = block.data[j];
        else                                                    // BLOCK IS TO BE READ FULLY OR UNTIL DATA IS FULL
            for (j = 0; j < fileFinalOffset && j < lastBlockOffset && bytesRead<length; j++)
                data[bytesRead++] = block.data[j];
        numberOfBlocksRead++;
        }
        if((bytesRead + offset) < entry.ss)                 // se ainda existe ficheiro pra ler
            return bytesRead;
        else if((bytesRead +offset) == entry.ss)            // se acabou o ficheiro
            return 0;
        else
            return -1;                                      // SOMETHING WENT WRONG
}

/****************************************************************/

int fs_write(char *name, char *data, int length, int offset) { // length max value is 8KB, Offset will never be outside the file

    if (superB.magic != FS_MAGIC) {
        printf("disc not mounted\n");
        return -1;
    }
    char fname[FNAMESZ];
    strEncode(fname, name, FNAMESZ);

    // TODO: write data to file
    int blockNOfFile = offset / BLOCKSZ;                                                                // numero do bloco do ficheiro (como se houvesse um vetor de blocos de ficheiro).
    int blockNumberEntry = blockNOfFile % FBLOCKS;                                                      // numero do bloco da entrada a ser escrita (dirent.blocks[]).
    int firstBlockOffset = offset % BLOCKSZ;                                                            // o offset do primeiro bloco a ser escrito.
    int numberOfBlocksToWrite = (length + firstBlockOffset) / BLOCKSZ+1;                                // numero de blocos a escrever (regras do disco: so pode ser escrito um bloco de cada vez).
    int lastBlockOffset = (length + offset) - BLOCKSZ * (blockNOfFile + numberOfBlocksToWrite - 1);     // o offset do ultimo bloco (numero de bytes a escrever do ultimo bloco).
    int bytesWritten = 0;
    union fs_block block;
    memset(block.data, FREE, BLOCKSZ);
    struct fs_dirent entry = block.dirent[0];
    int numberOfBlocksWritten;
    int blockNumber;
    int idxEntry = readFileEntry(fname, 0, &entry);                                                     // index da entrada a ser escrita
    if(length == 0)                                                                                     // Sneaky length is sneaky
        return 0;
    if (idxEntry == -1) {                                                                               // ENTRY NOT FOUND <=> FILE NOT FOUND
        entry.st = TFILE;
        entry.ex = 0;
        for (int i = 0; i < FNAMESZ; i++)
            entry.name[i] = fname[i];
        entry.ss = (uint16_t) length;
        for (numberOfBlocksWritten = 0; numberOfBlocksWritten < numberOfBlocksToWrite - 1; numberOfBlocksWritten++) {       // WRITING ALL COMPLETE BLOCKS
            blockNumber = allocBlock();
            if (blockNumber == -1)
                return 0; // NO MORE DISK SPACE
            disk_write((uint16_t) blockNumber, data + bytesWritten);
            bytesWritten += BLOCKSZ;
            entry.blocks[numberOfBlocksWritten] = (uint16_t) blockNumber;
        }
        blockNumber = allocBlock();
        if (blockNumber == -1)
            return 0;// NO MORE DISK SPACE
        for (int j = 0; j < lastBlockOffset; j++)     // WRITING LAST BLOCK
            block.data[j] = data[bytesWritten + j];
        bytesWritten += lastBlockOffset;
        entry.blocks[(uint16_t) numberOfBlocksWritten] = (uint16_t) blockNumber;
        disk_write((unsigned int) blockNumber, block.data);
    } else {
        // ENTRY FOUND <=> FILE EXISTS
        int nBlocksToWrite = numberOfBlocksToWrite;
        int numberOfNewBlocks = 0;
        int entrySizeOffSet = entry.ss % BLOCKSZ;
        for (int k = blockNumberEntry;nBlocksToWrite > 0; k++) {  // WRITING ALL BLOCKS INCLUDING THE LAST ONE
            if (!entry.blocks[k]) {
                blockNumber = allocBlock();
                if (blockNumber == -1)
                    return 0;
                numberOfNewBlocks++;
                entry.blocks[k] = (uint16_t) blockNumber;
            }else
                blockNumber = entry.blocks[k];
            disk_read((unsigned int) blockNumber, block.data);
            int l;
            if (nBlocksToWrite == numberOfBlocksToWrite) {
                if (numberOfBlocksToWrite == 1) {
                    for (l = firstBlockOffset; l < lastBlockOffset; l++)
                        block.data[l] = data[l - firstBlockOffset];
                    bytesWritten += length;
                } else {
                    for (l = firstBlockOffset; l < BLOCKSZ; l++)
                        block.data[l] = data[l - firstBlockOffset];
                    bytesWritten += BLOCKSZ-firstBlockOffset;
                }
            } else if (nBlocksToWrite == 1) {
                for (l = 0; l < lastBlockOffset; l++)
                    block.data[l] = data[l + bytesWritten];
                bytesWritten += lastBlockOffset;
            } else {
                for (l = 0; l < BLOCKSZ; l++)
                    block.data[l] = data[l  + bytesWritten];
                bytesWritten += BLOCKSZ;
            }
            disk_write((unsigned int) blockNumber, block.data);
            nBlocksToWrite--;
        }
        if (numberOfNewBlocks == 0) {           // NO NEW COMPLETE BLOCKS WERE ADDED
            if (entrySizeOffSet < lastBlockOffset)      // SOME BYTES WERE ADDED
                entry.ss += lastBlockOffset-entrySizeOffSet;
        } else                                  // NEW BLOCKS WERE ADDED
            entry.ss += (uint16_t) (BLOCKSZ - entrySizeOffSet + (numberOfNewBlocks - 1) * BLOCKSZ + lastBlockOffset);
    }
    writeFileEntry(idxEntry, entry);
    return bytesWritten;
}
#define _FILE_OFFSET_BITS 64

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/fs.h>

#define SECTOR_SIZE 512
#define RDB_SECTORS_RESERVED 64  /* RDB-Such- und Speicherbereich (Sektor 0-63) */
#define ID_RDSK 0x5244534B       /* "RDSK" */
#define ID_PART 0x50415254       /* "PART" */

#ifndef BLKGETSIZE
#define BLKGETSIZE _IO(0x12,96)
#endif
#ifndef BLKGETSIZE64
#define BLKGETSIZE64 _IOR(0x12,114,size_t)
#endif

typedef struct {
    const char *name;
    uint32_t dostype;
} DosTypeMap;

typedef struct {
    uint32_t low;
    uint32_t high;
    char name[32];
    uint32_t dostype;
} PartInfo;

static const DosTypeMap dostype_table[] = {
    {"OFS",      0x444F5300},
    {"FFS",      0x444F5301},
    {"FFS-INTL", 0x444F5303},
    {"FFS-DC",   0x444F5305},
    {"FFS2",     0x444F5307},
    {"SFS",      0x53465300},
    {"SFS2",     0x53465302},
    {"PFS3",     0x50465303},
    {"SWAP",     0x53575000},
    {"EXT2",     0x45585432},
    {"EXT3",     0x45585433},
    {NULL, 0}
};

static uint64_t get_disk_size(int fd) {
    uint64_t bytes = 0;
    unsigned long sectors = 0;

    if (ioctl(fd, BLKGETSIZE64, &bytes) == 0 && bytes > 0) {
        return bytes;
    }

    if (ioctl(fd, BLKGETSIZE, &sectors) == 0 && sectors > 0) {
        return (uint64_t)sectors * SECTOR_SIZE;
    }

    off_t size = lseek(fd, 0, SEEK_END);
    if (size > 0) {
        lseek(fd, 0, SEEK_SET);
        return (uint64_t)size;
    }
    lseek(fd, 0, SEEK_SET);

    return 0;
}

static inline uint32_t read_be32(const uint8_t *ptr) {
    return ((uint32_t)ptr[0] << 24) | ((uint32_t)ptr[1] << 16) |
           ((uint32_t)ptr[2] << 8)  | ((uint32_t)ptr[3]);
}

static inline void write_be32(uint8_t *ptr, uint32_t val) {
    ptr[0] = (val >> 24) & 0xFF;
    ptr[1] = (val >> 16) & 0xFF;
    ptr[2] = (val >> 8)  & 0xFF;
    ptr[3] = val & 0xFF;
}

static void update_rdb_checksum(uint8_t *block) {
    write_be32(&block[8], 0);
    uint32_t sum = 0;
    for (int i = 0; i < SECTOR_SIZE; i += 4) {
        sum += read_be32(&block[i]);
    }
    write_be32(&block[8], 0 - sum);
}

static uint32_t resolve_dostype(const char *str) {
    for (int i = 0; dostype_table[i].name != NULL; i++) {
        if (strcasecmp(str, dostype_table[i].name) == 0) {
            return dostype_table[i].dostype;
        }
    }
    if (strncasecmp(str, "0x", 2) == 0) {
        return (uint32_t)strtoul(str, NULL, 16);
    }
    if (strlen(str) == 4) {
        return ((uint32_t)str[0] << 24) | ((uint32_t)str[1] << 16) |
               ((uint32_t)str[2] << 8)  | ((uint32_t)str[3]);
    }
    return 0;
}

static void get_dostype_string(uint32_t dostype, char *buf, size_t len) {
    for (int i = 0; dostype_table[i].name != NULL; i++) {
        if (dostype_table[i].dostype == dostype) {
            snprintf(buf, len, "%s", dostype_table[i].name);
            return;
        }
    }
    
    char ascii[5] = {
        (char)((dostype >> 24) & 0xFF),
        (char)((dostype >> 16) & 0xFF),
        (char)((dostype >> 8) & 0xFF),
        (char)(dostype & 0xFF),
        '\0'
    };
    int printable = 1;
    for (int c = 0; c < 4; c++) {
        if (ascii[c] < 32 || ascii[c] > 126) {
            printable = 0;
            break;
        }
    }
    if (printable) {
        snprintf(buf, len, "%s", ascii);
    } else {
        snprintf(buf, len, "0x%08X", dostype);
    }
}

static uint64_t parse_size_to_bytes(const char *str, uint64_t total_disk_bytes, int *has_unit) {
    char *endptr;
    double val = strtod(str, &endptr);
    *has_unit = 1;

    if (endptr == str) {
        *has_unit = 0;
        return 0;
    }

    if (*endptr == '%') {
        return (uint64_t)((val / 100.0) * total_disk_bytes);
    } else if (strcasecmp(endptr, "K") == 0 || strcasecmp(endptr, "KB") == 0 || strcasecmp(endptr, "KiB") == 0) {
        return (uint64_t)(val * 1024.0);
    } else if (strcasecmp(endptr, "M") == 0 || strcasecmp(endptr, "MB") == 0 || strcasecmp(endptr, "MiB") == 0) {
        return (uint64_t)(val * 1024.0 * 1024.0);
    } else if (strcasecmp(endptr, "G") == 0 || strcasecmp(endptr, "GB") == 0 || strcasecmp(endptr, "GiB") == 0) {
        return (uint64_t)(val * 1024.0 * 1024.0 * 1024.0);
    } else if (strcasecmp(endptr, "B") == 0) {
        return (uint64_t)val;
    }

    *has_unit = 0;
    return (uint64_t)val;
}

static int compare_parts(const void *a, const void *b) {
    PartInfo *p1 = (PartInfo *)a;
    PartInfo *p2 = (PartInfo *)b;
    if (p1->low < p2->low) return -1;
    if (p1->low > p2->low) return 1;
    return 0;
}

static int check_partition_overlap(int fd, uint32_t first_part_sec, 
                                   uint32_t low_cyl, uint32_t high_cyl, 
                                   uint32_t *max_used_cyl) {
    uint32_t curr = first_part_sec;
    uint8_t block[SECTOR_SIZE];
    *max_used_cyl = 1;

    while (curr != 0xFFFFFFFF && curr != 0) {
        lseek(fd, (off_t)(curr * SECTOR_SIZE), SEEK_SET);
        if (read(fd, block, SECTOR_SIZE) != SECTOR_SIZE) break;

        if (read_be32(&block[0]) == ID_PART) {
            uint32_t p_low = read_be32(&block[164]);
            uint32_t p_high = read_be32(&block[168]);

            if (p_high > *max_used_cyl) {
                *max_used_cyl = p_high;
            }

            if (low_cyl <= high_cyl && high_cyl > 0) {
                if (!(high_cyl < p_low || low_cyl > p_high)) {
                    fprintf(stderr, "[-] FEHLER: Überschneidung! Neue Partition (Zyl %u-%u) überlappt mit existierender Partition (Zyl %u-%u)!\n",
                            low_cyl, high_cyl, p_low, p_high);
                    return -1;
                }
            }
        }
        curr = read_be32(&block[16]);
    }
    return 0;
}

static int calculate_cylinders(const char *start_str, const char *end_str, 
                               uint64_t cyl_size_bytes, uint64_t disk_size_bytes, 
                               uint32_t *low_cyl, uint32_t *high_cyl) {
    int has_unit = 0;

    uint64_t start_val = parse_size_to_bytes(start_str, disk_size_bytes, &has_unit);
    if (has_unit) {
        *low_cyl = (uint32_t)((start_val + cyl_size_bytes - 1) / cyl_size_bytes);
    } else {
        *low_cyl = (uint32_t)start_val;
    }

    if (end_str[0] == '+') {
        uint64_t rel_val = parse_size_to_bytes(end_str + 1, disk_size_bytes, &has_unit);
        if (has_unit) {
            uint32_t cyls_needed = (uint32_t)((rel_val + cyl_size_bytes - 1) / cyl_size_bytes);
            *high_cyl = *low_cyl + (cyls_needed > 0 ? cyls_needed - 1 : 0);
        } else {
            *high_cyl = *low_cyl + (uint32_t)rel_val - 1;
        }
    } else {
        uint64_t end_val = parse_size_to_bytes(end_str, disk_size_bytes, &has_unit);
        if (has_unit) {
            uint32_t target_cyl = (uint32_t)(end_val / cyl_size_bytes);
            *high_cyl = (target_cyl > 0) ? target_cyl - 1 : 0;
        } else {
            *high_cyl = (uint32_t)end_val;
        }
    }

    if (*high_cyl < *low_cyl) {
        fprintf(stderr, "[-] Fehler: End-Zyl (%u) liegt vor Start-Zyl (%u)!\n", *high_cyl, *low_cyl);
        return 0;
    }

    return 1;
}

static uint32_t get_sector_by_sorted_index(int fd, uint32_t first_part, long target_num) {
    typedef struct {
        uint32_t sector;
        uint32_t low;
    } SortInfo;
    
    SortInfo sparts[64];
    int sp_count = 0;
    uint32_t curr = first_part;
    uint8_t block[SECTOR_SIZE];
    
    while (curr != 0xFFFFFFFF && curr != 0 && sp_count < 64) {
        lseek(fd, (off_t)(curr * SECTOR_SIZE), SEEK_SET);
        if (read(fd, block, SECTOR_SIZE) == SECTOR_SIZE && read_be32(&block[0]) == ID_PART) {
            sparts[sp_count].sector = curr;
            sparts[sp_count].low = read_be32(&block[164]);
            sp_count++;
        }
        curr = read_be32(&block[16]);
    }
    
    if (target_num <= 0 || target_num > sp_count) return 0;

    for (int i = 0; i < sp_count - 1; i++) {
        for (int j = 0; j < sp_count - i - 1; j++) {
            if (sparts[j].low > sparts[j+1].low) {
                SortInfo temp = sparts[j];
                sparts[j] = sparts[j+1];
                sparts[j+1] = temp;
            }
        }
    }
    return sparts[target_num - 1].sector;
}

static int do_rename(int fd, const char *device, const char *target_arg, const char *new_name) {
    if (strlen(new_name) > 31) {
        fprintf(stderr, "[-] Fehler: Neuer Name '%s' ist zu lang (max. 31 Zeichen)!\n", new_name);
        return 1;
    }

    char *endptr;
    long target_num = strtol(target_arg, &endptr, 10);
    int is_numeric = (*endptr == '\0' && target_num > 0);

    lseek(fd, 0, SEEK_SET);

    uint8_t rdsk[SECTOR_SIZE];
    int rdsk_sec = -1;
    for (int i = 0; i < RDB_SECTORS_RESERVED; i++) {
        if (read(fd, rdsk, SECTOR_SIZE) != SECTOR_SIZE) break;
        if (read_be32(&rdsk[0]) == ID_RDSK) {
            rdsk_sec = i;
            break;
        }
    }

    if (rdsk_sec == -1) {
        fprintf(stderr, "[-] Kein Amiga RDB auf '%s' gefunden. Ist die Partitionstabelle initialisiert?\n", device);
        return 1;
    }

    uint32_t curr = read_be32(&rdsk[28]);
    uint32_t target_sector = 0;

    if (is_numeric) {
        target_sector = get_sector_by_sorted_index(fd, curr, target_num);
        if (target_sector == 0) {
            fprintf(stderr, "[-] Partition Nr. %ld existiert nicht.\n", target_num);
            return 1;
        }
    }

    uint8_t block[SECTOR_SIZE];

    while (curr != 0xFFFFFFFF && curr != 0) {
        lseek(fd, (off_t)(curr * SECTOR_SIZE), SEEK_SET);
        if (read(fd, block, SECTOR_SIZE) != SECTOR_SIZE) break;

        if (read_be32(&block[0]) == ID_PART) {
            uint8_t name_len = block[36];
            if (name_len > 31) name_len = 31;

            char current_name[32];
            memcpy(current_name, &block[37], name_len);
            current_name[name_len] = '\0';

            int match = 0;
            if (is_numeric && curr == target_sector) {
                match = 1;
            } else if (!is_numeric && strcasecmp(current_name, target_arg) == 0) {
                match = 1;
            }

            if (match) {
                size_t new_len = strlen(new_name);
                block[36] = (uint8_t)new_len;
                memset(&block[37], 0, 31);
                memcpy(&block[37], new_name, new_len);

                update_rdb_checksum(block);

                lseek(fd, (off_t)(curr * SECTOR_SIZE), SEEK_SET);
                if (write(fd, block, SECTOR_SIZE) != SECTOR_SIZE) {
                    perror("[-] Fehler beim Schreiben des neuen Partitionsnamens");
                    return 1;
                }

                printf("[+] Partition ('%s') erfolgreich in '%s' umbenannt (Sektor %u)!\n", 
                       current_name, new_name, curr);
                return 0;
            }
        }
        curr = read_be32(&block[16]);
    }

    if (is_numeric) {
        fprintf(stderr, "[-] Partition Nr. %ld wurde im RDB von '%s' nicht gefunden.\n", target_num, device);
    } else {
        fprintf(stderr, "[-] Partition '%s' wurde im RDB von '%s' nicht gefunden.\n", target_arg, device);
    }
    return 1;
}

static int do_rmpart(int fd, const char *device, const char *target_arg, int force_flag) {
    char *endptr;
    long target_num = strtol(target_arg, &endptr, 10);
    int is_numeric = (*endptr == '\0' && target_num > 0);

    lseek(fd, 0, SEEK_SET);

    uint8_t rdsk[SECTOR_SIZE];
    int rdsk_sec = -1;
    for (int i = 0; i < RDB_SECTORS_RESERVED; i++) {
        if (read(fd, rdsk, SECTOR_SIZE) != SECTOR_SIZE) break;
        if (read_be32(&rdsk[0]) == ID_RDSK) {
            rdsk_sec = i;
            break;
        }
    }

    if (rdsk_sec == -1) {
        fprintf(stderr, "[-] Kein Amiga RDB auf '%s' gefunden.\n", device);
        return 1;
    }

    uint32_t curr = read_be32(&rdsk[28]);
    uint32_t target_sector = 0;

    if (is_numeric) {
        target_sector = get_sector_by_sorted_index(fd, curr, target_num);
        if (target_sector == 0) {
            fprintf(stderr, "[-] Partition Nr. %ld existiert nicht.\n", target_num);
            return 1;
        }
    }

    uint32_t prev_sector = 0;
    int prev_is_rdsk = 1;

    uint8_t block[SECTOR_SIZE];

    while (curr != 0xFFFFFFFF && curr != 0) {
        uint32_t sector_to_read = curr;
        lseek(fd, (off_t)(sector_to_read * SECTOR_SIZE), SEEK_SET);
        if (read(fd, block, SECTOR_SIZE) != SECTOR_SIZE) break;

        if (read_be32(&block[0]) == ID_PART) {
            uint8_t name_len = block[36];
            if (name_len > 31) name_len = 31;

            char current_name[32];
            memcpy(current_name, &block[37], name_len);
            current_name[name_len] = '\0';

            int match = 0;
            if (is_numeric && sector_to_read == target_sector) {
                match = 1;
            } else if (!is_numeric && strcasecmp(current_name, target_arg) == 0) {
                match = 1;
            }

            if (match) {
                if (!force_flag) {
                    uint32_t p_low = read_be32(&block[164]);
                    uint32_t p_high = read_be32(&block[168]);
                    uint32_t dostype = read_be32(&block[192]);

                    fprintf(stderr, "[!] WARNUNG: Partition ('%s', DosType 0x%08X) wird gelöscht!\n",
                            current_name, dostype);
                    fprintf(stderr, "    Zylinder %u-%u werden aus der RDB-Kette auf '%s' entfernt.\n",
                            p_low, p_high, device);
                    fprintf(stderr, "    Möchten Sie fortfahren? [y/N]: ");
                    
                    char answer[16];
                    if (fgets(answer, sizeof(answer), stdin) == NULL ||
                        (answer[0] != 'y' && answer[0] != 'Y')) {
                        printf("[-] Abgebrochen.\n");
                        return 0;
                    }
                }

                uint32_t next_ptr = read_be32(&block[16]); /* pe_Next */

                if (prev_is_rdsk) {
                    write_be32(&rdsk[28], next_ptr);
                    update_rdb_checksum(rdsk);
                    lseek(fd, (off_t)(rdsk_sec * SECTOR_SIZE), SEEK_SET);
                    write(fd, rdsk, SECTOR_SIZE);
                } else {
                    uint8_t prev_block[SECTOR_SIZE];
                    lseek(fd, (off_t)(prev_sector * SECTOR_SIZE), SEEK_SET);
                    read(fd, prev_block, SECTOR_SIZE);
                    write_be32(&prev_block[16], next_ptr);
                    update_rdb_checksum(prev_block);
                    lseek(fd, (off_t)(prev_sector * SECTOR_SIZE), SEEK_SET);
                    write(fd, prev_block, SECTOR_SIZE);
                }

                uint8_t zero_block[SECTOR_SIZE];
                memset(zero_block, 0, SECTOR_SIZE);
                lseek(fd, (off_t)(sector_to_read * SECTOR_SIZE), SEEK_SET);
                write(fd, zero_block, SECTOR_SIZE);

                fsync(fd);
                printf("[+] Partition ('%s') in Sektor %u erfolgreich gelöscht und Kette aktualisiert!\n",
                       current_name, sector_to_read);
                return 0;
            }
        }

        prev_sector = curr;
        prev_is_rdsk = 0;
        curr = read_be32(&block[16]);
    }

    if (is_numeric) {
        fprintf(stderr, "[-] Partition Nr. %ld wurde im RDB von '%s' nicht gefunden.\n", target_num, device);
    } else {
        fprintf(stderr, "[-] Partition '%s' wurde im RDB von '%s' nicht gefunden.\n", target_arg, device);
    }
    return 1;
}

static int do_free(int fd, const char *device) {
    lseek(fd, 0, SEEK_SET);

    uint8_t rdsk[SECTOR_SIZE];
    int rdsk_sec = -1;
    for (int i = 0; i < RDB_SECTORS_RESERVED; i++) {
        if (read(fd, rdsk, SECTOR_SIZE) != SECTOR_SIZE) break;
        if (read_be32(&rdsk[0]) == ID_RDSK) {
            rdsk_sec = i;
            break;
        }
    }

    if (rdsk_sec == -1) {
        fprintf(stderr, "[-] Kein Amiga RDB auf '%s' gefunden. Bitte zuerst 'mklabel rdb' ausführen.\n", device);
        return 1;
    }

    uint32_t sectors_per_track = read_be32(&rdsk[68]);
    uint32_t surfaces          = read_be32(&rdsk[72]);
    uint32_t total_cyls        = read_be32(&rdsk[64]);
    uint32_t rdb_low_cyl       = read_be32(&rdsk[136]);
    uint32_t rdb_high_cyl      = read_be32(&rdsk[140]);

    if (surfaces == 0 || sectors_per_track == 0) {
        fprintf(stderr, "[-] Ungültige RDB-Geometrie auf '%s'.\n", device);
        return 1;
    }

    uint32_t secs_per_cyl = surfaces * sectors_per_track;
    uint64_t cyl_size_bytes = (uint64_t)secs_per_cyl * SECTOR_SIZE;

    PartInfo parts[64];
    int part_count = 0;

    uint32_t curr = read_be32(&rdsk[28]);
    uint8_t block[SECTOR_SIZE];

    while (curr != 0xFFFFFFFF && curr != 0 && part_count < 64) {
        lseek(fd, (off_t)(curr * SECTOR_SIZE), SEEK_SET);
        if (read(fd, block, SECTOR_SIZE) != SECTOR_SIZE) break;

        if (read_be32(&block[0]) == ID_PART) {
            parts[part_count].low = read_be32(&block[164]);
            parts[part_count].high = read_be32(&block[168]);
            parts[part_count].dostype = read_be32(&block[192]);

            uint8_t name_len = block[36];
            if (name_len > 31) name_len = 31;
            memcpy(parts[part_count].name, &block[37], name_len);
            parts[part_count].name[name_len] = '\0';

            part_count++;
        }
        curr = read_be32(&block[16]);
    }

    qsort(parts, part_count, sizeof(PartInfo), compare_parts);

    printf("==================================================================================-\n");
    printf(" Speichersituation & Freispeicher auf '%s'\n", device);
    printf("==================================================================================-\n");
    printf(" Geometrie: %u Zylinder (%u Heads, %u Sectors/Track, %.2f KB/Zyl)\n\n",
           total_cyls, surfaces, sectors_per_track, (double)cyl_size_bytes / 1024.0);

    uint32_t track_cyl = rdb_low_cyl;
    uint64_t total_free_bytes = 0;

    printf(" %-4s | %-12s | %-16s | %-14s | %s\n", "Nr.", "Typ", "Zylinder", "Größe", "Details / Name");
    printf("-----------------------------------------------------------------------------------\n");

    if (rdb_low_cyl > 0) {
        char cyl_range[32];
        snprintf(cyl_range, sizeof(cyl_range), "%u - %u", 0, rdb_low_cyl - 1);
        printf(" %-4s | %-12s | %-16s | %-14s | System-Bereich (RDB/Boot)\n", 
               "-", "[System]", cyl_range, "-");
    }

    for (int i = 0; i < part_count; i++) {
        if (parts[i].low > track_cyl) {
            uint32_t free_low = track_cyl;
            uint32_t free_high = parts[i].low - 1;
            uint32_t free_cyls = free_high - free_low + 1;
            uint64_t free_bytes = (uint64_t)free_cyls * cyl_size_bytes;
            total_free_bytes += free_bytes;

            char sz_str[32], cyl_range[32];
            snprintf(cyl_range, sizeof(cyl_range), "%u - %u", free_low, free_high);

            if (free_bytes >= 1024ULL*1024*1024) {
                snprintf(sz_str, sizeof(sz_str), "%.2f GB", (double)free_bytes / (1024.0*1024*1024));
            } else {
                snprintf(sz_str, sizeof(sz_str), "%.2f MB", (double)free_bytes / (1024.0*1024));
            }

            printf(" %-4s | %-12s | %-16s | %-14s | %u Zylinder unbelegt\n", 
                   "-", "[FREI]", cyl_range, sz_str, free_cyls);
        }

        uint32_t part_cyls = parts[i].high - parts[i].low + 1;
        uint64_t part_bytes = (uint64_t)part_cyls * cyl_size_bytes;
        char sz_str[32], cyl_range[32], num_str[8], fs_str[16];
        
        snprintf(cyl_range, sizeof(cyl_range), "%u - %u", parts[i].low, parts[i].high);
        snprintf(num_str, sizeof(num_str), "#%d", i + 1);
        get_dostype_string(parts[i].dostype, fs_str, sizeof(fs_str));

        if (part_bytes >= 1024ULL*1024*1024) {
            snprintf(sz_str, sizeof(sz_str), "%.2f GB", (double)part_bytes / (1024.0*1024*1024));
        } else {
            snprintf(sz_str, sizeof(sz_str), "%.2f MB", (double)part_bytes / (1024.0*1024));
        }

        printf(" %-4s | %-12s | %-16s | %-14s | Name: %-8s (DosType: 0x%08X)\n", 
               num_str, fs_str, cyl_range, sz_str, parts[i].name, parts[i].dostype);

        if (parts[i].high + 1 > track_cyl) {
            track_cyl = parts[i].high + 1;
        }
    }

    if (track_cyl <= rdb_high_cyl) {
        uint32_t free_low = track_cyl;
        uint32_t free_high = rdb_high_cyl;
        uint32_t free_cyls = free_high - free_low + 1;
        uint64_t free_bytes = (uint64_t)free_cyls * cyl_size_bytes;
        total_free_bytes += free_bytes;

        char sz_str[32], cyl_range[32];
        snprintf(cyl_range, sizeof(cyl_range), "%u - %u", free_low, free_high);

        if (free_bytes >= 1024ULL*1024*1024) {
            snprintf(sz_str, sizeof(sz_str), "%.2f GB", (double)free_bytes / (1024.0*1024*1024));
        } else {
            snprintf(sz_str, sizeof(sz_str), "%.2f MB", (double)free_bytes / (1024.0*1024));
        }

        printf(" %-4s | %-12s | %-16s | %-14s | %u Zylinder unbelegt\n", 
               "-", "[FREI]", cyl_range, sz_str, free_cyls);
    }

    printf("-----------------------------------------------------------------------------------\n");
    if (total_free_bytes >= 1024ULL*1024*1024) {
        printf(" GESAMT FREIER SPEICHER: %.2f GB (%.2f MB)\n", 
               (double)total_free_bytes / (1024.0*1024*1024),
               (double)total_free_bytes / (1024.0*1024));
    } else {
        printf(" GESAMT FREIER SPEICHER: %.2f MB\n", (double)total_free_bytes / (1024.0*1024));
    }
    printf("==================================================================================-\n");

    return 0;
}

static int do_mklabel(int fd, const char *device, const char *label_type, int force_flag) {
    if (strcasecmp(label_type, "rdb") != 0 && strcasecmp(label_type, "amiga") != 0) {
        fprintf(stderr, "[-] Ungültiges Label: '%s'. Unterstützte Labels: 'rdb' oder 'amiga'\n", label_type);
        return 1;
    }

    if (!force_flag) {
        fprintf(stderr, "[!] WARNUNG: 'mklabel' überschreibt den gesamten RDB-Bereich (Sektor 0-63) auf '%s'!\n", device);
        fprintf(stderr, "    Bestehende Partitionen und Daten gehen dabei unwiderruflich verloren.\n");
        fprintf(stderr, "    Möchten Sie fortfahren? [y/N]: ");
        
        char answer[16];
        if (fgets(answer, sizeof(answer), stdin) != NULL) {
            if (answer[0] != 'y' && answer[0] != 'Y') {
                printf("[-] Abgebrochen.\n");
                return 0;
            }
        } else {
            printf("[-] Abgebrochen.\n");
            return 0;
        }
    }

    uint64_t total_bytes = get_disk_size(fd);
    if (total_bytes == 0) {
        fprintf(stderr, "[-] Fehler: Konnte Festplattengröße von '%s' nicht bestimmen.\n", device);
        return 1;
    }
    lseek(fd, 0, SEEK_SET);

    uint32_t heads = 16;
    uint32_t sectors_per_track = 63;
    uint32_t cyl_sectors = heads * sectors_per_track;
    uint64_t total_sectors = total_bytes / SECTOR_SIZE;
    uint32_t cylinders = (uint32_t)(total_sectors / cyl_sectors);

    if (cylinders < 4) {
        fprintf(stderr, "[-] Datenträger '%s' ist zu klein für eine RDB-Partitionstabelle.\n", device);
        return 1;
    }

    uint8_t zero_block[SECTOR_SIZE];
    memset(zero_block, 0, SECTOR_SIZE);
    for (int i = 0; i < RDB_SECTORS_RESERVED; i++) {
        write(fd, zero_block, SECTOR_SIZE);
    }

    uint8_t rdsk[SECTOR_SIZE];
    memset(rdsk, 0, SECTOR_SIZE);

    write_be32(&rdsk[0], ID_RDSK);             /* rdb_ID */
    write_be32(&rdsk[4], 64);                  /* rdb_SumLongs */
    write_be32(&rdsk[12], 7);                  /* rdb_HostID */
    write_be32(&rdsk[16], SECTOR_SIZE);        /* rdb_BlockBytes */
    write_be32(&rdsk[24], 0xFFFFFFFF);         /* rdb_BadBlockList */
    write_be32(&rdsk[28], 0xFFFFFFFF);         /* rdb_PartitionList */
    write_be32(&rdsk[32], 0xFFFFFFFF);         /* rdb_FileSysHeaderList */
    write_be32(&rdsk[36], 0xFFFFFFFF);         /* rdb_DriveInit */

    write_be32(&rdsk[64], cylinders);          /* rdb_Cylinders */
    write_be32(&rdsk[68], sectors_per_track);  /* rdb_Sectors */
    write_be32(&rdsk[72], heads);              /* rdb_Heads */
    write_be32(&rdsk[76], 0);                  /* rdb_Interleave */
    write_be32(&rdsk[80], 0);                  /* rdb_ParkCyl */
    write_be32(&rdsk[96], 0);                  /* rdb_WritePreComp */
    write_be32(&rdsk[100], 0);                 /* rdb_ReducedWrite */
    write_be32(&rdsk[104], 0);                 /* rdb_StepRate */
    
    write_be32(&rdsk[136], 2);                 /* rdb_LoCylinder */
    write_be32(&rdsk[140], cylinders - 1);     /* rdb_HiCylinder */
    write_be32(&rdsk[144], cyl_sectors);       /* rdb_CylBlocks */
    write_be32(&rdsk[148], 0);                 /* rdb_AutoParkSeconds */
    write_be32(&rdsk[152], RDB_SECTORS_RESERVED - 1); /* rdb_HighRDSKBlock */
    write_be32(&rdsk[156], 0);                 /* rdb_Reserved4 */

    memcpy(&rdsk[160], "GENERIC ", 8);         /* rdb_DiskVendor */
    memcpy(&rdsk[168], "AMIGA RDB DISK  ", 16);  /* rdb_DiskProduct */
    memcpy(&rdsk[184], "1.0 ", 4);             /* rdb_DiskRevision */

    update_rdb_checksum(rdsk);

    lseek(fd, 0, SEEK_SET);
    if (write(fd, rdsk, SECTOR_SIZE) != SECTOR_SIZE) {
        perror("[-] Fehler beim Schreiben des RDB-Headers");
        return 1;
    }

    fsync(fd);
    double size_gb = (double)total_bytes / (1024.0 * 1024.0 * 1024.0);
    printf("[+] Neues Amiga RDB Disklabel auf '%s' erstellt (%.2f GB, %u Zylinder).\n", device, size_gb, cylinders);
    return 0;
}

static int do_mkpart(int fd, const char *device, const char *part_name, 
                     const char *fs_str, const char *start_str, const char *end_str) {
    uint32_t dostype = resolve_dostype(fs_str);
    if (dostype == 0) {
        fprintf(stderr, "[-] Unbekanntes Dateisystem / DosType: '%s'\n", fs_str);
        return 1;
    }

    uint64_t total_disk_bytes = get_disk_size(fd);
    if (total_disk_bytes == 0) {
        fprintf(stderr, "[-] Fehler: Konnte Festplattengröße von '%s' nicht bestimmen.\n", device);
        return 1;
    }
    lseek(fd, 0, SEEK_SET);

    uint8_t rdsk[SECTOR_SIZE];
    int rdsk_sec = -1;
    for (int i = 0; i < RDB_SECTORS_RESERVED; i++) {
        if (read(fd, rdsk, SECTOR_SIZE) != SECTOR_SIZE) break;
        if (read_be32(&rdsk[0]) == ID_RDSK) {
            rdsk_sec = i;
            break;
        }
    }

    if (rdsk_sec == -1) {
        fprintf(stderr, "[-] Kein Amiga RDB auf '%s' gefunden. Bitte zuerst 'mklabel rdb' ausführen.\n", device);
        return 1;
    }

    uint32_t sectors_per_track = read_be32(&rdsk[68]);
    uint32_t surfaces          = read_be32(&rdsk[72]);
    uint32_t rdb_low_cyl       = read_be32(&rdsk[136]);
    uint32_t rdb_high_cyl      = read_be32(&rdsk[140]);

    if (surfaces == 0 || sectors_per_track == 0) {
        fprintf(stderr, "[-] Ungültige RDB-Geometrie auf '%s'.\n", device);
        return 1;
    }

    uint32_t secs_per_cyl = surfaces * sectors_per_track;
    uint64_t cyl_size_bytes = (uint64_t)secs_per_cyl * SECTOR_SIZE;

    uint32_t first_part = read_be32(&rdsk[28]);
    
    PartInfo parts[64];
    int part_count = 0;
    uint32_t curr_p = first_part;
    uint8_t tmp_block[SECTOR_SIZE];
    while (curr_p != 0xFFFFFFFF && curr_p != 0 && part_count < 64) {
        lseek(fd, (off_t)(curr_p * SECTOR_SIZE), SEEK_SET);
        if (read(fd, tmp_block, SECTOR_SIZE) != SECTOR_SIZE) break;
        if (read_be32(&tmp_block[0]) == ID_PART) {
            parts[part_count].low = read_be32(&tmp_block[164]);
            parts[part_count].high = read_be32(&tmp_block[168]);
            part_count++;
        }
        curr_p = read_be32(&tmp_block[16]);
    }
    qsort(parts, part_count, sizeof(PartInfo), compare_parts);

    uint32_t low_cyl = 0, high_cyl = 0;

    if (strcmp(start_str, "+") == 0 || strcasecmp(start_str, "auto") == 0 || strcasecmp(start_str, "next") == 0) {
        uint32_t candidate_low = rdb_low_cyl;
        int found_gap = 0;

        for (int i = 0; i <= part_count; i++) {
            uint32_t candidate_high = (i < part_count) ? (parts[i].low - 1) : rdb_high_cyl;

            if (candidate_low <= candidate_high) {
                uint32_t free_cyls = candidate_high - candidate_low + 1;
                uint64_t free_bytes = (uint64_t)free_cyls * cyl_size_bytes;

                uint32_t cyls_needed = 0;
                int has_unit = 0;

                const char *val_str = (end_str[0] == '+') ? end_str + 1 : end_str;
                char *endptr;
                double val = strtod(val_str, &endptr);

                if (*endptr == '%') {
                    uint64_t target_bytes = (uint64_t)((val / 100.0) * free_bytes);
                    cyls_needed = (uint32_t)((target_bytes + cyl_size_bytes - 1) / cyl_size_bytes);
                    if (cyls_needed == 0) cyls_needed = 1;
                    if (cyls_needed > free_cyls) cyls_needed = free_cyls;

                    low_cyl = candidate_low;
                    high_cyl = low_cyl + cyls_needed - 1;
                    found_gap = 1;
                    break;
                } else {
                    if (end_str[0] == '+') {
                        uint64_t rel_val = parse_size_to_bytes(end_str + 1, total_disk_bytes, &has_unit);
                        cyls_needed = has_unit ? (uint32_t)((rel_val + cyl_size_bytes - 1) / cyl_size_bytes) : (uint32_t)rel_val;
                    } else {
                        uint64_t end_val = parse_size_to_bytes(end_str, total_disk_bytes, &has_unit);
                        if (has_unit) {
                            uint32_t target_cyl = (uint32_t)(end_val / cyl_size_bytes);
                            cyls_needed = (target_cyl > candidate_low) ? (target_cyl - candidate_low) : 1;
                        } else {
                            cyls_needed = (uint32_t)end_val;
                        }
                    }
                    if (cyls_needed == 0) cyls_needed = 1;

                    if (free_cyls >= cyls_needed) {
                        low_cyl = candidate_low;
                        high_cyl = low_cyl + cyls_needed - 1;
                        found_gap = 1;
                        break;
                    }
                }
            }
            if (i < part_count) {
                candidate_low = parts[i].high + 1;
            }
        }

        if (!found_gap) {
            fprintf(stderr, "[-] FEHLER: Keine freie Lücke gefunden, die groß genug für die angeforderte Größe ist!\n");
            return 1;
        }
    } else {
        if (!calculate_cylinders(start_str, end_str, cyl_size_bytes, total_disk_bytes, &low_cyl, &high_cyl)) {
            return 1;
        }
    }

    if (low_cyl < rdb_low_cyl) {
        fprintf(stderr, "[-] FEHLER: Start-Zylinder %u liegt im reservierten RDB-Bereich (< %u)!\n", low_cyl, rdb_low_cyl);
        return 1;
    }

    if (high_cyl > rdb_high_cyl) {
        fprintf(stderr, "[-] FEHLER: End-Zylinder %u überschreitet das Datenträgerende (max. %u)!\n", high_cyl, rdb_high_cyl);
        return 1;
    }

    uint32_t dummy_max = 0;
    if (check_partition_overlap(fd, first_part, low_cyl, high_cyl, &dummy_max) != 0) {
        return 1;
    }

    int new_part_sec = -1;
    uint8_t test_block[SECTOR_SIZE];
    for (int s = 3; s < RDB_SECTORS_RESERVED; s++) {
        lseek(fd, (off_t)(s * SECTOR_SIZE), SEEK_SET);
        read(fd, test_block, SECTOR_SIZE);
        uint32_t id = read_be32(&test_block[0]);
        if (id == 0x00000000 || id == 0xFFFFFFFF) {
            new_part_sec = s;
            break;
        }
    }

    if (new_part_sec == -1) {
        fprintf(stderr, "[-] Kein freier Sektor im RDB-Bereich (0-63) auf '%s' gefunden.\n", device);
        return 1;
    }

    uint8_t part[SECTOR_SIZE];
    memset(part, 0, SECTOR_SIZE);

    write_be32(&part[0], ID_PART);
    write_be32(&part[4], 64);
    write_be32(&part[16], 0xFFFFFFFF); /* pe_Next Ende der Kette */

    size_t name_len = strlen(part_name);
    if (name_len > 31) name_len = 31;
    part[36] = (uint8_t)name_len;
    memcpy(&part[37], part_name, name_len);

    write_be32(&part[128], 16);
    write_be32(&part[132], 128);
    write_be32(&part[140], surfaces);
    write_be32(&part[148], sectors_per_track);
    write_be32(&part[164], low_cyl);
    write_be32(&part[168], high_cyl);
    write_be32(&part[192], dostype);

    update_rdb_checksum(part);

    /* Schreibe neuen Partitionsblock zuerst */
    lseek(fd, (off_t)(new_part_sec * SECTOR_SIZE), SEEK_SET);
    write(fd, part, SECTOR_SIZE);

    /* Kette verlinken */
    if (first_part == 0xFFFFFFFF || first_part == 0) {
        write_be32(&rdsk[28], (uint32_t)new_part_sec);
        update_rdb_checksum(rdsk);
        lseek(fd, (off_t)(rdsk_sec * SECTOR_SIZE), SEEK_SET);
        write(fd, rdsk, SECTOR_SIZE);
    } else {
        uint32_t curr = first_part;
        uint32_t prev = 0;
        uint8_t curr_block[SECTOR_SIZE];
        
        while (curr != 0xFFFFFFFF && curr != 0) {
            prev = curr;
            lseek(fd, (off_t)(curr * SECTOR_SIZE), SEEK_SET);
            if (read(fd, curr_block, SECTOR_SIZE) != SECTOR_SIZE) break;
            curr = read_be32(&curr_block[16]);
        }

        if (prev != 0) {
            lseek(fd, (off_t)(prev * SECTOR_SIZE), SEEK_SET);
            read(fd, curr_block, SECTOR_SIZE);
            write_be32(&curr_block[16], (uint32_t)new_part_sec);
            update_rdb_checksum(curr_block);
            lseek(fd, (off_t)(prev * SECTOR_SIZE), SEEK_SET);
            write(fd, curr_block, SECTOR_SIZE);
        }
    }

    fsync(fd);
    printf("[+] Partition '%s' (%s - 0x%08X) in RDB-Sektor %d auf '%s' angelegt (Zylinder %u - %u)!\n", 
           part_name, fs_str, dostype, new_part_sec, device, low_cyl, high_cyl);

    return 0;
}

static void print_fs_help(void) {
    printf("===============================================================================\n");
    printf(" Unterstützte Dateisysteme / DosTypes für <fs_type>\n");
    printf("===============================================================================\n\n");
    for (int i = 0; dostype_table[i].name != NULL; i++) {
        uint32_t dt = dostype_table[i].dostype;
        char ascii[5] = {
            (char)((dt >> 24) & 0xFF),
            (char)((dt >> 16) & 0xFF),
            (char)((dt >> 8) & 0xFF),
            (char)(dt & 0xFF),
            '\0'
        };
        for (int c = 0; c < 4; c++) {
            if (ascii[c] < 32 || ascii[c] > 126) ascii[c] = '?';
        }
        printf("  %-12s -> Hex: 0x%08X  (ASCII: %s)\n", dostype_table[i].name, dt, ascii);
    }
    printf("\n");
}

static void print_units_help(void) {
    printf("===============================================================================\n");
    printf(" Einheiten & Größenangaben für <start> und <end>\n");
    printf("===============================================================================\n\n");
    printf("  - Zahl:     2 oder 500\n");
    printf("  - Relativ:  +500M, +2G\n");
    printf("  - Auto:     + oder auto (sucht automatisch die erste passende Lücke)\n");
    printf("  - Prozent:  100%% (bei Auto bezieht sich auf die Größe der freien Lücke)\n\n");
}

static void print_help(const char *prog, const char *topic) {
    if (topic != NULL) {
        if (strcasecmp(topic, "fs_type") == 0 || strcasecmp(topic, "fs") == 0) {
            print_fs_help();
            return;
        }
        if (strcasecmp(topic, "units") == 0) {
            print_units_help();
            return;
        }
    }

    printf("===============================================================================\n");
    printf(" Amiga RDB Partitioning Tool - Hilfe & Dokumentation\n");
    printf("===============================================================================\n\n");
    printf("SYNTAX:\n");
    printf("  1) Disklabel initialisieren:\n     %s <device> mklabel [rdb|amiga] [--force]\n\n", prog);
    printf("  2) Partition anlegen:\n     %s <device> mkpart <name> <fs> <start> <end>\n\n", prog);
    printf("  3) Partition löschen:\n     %s <device> rmpart <nr|name> [--force]\n\n", prog);
    printf("  4) Partition umbenennen:\n     %s <device> rename <nr|old_name> <new_name>\n\n", prog);
    printf("  5) Belegung & Freispeicher anzeigen:\n     %s <device> free\n\n", prog);
    printf("BEISPIELE:\n");
    printf("  %s /dev/hda mklabel rdb\n", prog);
    printf("  %s /dev/hda mkpart test SFS + +100M\n", prog);
    printf("  %s /dev/hda mkpart test2 SFS + +100%%\n", prog);
    printf("  %s /dev/hda rmpart 1\n", prog);
    printf("  %s /dev/hda rename DH1 Work\n", prog);
    printf("  %s /dev/hda free\n", prog);
    printf("===============================================================================\n");
}

static int is_help_flag(const char *arg) {
    return (strcasecmp(arg, "help") == 0 || 
            strcasecmp(arg, "--help") == 0 || 
            strcasecmp(arg, "-h") == 0 || 
            strcasecmp(arg, "-?") == 0);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_help(argv[0], NULL);
        return 0;
    }

    if (is_help_flag(argv[1])) {
        print_help(argv[0], (argc >= 3) ? argv[2] : NULL);
        return 0;
    }

    if (argc >= 3 && is_help_flag(argv[2])) {
        print_help(argv[0], argv[1]);
        return 0;
    }

    if (strcasecmp(argv[1], "rename") == 0 || 
        strcasecmp(argv[1], "rmpart") == 0 || 
        strcasecmp(argv[1], "mkpart") == 0 || 
        strcasecmp(argv[1], "mklabel") == 0) {
        fprintf(stderr, "[-] FEHLER: Das Gerät (<device>) fehlt an erster Stelle!\n");
        return 1;
    }

    if (argc < 3) {
        fprintf(stderr, "[-] FEHLER: Zu wenige Argumente übergeben.\n");
        return 1;
    }

    const char *device = argv[1];
    const char *cmd = argv[2];

    if (strcasecmp(argv[1], "free") == 0 && argc >= 3) {
        device = argv[2];
        cmd = argv[1];
    }

    int fd = open(device, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "[-] FEHLER: Konnte Gerät '%s' nicht öffnen: %s\n", device, strerror(errno));
        return 1;
    }

    int res = 0;

    if (strcasecmp(cmd, "free") == 0) {
        res = do_free(fd, device);
    } else if (strcasecmp(cmd, "mklabel") == 0) {
        const char *label_type = "rdb";
        int force_force = 0;
        for (int i = 3; i < argc; i++) {
            if (strcasecmp(argv[i], "--force") == 0 || strcasecmp(argv[i], "-f") == 0) {
                force_force = 1;
            } else {
                label_type = argv[i];
            }
        }
        res = do_mklabel(fd, device, label_type, force_force);
    } else if (strcasecmp(cmd, "rmpart") == 0) {
        int force_flag = 0;
        const char *target = NULL;
        for (int i = 3; i < argc; i++) {
            if (strcasecmp(argv[i], "--force") == 0 || strcasecmp(argv[i], "-f") == 0) {
                force_flag = 1;
            } else {
                target = argv[i];
            }
        }
        if (target == NULL) {
            fprintf(stderr, "[-] FEHLER: Zu wenige Argumente für 'rmpart'. Syntax: %s <device> rmpart <nr|name> [--force]\n", argv[0]);
            res = 1;
        } else {
            res = do_rmpart(fd, device, target, force_flag);
        }
    } else if (strcasecmp(cmd, "rename") == 0) {
        if (argc < 5) {
            fprintf(stderr, "[-] FEHLER: Zu wenige Argumente für 'rename'. Syntax: %s <device> rename <nr|old_name> <new_name>\n", argv[0]);
            res = 1;
        } else {
            res = do_rename(fd, device, argv[3], argv[4]);
        }
    } else if (strcasecmp(cmd, "mkpart") == 0) {
        if (argc < 7) {
            fprintf(stderr, "[-] FEHLER: Zu wenige Argumente für 'mkpart'. Syntax: %s <device> mkpart <name> <fs> <start> <end>\n", argv[0]);
            res = 1;
        } else {
            res = do_mkpart(fd, device, argv[3], argv[4], argv[5], argv[6]);
        }
    } else {
        if (argc < 6) {
            fprintf(stderr, "[-] FEHLER: Unbekannter Befehl: '%s'\n", cmd);
            res = 1;
        } else {
            res = do_mkpart(fd, device, argv[2], argv[3], argv[4], argv[5]);
        }
    }

    close(fd);
    return res;
}

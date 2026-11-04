
#ifndef CRTPARSER_H
#define CRTPARSER_H

#include <cstring>
#include <cstdio>

#define NEXT_CHIP    2

// EasyFlash bank packing (see CRTParser::parseEasyFlashChip)
#define EF_BANKS        64
#define EF_MAP_SIZE     (2 * EF_BANKS)
#define EF_CHUNK_SIZE   (8 * 1024)
#define EF_CHUNK_NONE   0xFF

struct ChipBlock {
   uint8_t type;              // 0:ROM, 1:RAM, 2:FLASH
   uint8_t number;
   uint16_t startAddress;
   uint16_t length;
};

/*
 * class CRTParser
 * constructor accepts IDataReader (file or buffer), buffer for banks data, size of buffer
 */
class CRTParser {

public:
   CRTParser(IDataReader& reader, uint8_t *crt_buf, uint32_t crt_buf_size, uint8_t *crt_map, uint8_t crt_map_size) : 
      reader(reader), crt_buf(crt_buf), crt_buf_size(crt_buf_size), crt_map(crt_map), crt_map_size(crt_map_size) {}

   char* getName(void) {
      return name;
   }

   uint16_t getType(void) {
      return type;
   }

   bool getExrom(void) {
      return exrom;
   }

   bool getGame(void) {
      return game;
   }

   // hardware subtype (CRT version 1.1+), e.g. RGCD: 0 RGCD, 1 Hucky
   uint8_t getSubtype(void) {
      return subtype;
   }

   uint32_t getSize(void) {
      return size;
   }

   bool parse(void) {
      if (!parseHeader()) 
         return false;

      while (true) {

         // parse chip blocks
         uint8_t res = parseChipBlock();

         if (res == NEXT_CHIP)
            continue;
         else if (res == true)
            break;
         else return false;
      }

      // Ocean: the header is often wrong. Images with ROMH chips or larger
      // than 128K (256K, e.g. SCI) need 16K mode, the others (e.g.
      // Wonderboy) are 8K
      if (type == 5) {
         exrom = 0;
         game = (has_romh || size > 128*1024) ? 0 : 1;
      }

      return true;
   }

   void print(void) {

      printf("name: %s\n", name);
      printf("hardware type: %d\n", type);
      printf("exrom: %d, game: %d\n", exrom, game);

      printf("chips: %d, size: %lu\n", chip_count, (unsigned long)size);
   }

private:
   IDataReader& reader;

   uint16_t type;
   uint8_t subtype = 0;
   bool exrom, game;
   char name[33];
   uint32_t size = 0;

   // number of chip blocks (no list kept: heap is small)
   uint16_t chip_count = 0;

   // at least one chip is mapped to ROMH ($A000/$E000)
   bool has_romh = false;

   uint8_t *crt_buf;
   uint32_t crt_buf_size;
   uint8_t *crt_map;
   uint8_t crt_map_size;

   // EasyFlash packing state
   bool ef_init = false;
   uint8_t ef_chunks = 0;
 
   bool parseHeader(void) {

      uint8_t buf[50];

      if (!reader.read(buf, 16))
         return false;
      if (memcmp(buf, "C64 CARTRIDGE   ", 16) != 0)
         return false;

      // file header length, cartridge version
      if(!reader.read(buf, 6))
         return false;
      uint16_t version = (buf[4] << 8) | buf[5];

      if(!reader.read(buf, 2))
         return false;
      type = (buf[0] << 8) | buf[1];

      if(!reader.read(buf, 2))
         return false;
      exrom = bool(buf[0]);
      game = bool(buf[1]);

      // Ocean: EXROM/GAME are set after the chips are parsed (see parse())
      if(type == 32) {   // fix EasyFlash bad EXROM/GAME
         exrom = 1;
         game =0;
      } else if(type == 7 || type == 19) {
         // Fun Play, Magic Desk: always start in 8K mode
         exrom = 0;
         game = 1;
      } else if(type == 18) {
         // Zaxxon: always 16K mode
         exrom = game = 0;
      } else if(type == 15 || type == 43 || type == 57 || type == 12 || type == 60) {
         // C64 Game System, Prophet64, RGCD, Rex Utility, GMod2: 8K mode
         exrom = 0;
         game = 1;
      } else if(type == 11 || type == 13 || type == 16 || type == 21) {
         // Westermann, Final Cartridge I, Warp Speed, Comal-80: 16K mode
         exrom = game = 0;
      }

      // reserved bytes, the first one is the hardware subtype (CRT 1.1+)
      if(!reader.read(buf, 6))
         return false;
      if (version >= 0x0101)
         subtype = buf[0];

      if(!reader.read(buf, 32))
         return false;
      memcpy(name, buf, 32);
      name[32] = '\0';

      return true;
   }

   uint8_t parseChipBlock(void) {

      if (reader.eof())
         return true;

      uint8_t buf[50];

      if (!reader.read(buf, 4)) 
         return false;
      if (memcmp(buf, "CHIP", 4) != 0)
         return false;

      // skip total packet length
      if (!reader.read(buf, 4))
         return false;

      if (!reader.read(buf, 8)) 
         return false;

      uint16_t chipType = (buf[0] << 8) | buf[1];
      uint8_t number = (buf[2] << 8) | buf[3];
      uint16_t startAddr = (buf[4] << 8) | buf[5];
      uint16_t blockLen = (buf[6] << 8) | buf[7];

      if (type == 32) {
         if (!parseEasyFlashChip(number, startAddr, blockLen))
            return false;
         addChip(chipType, number, startAddr, blockLen);
         return NEXT_CHIP;
      }

      // 8K banks packed as chunks (see parse8KPackedChip): Ocean, C64 Game
      // System, GMod2 (64 banks, 512K), Magic Desk (128 banks, 1 MB)
      if (type == 5 || type == 15 || type == 19 || type == 60) {
         if (type == 5 && startAddr == 0xA000)
            has_romh = true;      // Ocean: 16K mode
         if (!parse8KPackedChip(number, startAddr, blockLen, type == 19 ? 2 * EF_BANKS : EF_BANKS))
            return false;
         addChip(chipType, number, startAddr, blockLen);
         return NEXT_CHIP;
      }

      // 8K banks selected by a register: Prophet64 (32), RGCD/Hucky (8)
      if (type == 43 || type == 57) {
         uint8_t max_banks = (type == 43) ? 32 : 8;
         if (!parse8KBankChip(number, startAddr, blockLen, max_banks))
            return false;
         addChip(chipType, number, startAddr, blockLen);
         return NEXT_CHIP;
      }

      // calculate offset inside memory buffer
      uint32_t ofs = 0;

      if(startAddr == 0x8000 && blockLen <= 16*1024) {

         // a 16K chip at $8000 also covers ROMH
         if (blockLen > 8*1024)
            has_romh = true;

         // Support ROML only cartridges with more than 64 banks
         if(blockLen<= 8*1024 && (type == 7) ) {

            // Fun Play
            ofs = (( (number >> 3) & 0x07 ) | ( (number & 0x01) << 3)) * 16*1024;

         } else if(blockLen <= 8*1024 && (type == 19) ) {

            // Magic Desk
            bool odd_bank = number & 1;
            number >>= 1;
            ofs = number * 16*1024;

            // Use ROMH bank location for odd banks
            if(odd_bank)
               ofs += 8*1024;
      
         } else {

            ofs = number * 16*1024;
         }

      // ROMH bank
      } else if((startAddr == 0xa000 || startAddr == 0xe000) && blockLen <= 8*1024 ) {

         has_romh = true;
         ofs = number * 16*1024 + 8*1024;

      } else if(startAddr == 0xf000 && blockLen <= 4*1024 ) {

         ofs = number * 16*1024 + 8*1024;
      }

      //printf("bank %d, load_addr: 0x%X, ofs: %lu\n", number, startAddr, ofs);
      
      // 4K images are mirrored to 8K
      uint32_t span = (blockLen <= 4*1024) ? 8*1024 : blockLen;
      if (ofs + span > crt_buf_size) {
         printf("E: CRT too large for buffer (bank %d, offset %lu)\n", number, (unsigned long)ofs);
         return false;
      }

      if (!reader.read(crt_buf+ofs, blockLen)) 
         return false;

      // mirror 4K image
      if (blockLen <= 4*1024)
         memcpy(crt_buf+ofs+(4*1024), crt_buf+ofs, 4*1024);

      addChip(chipType, number, startAddr, blockLen);

      return NEXT_CHIP;
   }

   void addChip(uint16_t chipType, uint8_t number, uint16_t startAddr, uint16_t blockLen) {

      (void)chipType;
      (void)number;
      (void)startAddr;
      chip_count++;
      size += blockLen;
   }

   /*
    * Cartridges with 8K banks at ROML selected by a register (C64 Game System,
    * Prophet64, RGCD): bank n at crt_buf + n * 8K. The load address in the
    * CRT is not meaningful (RGCD images use $8000 or $A000).
    */
   bool parse8KBankChip(uint8_t number, uint16_t startAddr, uint16_t blockLen, uint8_t max_banks) {

      if (blockLen != 8*1024 || number >= max_banks ||
          (startAddr != 0x8000 && startAddr != 0xA000)) {
         printf("E: chip not supported (bank %d, $%04X, %d bytes)\n", number, startAddr, blockLen);
         return false;
      }

      uint32_t ofs = (uint32_t)number * 8*1024;
      if (ofs + blockLen > crt_buf_size) {
         printf("E: CRT too large for buffer (bank %d, offset %lu)\n", number, (unsigned long)ofs);
         return false;
      }

      return reader.read(crt_buf + ofs, blockLen);
   }

   /*
    * Cartridges with 8K banks selected by a register: Ocean, C64 Game System,
    * GMod2 (up to 64 banks, 512K), Magic Desk (up to 128, 1 MB). The chips are
    * packed as 8K chunks like EasyFlash ROML, so that images padded with
    * erased or repeated banks fit in crt_buf, and the same chunk cache can
    * be used when the image is in the PSRAM:
    *   crt_map[bank] -> chunk index (crt_buf + n * 8K), EF_CHUNK_NONE: erased
    * An erased (all $FF) bank takes no chunk, a bank equal to a previous one
    * shares its chunk (not in the PSRAM: comparing would take too long).
    * getEasyFlashChunks() returns the chunks used. The load address in the
    * CRT is not meaningful (Ocean 256K images have banks 16-31 either at
    * $8000 or at $A000). A 16K chip at $8000 holds banks 2n and 2n + 1
    * (Magic Desk images converted from 16K banks).
    */
   bool parse8KPackedChip(uint8_t number, uint16_t startAddr, uint16_t blockLen, uint32_t max_banks) {

      uint32_t first = number, count = 1;
      if (blockLen == 16*1024 && startAddr == 0x8000) {
         first = (uint32_t)number * 2;
         count = 2;
      } else if (blockLen != 8*1024 || (startAddr != 0x8000 && startAddr != 0xA000)) {
         first = max_banks;      // not supported
      }
      if (first + count > max_banks || crt_map_size < max_banks) {
         printf("E: chip not supported (bank %d, $%04X, %d bytes)\n", number, startAddr, blockLen);
         return false;
      }

      if (!ef_init) {
         memset(crt_map, EF_CHUNK_NONE, crt_map_size);
         ef_chunks = 0;
         ef_init = true;
      }

      for (uint32_t b = first; b < first + count; b++) {
         if (!pack8KChunk(b))
            return false;
      }
      return true;
   }

   // read 8K into the next chunk, map it to bank (see parse8KPackedChip)
   bool pack8KChunk(uint32_t bank) {

      const uint32_t len = 8*1024;

      // buffer full: only an erased bank can still be accepted
      if ((uint32_t)(ef_chunks + 1) * EF_CHUNK_SIZE > crt_buf_size) {
         uint8_t part[64];
         for (uint32_t ofs = 0; ofs < len; ofs += sizeof(part)) {
            if (!reader.read(part, sizeof(part)))
               return false;
            for (uint8_t i = 0; i < sizeof(part); i++) {
               if (part[i] != 0xFF) {
                  printf("E: CRT too large for buffer (bank %lu)\n", (unsigned long)bank);
                  return false;
               }
            }
         }
         crt_map[bank] = EF_CHUNK_NONE;
         return true;
      }

      uint8_t *dst = crt_buf + (uint32_t)ef_chunks * EF_CHUNK_SIZE;
      if (!reader.read(dst, len))
         return false;

      // erased bank
      uint32_t i = 0;
      while (i < len && dst[i] == 0xFF)
         i++;
      if (i == len) {
         crt_map[bank] = EF_CHUNK_NONE;
         return true;
      }

      // same data as a previous bank (crt_buf in SRAM only)
      if ((uintptr_t)crt_buf >= 0x20000000u) {
         for (uint8_t c = 0; c < ef_chunks; c++) {
            if (memcmp(crt_buf + (uint32_t)c * EF_CHUNK_SIZE, dst, len) == 0) {
               crt_map[bank] = c;
               return true;
            }
         }
      }

      crt_map[bank] = ef_chunks++;
      return true;
   }

   /*
    * EasyFlash
    *
    * Up to 64 banks, each one with an optional 8K ROML and 8K ROMH chip.
    * Most images use only ROML in many banks, so chips are packed in crt_buf
    * as 8K chunks in load order, instead of one 16K slot per bank.
    *
    * crt_map layout (EF_MAP_SIZE bytes):
    *   crt_map[bank]                -> ROML chunk index
    *   crt_map[EF_BANKS + bank]     -> ROMH chunk index
    *   EF_CHUNK_NONE                -> chip not present
    *
    * chunk n is at crt_buf + n * EF_CHUNK_SIZE
    */
   bool parseEasyFlashChip(uint8_t number, uint16_t startAddr, uint16_t blockLen) {

      if (crt_map_size < EF_MAP_SIZE) {
         printf("E: EasyFlash map too small\n");
         return false;
      }

      if (number >= EF_BANKS) {
         printf("E: EasyFlash bank %d out of range\n", number);
         return false;
      }

      if (!ef_init) {
         memset(crt_map, EF_CHUNK_NONE, EF_MAP_SIZE);
         ef_chunks = 0;
         ef_init = true;
      }

      // a 16K chip at $8000 fills both ROML and ROMH
      uint8_t parts = 1;
      uint8_t *slot;

      if (startAddr == 0x8000 && blockLen <= 8*1024) {
         slot = &crt_map[number];
      } else if (startAddr == 0x8000 && blockLen <= 16*1024) {
         slot = &crt_map[number];
         parts = 2;
      } else if ((startAddr == 0xa000 || startAddr == 0xe000) && blockLen <= 8*1024) {
         slot = &crt_map[EF_BANKS + number];
      } else if (startAddr == 0xf000 && blockLen <= 4*1024) {
         slot = &crt_map[EF_BANKS + number];
      } else {
         printf("E: EasyFlash chip bank %d at 0x%X len 0x%X not supported\n", number, startAddr, blockLen);
         return false;
      }

      for (uint8_t i = 0; i < parts; i++) {

         uint16_t len = (parts == 2) ? 8*1024 : blockLen;
         uint8_t *map = (i == 0) ? slot : &crt_map[EF_BANKS + number];

         // reuse the chunk if the same chip appears twice
         uint8_t chunk = *map;
         if (chunk == EF_CHUNK_NONE) {
            if ((uint32_t)(ef_chunks + 1) * EF_CHUNK_SIZE > crt_buf_size) {
               printf("E: CRT too large for buffer (bank %d)\n", number);
               return false;
            }
            chunk = ef_chunks++;
            *map = chunk;
         }

         uint8_t *dst = crt_buf + (uint32_t)chunk * EF_CHUNK_SIZE;
         if (!reader.read(dst, len))
            return false;

         // startAddr $F000: 4K image mirrored to 8K (ROMH $E000-$FFFF)
         if (len <= 4*1024)
            memcpy(dst + 4*1024, dst, 4*1024);
      }

      return true;
   }

public:
   // number of 8K chunks used by an EasyFlash image (valid after parse())
   uint8_t getEasyFlashChunks(void) {
      return ef_chunks;
   }

private:
};

#endif

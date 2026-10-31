
#ifndef CRTPARSER_H
#define CRTPARSER_H

#include <vector>
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
      return true;
   }

   void print(void) {

      printf("name: %s\n", name);
      printf("hardware type: %d\n", type);
      printf("exrom: %d, game: %d\n", exrom, game);

      for(uint8_t i=0; i<chips.size(); i++) {
         printf("bank: %d\n", chips[i].number);
         printf("\ttype: %d\n", chips[i].type);
         printf("\tstart address: 0x%X\n", chips[i].startAddress);
         printf("\tlength: 0x%X\n", chips[i].length);
      }
   }

private:
   IDataReader& reader;

   uint16_t type;
   bool exrom, game;
   char name[33];
   uint32_t size = 0;

   std::vector<ChipBlock> chips;

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

      // skip file header length and cartridge version
      if(!reader.read(buf, 6))
         return false;

      if(!reader.read(buf, 2))
         return false;
      type = (buf[0] << 8) | buf[1];

      if(!reader.read(buf, 2))
         return false;
      exrom = bool(buf[0]);
      game = bool(buf[1]);

      // fix Ocean bad EXROM/GAME
      if(type == 5) {
         exrom = game = 0;
      } else if(type == 32) {   // fix EasyFlash bad EXROM/GAME
         exrom = 1;
         game =0;
      }
      
      // skip reserved bytes
      if(!reader.read(buf, 6))
         return false;

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

      // calculate offset inside memory buffer
      uint32_t ofs = 0;

      if(startAddr == 0x8000 && blockLen <= 16*1024) {

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

         if (type == 5) {
            // Ocean
            ofs = (number % 16) * 16*1024 + 8*1024;
         } else {
            ofs = number * 16*1024 + 8*1024;
         }

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

      ChipBlock chip;
      chip.type = chipType;
      chip.number = number;
      chip.startAddress = startAddr;
      chip.length = blockLen;

      chips.push_back(chip);
      size += chip.length;
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

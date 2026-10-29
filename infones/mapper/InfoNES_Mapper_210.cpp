/* Mapper 210: explicit NES 2.0 Namco 175/340, without N163 IRQ/audio. */
static unsigned Map210_RamBytes;
static BYTE Map210_WriteEnable;

static BYTE Map210_ReadSram(WORD address)
{
  return Map210_RamBytes ? SRAM[address & (Map210_RamBytes - 1)] : address >> 8;
}

static void Map210_Sram(WORD address, BYTE data)
{
  if (Map210_RamBytes && Map210_WriteEnable) {
    SRAM[address & (Map210_RamBytes - 1)] = data;
    SRAMwritten = true;
  }
}

void Map210_Init()
{
  Map0_Init();
  MapperInit = Map210_Init;
  MapperWrite = Map210_Write;
  MapperReadSram = Map0_ReadApu;
  MapperSramOwnsWrite = true;
  Map210_RamBytes = 0;
  Map210_WriteEnable = 0;
  if (SubMapperNo == 1) {
    BYTE sizes = NesHeader.byReserve[2];
    Map210_RamBytes = (sizes & 15 ? 64u << (sizes & 15) : 0) +
                     (sizes >> 4 ? 64u << (sizes >> 4) : 0);
    MapperReadSram = Map210_ReadSram;
    MapperSram = Map210_Sram;
  }
  ROMBANK2 = ROMLASTPAGE(1);
  ROMBANK3 = ROMLASTPAGE(0);
  InfoNES_Mirroring(SubMapperNo == 1 ? ROM_Mirroring : 3);
}

void Map210_Write(WORD address, BYTE data)
{
  const WORD reg = address & 0xf800;
  if (reg >= 0x8000 && reg <= 0xb800) {
    PPUBANK[(reg - 0x8000) / 0x800] = VROMPAGE(data % (NesHeader.byVRomSize * 8));
    InfoNES_SetupChr();
  } else if (reg >= 0xc000 && reg <= 0xd800 && SubMapperNo == 1) {
    Map210_WriteEnable = data & 1;
  } else if (reg >= 0xe000 && reg <= 0xf000) {
    ROMBANK[(reg - 0xe000) / 0x800] = ROMPAGE((data & 63) % (NesHeader.byRomSize * 2));
    if (reg == 0xe000 && SubMapperNo == 2) {
      static const BYTE mirroring[] = {3, 1, 0, 2};
      InfoNES_Mirroring(mirroring[data >> 6]);
    }
  }
}

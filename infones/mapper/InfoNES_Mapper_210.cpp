/* Mapper 210.2: Namco 340. No WRAM, IRQ or N163 nametable ROM banks.
 * Namco 175 and ambiguous legacy headers are rejected by the loader. */
void Map210_Init()
{
  Map0_Init();
  MapperInit = Map210_Init;
  MapperWrite = Map210_Write;
  MapperReadSram = Map0_ReadApu;
  MapperSramOwnsWrite = true;
  ROMBANK2 = ROMLASTPAGE(1);
  ROMBANK3 = ROMLASTPAGE(0);
  InfoNES_Mirroring(3);
}

void Map210_Write(WORD address, BYTE data)
{
  const WORD reg = address & 0xf800;
  if (reg >= 0x8000 && reg <= 0xb800) {
    PPUBANK[(reg - 0x8000) / 0x800] = VROMPAGE(data % (NesHeader.byVRomSize * 8));
    InfoNES_SetupChr();
  } else if (reg >= 0xe000 && reg <= 0xf000) {
    ROMBANK[(reg - 0xe000) / 0x800] = ROMPAGE((data & 63) % (NesHeader.byRomSize * 2));
    if (reg == 0xe000) {
      static const BYTE mirroring[] = {3, 1, 0, 2};
      InfoNES_Mirroring(mirroring[data >> 6]);
    }
  }
}

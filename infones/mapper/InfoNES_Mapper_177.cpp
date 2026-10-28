/* Mapper 177: Hengge 32 KiB PRG banking with software mirroring.
   $8000-$FFFF: bits 0-4 select PRG; bit 5 selects H (1) / V (0).
   CHR RAM and the unbanked WRAM window use the ordinary core mapping. */

void Map177_Init()
{
  Map241_Init();
  MapperInit = Map177_Init;
  MapperWrite = Map177_Write;
  Map177_Write(0x8000, 0);
}

void Map177_Write(WORD address, BYTE data)
{
  if (address < 0x8000) return;
  Map241_Write(address, data & 0x1f);
  InfoNES_Mirroring(data & 0x20 ? 0 : 1);
}

/* Mapper 34: BNROM (CHR RAM) or NINA-001 (CHR ROM).
   NES 2.0 submapper/geometry is checked before entering the core. */
void Map34_Init()
{
  Map0_Init();
  MapperInit = Map34_Init;
  MapperWrite = NesHeader.byVRomSize ? Map0_Write : Map34_Write;
  MapperSram = NesHeader.byVRomSize ? Map34_Sram : Map0_Sram;
  // Keep BNROM's reset vector and startup code in one coherent last bank.
  // NINA-001 powers on at bank 0; its registers also write through to WRAM.
  MapBankPrg(0, NesHeader.byVRomSize ? 0 : (NesHeader.byRomSize << 1) - 4, 4);
}
void Map34_Write(WORD address, BYTE data)
{
  if (address >= 0x8000)
    MapBankPrg(0, unsigned(MapBusConflict(address, data)) * 4, 4);
}
void Map34_Sram(WORD address, BYTE data)
{
  switch (address) {
    case 0x7ffd: MapBankPrg(0, (data & 1) * 4, 4); break;
    case 0x7ffe: MapBankChr(0, (data & 15) * 4, 4); break;
    case 0x7fff: MapBankChr(4, (data & 15) * 4, 4); break;
  }
}

/* Mapper 80 / 207: Taito X1-005, 128-byte protected RAM. */
static bool Map80_Alt_Mirroring;
static BYTE Map80_RamPermission;

static int Map80_BlobSize() { return 5; }
static void Map80_SaveBlob(BYTE *out)
{
  for (unsigned n = 0; n < 4; ++n)
    out[n] = (BYTE)((PPUBANK[NAME_TABLE0 + n] - VRAMPAGE(0)) / 0x400);
  out[4] = Map80_RamPermission;
}
static void Map80_LoadBlob(BYTE *in)
{
  for (unsigned n = 0; n < 4; ++n)
    PPUBANK[NAME_TABLE0 + n] = VRAMPAGE(in[n] & 1);
  Map80_RamPermission = in[4];
}
static BYTE Map80_ReadSram(WORD address)
{
  return address >= 0x7f00 && Map80_RamPermission == 0xa3
    ? SRAM[address & 0x7f] : (BYTE)(address >> 8);
}

void Map80_Init()
{
  Map0_Init();
  MapperInit = Map80_Init;
  MapperSram = Map80_Sram;
  MapperReadSram = Map80_ReadSram;
  MapperSramOwnsWrite = true;
  ROMBANK2 = ROMLASTPAGE(1);
  ROMBANK3 = ROMLASTPAGE(0);
  Map80_Alt_Mirroring = false; // Alternate wiring is explicitly mapper 207.
  Map80_RamPermission = 0;
  MapperBlobSize = Map80_BlobSize;
  MapperSaveBlob = Map80_SaveBlob;
  MapperLoadBlob = Map80_LoadBlob;
}

void Map80_Sram(WORD address, BYTE data)
{
  if (address >= 0x7f00) {
    if (Map80_RamPermission == 0xa3) {
      SRAM[address & 0x7f] = data;
      SRAMwritten = true;
    }
  } else if (address == 0x7ef0 || address == 0x7ef1) {
    const unsigned pair = address & 1;
    const unsigned pages = NesHeader.byVRomSize * 8;
    PPUBANK[pair * 2] = VROMPAGE(data % pages);
    PPUBANK[pair * 2 + 1] = VROMPAGE((unsigned(data) + 1) % pages);
    if (Map80_Alt_Mirroring) {
      PPUBANK[NAME_TABLE0 + pair * 2] = VRAMPAGE(data >> 7);
      PPUBANK[NAME_TABLE1 + pair * 2] = VRAMPAGE(data >> 7);
    }
    InfoNES_SetupChr();
  } else if (address >= 0x7ef2 && address <= 0x7ef5) {
    PPUBANK[address - 0x7ef2 + 4] = VROMPAGE(data % (NesHeader.byVRomSize * 8));
    InfoNES_SetupChr();
  } else if (address == 0x7ef6 || address == 0x7ef7) {
    if (!Map80_Alt_Mirroring) InfoNES_Mirroring(data & 1 ? 1 : 0);
  } else if (address == 0x7ef8 || address == 0x7ef9) {
    Map80_RamPermission = data;
  } else if (address >= 0x7efa && address <= 0x7eff) {
    ROMBANK[(address - 0x7efa) / 2] = ROMPAGE(data % (NesHeader.byRomSize * 2));
  }
}

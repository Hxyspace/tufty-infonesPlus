#pragma once

// Helpers for discrete boards. Called only on register writes/reset, never
// from the shared CPU/PPU memory access paths. Admission validates geometry.
static void MapBankPrg(unsigned slot, unsigned page, unsigned count = 1)
{
  for (unsigned n = 0; n < count; ++n)
    ROMBANK[slot + n] = ROMPAGE((page + n) % (NesHeader.byRomSize << 1));
}
static void MapBankChr(unsigned slot, unsigned page, unsigned count = 1)
{
  for (unsigned n = 0; n < count; ++n)
    PPUBANK[slot + n] = VROMPAGE((page + n) % (NesHeader.byVRomSize << 3));
  InfoNES_SetupChr();
}
static BYTE MapBusConflict(WORD address, BYTE data)
{
  return data & ROMBANK[(address - 0x8000) >> 13][address & 0x1fff];
}

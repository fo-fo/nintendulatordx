NUM_16K_PRG_BANKS  = 2
NUM_8K_CHR_BANKS   = 1
MAPPER             = 0 ; NROM
MIRRORING          = 0 ; Horizontal

.segment "HEADER"
    .byte "NES", $1A
    .byte NUM_16K_PRG_BANKS
    .byte NUM_8K_CHR_BANKS
    .byte ( MAPPER & $F ) << 4 | MIRRORING
    .byte MAPPER & $F0

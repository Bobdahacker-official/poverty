#ifndef AVALANCHE_FONT_H
#define AVALANCHE_FONT_H
#include "avalanche.h"
#define FONT_WIDTH 18
#define FONT_HEIGHT 20
bool font_init(void);
const uint8_t *font_character(char);
unsigned font_advance(char);
const uint8_t *font_scaled(char, unsigned, unsigned *, unsigned *);
unsigned font_scaled_advance(char, unsigned);
#endif

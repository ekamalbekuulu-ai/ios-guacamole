#pragma once
#include <stdint.h>
#include "board.h"

#define RED	0xffff0000U
#define BLUE	0xff0000a0U
#define WHITE	0xffffffffU
#define YELLOW	0xffffff00U
#define GREEN	0xff00ff00U

extern volatile uint32_t *con_fb;

void con_clear(uint32_t bg);
void con_skip(void);
/* redraw one line at an explicit row (probe status/countdown) */
void con_at(int row, const char *label, const char *text);
/* current row / rows and cols that fit on the panel (probe.c adapts its report) */
int con_row(void);
int con_rows(void);
int con_cols(void);
void con_str(const char *label, const char *s);
void con_hex(const char *label, uint64_t v);
/* label, then "a=xxxxxxxx b=xxxxxxxx" style pairs of 32-bit values */
void con_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb);

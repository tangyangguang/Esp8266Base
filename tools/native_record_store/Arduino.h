#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
using PGM_P = const char*;
#define PSTR(x) (x)
void yield();
uint32_t millis();

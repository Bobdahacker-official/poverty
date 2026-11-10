#ifndef AV_CALCULATOR_H
#define AV_CALCULATOR_H
#include <stdbool.h>
#include <stddef.h>
bool calculator_eval(const char *, double, double *, const char **);
void calculator_format(char *, size_t, double);
#endif

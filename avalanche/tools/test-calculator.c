#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "calculator.h"
static void result(const char *text, double answer, double expected) {
  double value=0; const char *error;
  assert(calculator_eval(text,answer,&value,&error));
  double delta=value-expected;if(delta<0)delta=-delta;
  assert(delta<1e-10 && !error);
}
static void invalid(const char *text) {
  double value=37;const char *error;
  assert(!calculator_eval(text,0,&value,&error) && error && value==37);
}
int main(void) {
  result("2+3*4",0,14);result("(2+3)*4",0,20);
  result("-2^2",0,-4);result("2^-3",0,.125);result("2^3^2",0,512);
  result(".25 + 1.5",0,1.75);result("1.5e2 / 3",0,50);
  result("ans * 2",21,42);result("---2",0,-2);result("0^0",0,1);
  result("1e-10 * 1e10",0,1);result("2^(1+2)",0,8);
  invalid("");invalid(".");invalid("(1+2");invalid("1/0");invalid("0^-1");
  invalid("2^0.5");invalid("1e309");invalid("1e");invalid("1e+");invalid("2x");
  invalid("1e308 * 1e308");invalid("2^1025");invalid("ansx");
  char deep[200];memset(deep,'-',199);deep[199]=0;invalid(deep);
  char longtext[300];memset(longtext,'1',299);longtext[299]=0;invalid(longtext);
  char output[64];calculator_format(output,sizeof(output),42.125);
  assert(!strcmp(output,"42.125"));calculator_format(output,sizeof(output),-0.0);
  assert(!strcmp(output,"0"));
  calculator_format(output,sizeof(output),1e20);
  assert(strchr(output,'e') || strchr(output,'E'));
  result(output,0,1e20);
  puts("PASS calculator precedence, decimals, powers, answer recall, errors, bounds and formatting");
}

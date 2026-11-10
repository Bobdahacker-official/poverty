#include "calculator.h"
#include "../vendor/printf/printf.h"
struct parser { const char *at, *error; unsigned depth; double answer; };
static void space(struct parser *p) { while (*p->at==' ' || *p->at=='\t') p->at++; }
static double sum(struct parser *);
static double unary(struct parser *);
static double checked(struct parser *p, double value) {
  if (!__builtin_isfinite(value)) p->error="Result is out of range";
  return value;
}
static double primary(struct parser *p) {
  space(p);
  if (++p->depth>64) { p->error="Expression is nested too deeply"; p->depth--; return 0; }
  double v=0;
  if (*p->at=='(') {
    p->at++; v=sum(p); space(p);
    if (*p->at==')') p->at++; else p->error="Missing closing parenthesis";
  } else if (p->at[0]=='a' && p->at[1]=='n' && p->at[2]=='s') {
    p->at+=3; v=p->answer;
  } else {
    bool digits=false;
    while (*p->at>='0' && *p->at<='9') { digits=true; v=v*10+(*p->at++-'0'); }
    if (*p->at=='.') {
      double place=.1; p->at++;
      while (*p->at>='0' && *p->at<='9') { digits=true; v+=(*p->at++-'0')*place; place*=.1; }
    }
    if (!digits) p->error="Enter a number, ans, or parentheses";
    if (digits && (*p->at=='e' || *p->at=='E')) {
      p->at++; bool negative=false;
      if (*p->at=='+' || *p->at=='-') negative=*p->at++=='-';
      unsigned exponent=0;
      if (*p->at<'0' || *p->at>'9') p->error="Missing scientific exponent";
      while (*p->at>='0' && *p->at<='9') {
        if (exponent<1000) exponent=exponent*10+(*p->at-'0');
        p->at++;
      }
      if (exponent>308) p->error="Scientific exponent is out of range";
      else while (exponent--) v=negative?v/10:v*10;
    }
  }
  p->depth--; return checked(p,v);
}
static double power(struct parser *p) {
  double v=primary(p); space(p);
  if (!p->error && *p->at=='^') {
    p->at++; double exponent=unary(p);
    if (p->error) return 0;
    if (exponent < -1024 || exponent > 1024 || exponent!=(int)exponent) {
      p->error="Use a whole-number power from -1024 to 1024"; return 0;
    }
    int n=(int)exponent; bool inverse=n<0;
    if (inverse && v==0) { p->error="Division by zero"; return 0; }
    if (inverse) { v=1/v; n=-n; }
    double result=1;
    while(n) { if(n&1) result*=v; n>>=1; if(n) v*=v; }
    v=checked(p,result);
  }
  return v;
}
static double unary(struct parser *p) {
  space(p);
  if (++p->depth>64) { p->error="Expression is nested too deeply"; p->depth--; return 0; }
  double v;
  if (*p->at=='+' || *p->at=='-') {
    char sign=*p->at++; v=unary(p); if(sign=='-') v=-v;
  } else v=power(p);
  p->depth--; return v;
}
static double product(struct parser *p) {
  double v=unary(p); space(p);
  while (!p->error && (*p->at=='*' || *p->at=='/')) {
    char op=*p->at++; double right=unary(p);
    if(op=='/' && right==0) { p->error="Division by zero"; return 0; }
    v=checked(p,op=='*'?v*right:v/right); space(p);
  }
  return v;
}
static double sum(struct parser *p) {
  double v=product(p); space(p);
  while (!p->error && (*p->at=='+' || *p->at=='-')) {
    char op=*p->at++; double right=product(p);
    v=checked(p,op=='+'?v+right:v-right); space(p);
  }
  return v;
}
bool calculator_eval(const char *expression, double answer, double *result, const char **error) {
  struct parser p={.at=expression,.answer=answer};
  unsigned length=0; while(expression[length] && length<256) length++;
  if(length==256) { *error="Expression is too long"; return false; }
  double value=sum(&p); space(&p);
  if(!p.error && *p.at) p.error="Unexpected character";
  if(p.error) { *error=p.error; return false; }
  *result=value; *error=0; return true;
}
void calculator_format(char *out, size_t capacity, double value) {
  if (value == 0) {
    if (capacity) { out[0]='0'; if(capacity>1) out[1]=0; else out[0]=0; }
    return;
  }
  char text[64];
  snprintf_(text,sizeof(text),"%.9g",value==0?0:value);
  unsigned end=0, decimal=64;
  while(text[end] && text[end]!='e' && text[end]!='E') {
    if(text[end]=='.') decimal=end;
    end++;
  }
  unsigned exponent=end;
  if(decimal<end) {
    while(end>decimal+1 && text[end-1]=='0') end--;
    if(end==decimal+1) end=decimal;
  }
  unsigned tail=exponent;
  while(text[tail]) text[end++]=text[tail++];
  text[end]=0;
  if(capacity) {
    size_t i=0;
    while(i+1<capacity && text[i]) { out[i]=text[i]; i++; }
    out[i]=0;
  }
}

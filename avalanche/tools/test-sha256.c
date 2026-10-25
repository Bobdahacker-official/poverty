#include "package.h"
extern int puts(const char *);
static int check(const char *label,const char *input, const char *expected) {
  uint8_t digest[32];
  sha256(input, strlen(input), digest);
  const char *digits = "0123456789abcdef";
  for (unsigned i = 0; i < 32; i++)
    if (digits[digest[i] >> 4] != expected[i * 2] ||
        digits[digest[i] & 15] != expected[i * 2 + 1])
      {puts("FAIL SHA-256 vector");return 1;}
  puts(label);
  return 0;
}
int main(void) {
  return check("PASS SHA-256 empty vector","", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b785"
                   "2b855") |
         check("PASS SHA-256 abc vector","abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61"
                      "f20015ad") |
         check("PASS SHA-256 multi-block padding vector","abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
               "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c"
               "1");
}

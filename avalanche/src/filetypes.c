#include "filetypes.h"
static int matches(const char *a, const char *b) {
  while (*a && *b) {
    char c=*a++;
    if(c>='A' && c<='Z') c+=32;
    if(c!=*b++) return 0;
  }
  return !*a && !*b;
}
const char *file_type_mime(enum file_type type) {
  static const char *types[]={"text/plain", "text/html", "image/png", "image/jpeg",
    "image/bmp", "audio/wav", "application/x-executable", "application/octet-stream"};
  return type<FILE_TYPE_COUNT?types[type]:types[FILE_BINARY];
}
enum file_type file_type_detect(const char *path, const unsigned char *data, size_t size) {
  if(size>=4 && data[0]==127 && data[1]=='E' && data[2]=='L' && data[3]=='F') return FILE_EXECUTABLE;
  if(size>=8 && data[0]==137 && data[1]=='P' && data[2]=='N' && data[3]=='G' && data[4]==13 && data[5]==10 && data[6]==26 && data[7]==10) return FILE_PNG;
  if(size>=3 && data[0]==255 && data[1]==216 && data[2]==255) return FILE_JPEG;
  if(size>=14 && data[0]=='B' && data[1]=='M') return FILE_BMP;
  if(size>=12 && data[0]=='R' && data[1]=='I' && data[2]=='F' && data[3]=='F' && data[8]=='W' && data[9]=='A' && data[10]=='V' && data[11]=='E') return FILE_WAV;
  const char *extension="";
  for(const char *p=path;*p;p++) {
    if(*p=='/') extension="";
    if(*p=='.') extension=p+1;
  }
  if(matches(extension,"html") || matches(extension,"htm")) return FILE_HTML;
  if(matches(extension,"png")) return FILE_PNG;
  if(matches(extension,"jpg") || matches(extension,"jpeg")) return FILE_JPEG;
  if(matches(extension,"bmp")) return FILE_BMP;
  if(matches(extension,"wav")) return FILE_WAV;
  for(size_t i=0;i<size;i++)
    if(data[i]==0 || (data[i]<32 && data[i]!=9 && data[i]!=10 && data[i]!=13)) return FILE_BINARY;
  return FILE_TEXT;
}

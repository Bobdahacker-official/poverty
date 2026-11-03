#include <stdio.h>
#include "../src/gui.c"
/* Compiler layout for debugger-driven automation of the native desktop. */
int main(void) {
  printf("{\"text_type\":%u,\"text_string\":%zu,\"text_length\":%zu,"
         "\"text_x\":%zu,\"text_y\":%zu,\"text_w\":%zu,\"text_h\":%zu,"
         "\"app_size\":%zu,\"app_bounds\":%zu,\"app_visible\":%zu,"
         "\"app_maximized\":%zu,\"app_count\":%u,\"theme_offset\":%zu,"
         "\"web_context_size\":%zu,\"web_node_size\":%zu,\"web_id\":%zu,\"web_value\":%zu,"
         "\"settings\":%u,\"network\":%u,\"editor\":%u,\"packages\":%u,\"terminal\":%u}\n",
         NK_COMMAND_TEXT,offsetof(struct nk_command_text,string),offsetof(struct nk_command_text,length),
         offsetof(struct nk_command_text,x),offsetof(struct nk_command_text,y),
         offsetof(struct nk_command_text,w),offsetof(struct nk_command_text,h),
         sizeof(struct app),offsetof(struct app,bounds),offsetof(struct app,visible),
         offsetof(struct app,maximized),APP_COUNT,__builtin_offsetof(__typeof__(preferences),theme),
         sizeof(struct web_context),sizeof(struct web_node),offsetof(struct web_node,id),offsetof(struct web_node,value),
         SETTINGS,NETWORK,EDITOR,PACKAGES,TERMINAL);
  return 0;
}

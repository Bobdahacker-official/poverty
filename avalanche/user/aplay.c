#include "lib.h"
int main(void) {
  char arguments[256];
  if (args_u(arguments, sizeof(arguments)) < 0 || !*arguments) {
    puts_u("Usage: aplay PATH.wav\n");
    return 2;
  }
  int result = av_audio_play(arguments);
  if (!result) {
    while ((result = av_audio_status(0)) > 0)
      av_call(AV_SLEEP, 1, 0, 0, 0);
  }
  if (result < 0) {
    puts_u("Audio playback failed: ");
    uint_u((unsigned)-result);
    puts_u("\n");
    return 1;
  }
  puts_u("Playback complete.\n");
  return 0;
}

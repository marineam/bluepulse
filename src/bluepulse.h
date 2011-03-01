#include <pulse/pulseaudio.h>

/* Used to identify duplicate processes */
#define APPLICATION_NAME "BluePulse"

void quit(int retval);

int pulse_init(pa_mainloop_api *api);
void pulse_quit();

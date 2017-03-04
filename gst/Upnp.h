#ifndef __UPNPP_H__
#define __UPNPP_H__

#include <glib-2.0/glib.h>

struct Renderer {
	char Name[50];
	char Udn[100];
};

struct Renderer* 	up_scan (int*);
void 				up_stop (char*);
void 				up_play (char*, char*);

void* 				start_upnp (void);
void  				stop_upnp (void*);

#endif

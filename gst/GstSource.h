/* 
 * Copyright (C) 2015 Vikram Fugro
 *
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.
 */

#ifndef _GST_SOURCE_H_
#define _GST_SOURCE_H_

typedef struct GstSource GstSource;

int getData (GstSource *p, char* fTo, int fMaxSize);
GstSource* startPipeline  (int port, char *device, char *type, char *url, int *ret);
void destroyPipeline (GstSource* p);

#endif

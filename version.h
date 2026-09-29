#ifndef VERSION_H
#define VERSION_H
/* The version comes from the Makefile (-DSBPRO_VERSION=x.y). */
#define SBPRO_STR_(x) #x
#define SBPRO_STR(x) SBPRO_STR_(x)
#ifdef SBPRO_VERSION_NUM
#define SBPRO_VERSION SBPRO_STR(SBPRO_VERSION_NUM)
#else
#define SBPRO_VERSION "dev"
#endif
#endif

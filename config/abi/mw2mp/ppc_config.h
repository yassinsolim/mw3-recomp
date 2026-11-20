#pragma once
#ifndef PPC_CONFIG_H_INCLUDED
#define PPC_CONFIG_H_INCLUDED


#define PPC_IMAGE_BASE 0x82000000ull
#define PPC_IMAGE_SIZE 0x1DE0000ull
#define PPC_CODE_BASE 0x820A0000ull
#define PPC_CODE_SIZE 0x39DCC4ull

#ifdef PPC_INCLUDE_DETAIL
#include "ppc_detail.h"
#endif

#endif

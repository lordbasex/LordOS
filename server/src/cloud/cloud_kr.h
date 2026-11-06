/* cloud_kr.h — Cloud's secrets in the session keyring (cloud_kr.c). */
#ifndef LORD_CLOUD_KR_H
#define LORD_CLOUD_KR_H

#include "cloud.h"

extern const CloudSecrets cloud_keyring;
void cloud_kr_watch(void);          /* retry the waiting accounts when the session keyring opens */

#endif

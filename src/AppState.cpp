#include "AppState.h"
RuntimeState gState;
SemaphoreHandle_t gSpiMutex = nullptr;
SemaphoreHandle_t gI2cMutex = nullptr;

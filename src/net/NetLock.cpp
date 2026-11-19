#include "NetLock.h"

namespace net {

    namespace {
        SemaphoreHandle_t turn = xSemaphoreCreateMutex();
    }

    HttpsTurn::HttpsTurn() { xSemaphoreTake(turn, portMAX_DELAY); }
    HttpsTurn::~HttpsTurn() { xSemaphoreGive(turn); }

}

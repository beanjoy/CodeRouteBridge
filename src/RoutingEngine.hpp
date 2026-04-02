#pragma once

#include <string>

bool TryExecuteRouting(std::wstring* errorMessage);
const std::wstring& GetLastRouteTrace();
bool WasRoutingAttempted();

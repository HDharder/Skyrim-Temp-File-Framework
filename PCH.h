#pragma once

#include "RE/Skyrim.h"
#include "SKSE/SKSE.h"

#include <MinHook.h>

#include <atomic>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

#include <spdlog/sinks/basic_file_sink.h>

using namespace std::literals;

namespace logger = SKSE::log;

// The JSON reader and writer, brought across the module boundary.
//
// Only #include and export using. JSON is not a convenience here: the NDK
// meta files that the API stubs are generated from are JSON, and so is the
// api-map.json that ndkstubgen is handed.

module;

#include <nlohmann/json.hpp>

export module mandk.json;

export using nlohmann::json;

#include "vr_settings.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>


namespace {
constexpr VRConfig kDefaults = {1.0f, true, false, 0.7f, 100.0f, 0, 0.0f, 0.0f, true};

std::string trim(std::string value) {
    const auto non_space = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), non_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), non_space).base(), value.end());
    return value;
}

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool parse_float(const std::string &text, float &out) {
    char *end = nullptr;
    errno = 0;
    const float value = std::strtof(text.c_str(), &end);
    while (end && std::isspace(static_cast<unsigned char>(*end))) ++end;
    if (errno || end == text.c_str() || !end || *end || !std::isfinite(value)) return false;
    out = value;
    return true;
}

bool parse_int(const std::string &text, int &out) {
    char *end = nullptr;
    errno = 0;
    const long value = std::strtol(text.c_str(), &end, 10);
    while (end && std::isspace(static_cast<unsigned char>(*end))) ++end;
    if (errno || end == text.c_str() || !end || *end) return false;
    out = static_cast<int>(value);
    return true;
}

bool parse_bool(const std::string &text, bool &out) {
    const std::string value = lowercase(trim(text));
    if (value == "1" || value == "true" || value == "yes" || value == "on") { out = true; return true; }
    if (value == "0" || value == "false" || value == "no" || value == "off") { out = false; return true; }
    return false;
}

void validate(VRConfig &config) {
    if (!std::isfinite(config.resolution_scale)) config.resolution_scale = kDefaults.resolution_scale;
    if (!std::isfinite(config.dynamic_resolution_min)) config.dynamic_resolution_min = kDefaults.dynamic_resolution_min;
    if (!std::isfinite(config.world_scale)) config.world_scale = kDefaults.world_scale;
    if (!std::isfinite(config.camera_height_offset)) config.camera_height_offset = kDefaults.camera_height_offset;
    if (!std::isfinite(config.camera_distance_offset)) config.camera_distance_offset = kDefaults.camera_distance_offset;
    config.resolution_scale = std::clamp(config.resolution_scale, 0.5f, 2.0f);
    config.dynamic_resolution_min = std::clamp(config.dynamic_resolution_min, 0.5f, config.resolution_scale);
    config.world_scale = std::clamp(config.world_scale, 50.0f, 200.0f);
    config.camera_height_offset = std::clamp(config.camera_height_offset, -50.0f, 50.0f);
    config.camera_distance_offset = std::clamp(config.camera_distance_offset, -100.0f, 100.0f);
    if (config.view_mode != 0 && config.view_mode != 1) config.view_mode = kDefaults.view_mode;
}

bool is_section(const std::string &line, const char *name) {
    const std::string value = trim(line);
    return value.size() >= 2 && value.front() == '[' && value.back() == ']' &&
           lowercase(trim(value.substr(1, value.size() - 2))) == lowercase(name);
}

bool any_section(const std::string &line) {
    const std::string value = trim(line);
    return value.size() >= 2 && value.front() == '[' && value.back() == ']';
}

std::vector<std::string> vr_lines() {
    std::ostringstream resolution, resolution_min, world, height, distance;
    resolution << std::fixed << std::setprecision(2) << g_vr_config.resolution_scale;
    resolution_min << std::fixed << std::setprecision(2) << g_vr_config.dynamic_resolution_min;
    world << std::fixed << std::setprecision(2) << g_vr_config.world_scale;
    height << std::fixed << std::setprecision(2) << g_vr_config.camera_height_offset;
    distance << std::fixed << std::setprecision(2) << g_vr_config.camera_distance_offset;
    return {"[VR]", "resolution_scale = " + resolution.str(),
            std::string("desktop_mirror = ") + (g_vr_config.desktop_mirror ? "true" : "false"),
            std::string("dynamic_resolution = ") + (g_vr_config.dynamic_resolution ? "true" : "false"),
            "dynamic_resolution_min = " + resolution_min.str(),
            "world_scale = " + world.str(),
            "view_mode = " + std::to_string(g_vr_config.view_mode),
            "camera_height_offset = " + height.str(), "camera_distance_offset = " + distance.str(),
            std::string("level_horizon = ") + (g_vr_config.level_horizon ? "true" : "false")};
}
} // namespace

VRConfig g_vr_config = kDefaults;

extern "C" void ResetVRConfig(void) { g_vr_config = kDefaults; }

extern "C" void LoadVRConfig(const char *filepath) {
    ResetVRConfig();
    if (!filepath || !*filepath) return;
    std::ifstream file(filepath);
    if (!file) return;

    bool in_vr = false;
    std::string line;
    while (std::getline(file, line)) {
        if (any_section(line)) { in_vr = is_section(line, "vr"); continue; }
        if (!in_vr) continue;
        const std::size_t equals = line.find('=');
        if (equals == std::string::npos) continue;
        const std::string key = lowercase(trim(line.substr(0, equals)));
        std::string value = trim(line.substr(equals + 1));
        const std::size_t comment = value.find_first_of(";#");
        if (comment != std::string::npos) value = trim(value.substr(0, comment));
        if (key == "resolution_scale") parse_float(value, g_vr_config.resolution_scale);
        else if (key == "desktop_mirror") parse_bool(value, g_vr_config.desktop_mirror);
        else if (key == "dynamic_resolution") parse_bool(value, g_vr_config.dynamic_resolution);
        else if (key == "dynamic_resolution_min") parse_float(value, g_vr_config.dynamic_resolution_min);
        else if (key == "world_scale") parse_float(value, g_vr_config.world_scale);
        else if (key == "view_mode") parse_int(value, g_vr_config.view_mode);
        else if (key == "camera_height_offset") parse_float(value, g_vr_config.camera_height_offset);
        else if (key == "camera_distance_offset") parse_float(value, g_vr_config.camera_distance_offset);
        else if (key == "level_horizon") parse_bool(value, g_vr_config.level_horizon);
    }
    validate(g_vr_config);
}

extern "C" void SaveVRConfig(const char *filepath) {
    if (!filepath || !*filepath) return;
    validate(g_vr_config);

    std::vector<std::string> input;
    std::ifstream source(filepath);
    std::string line;
    while (source && std::getline(source, line)) input.push_back(line);

    std::vector<std::string> output;
    bool replaced = false;
    for (std::size_t i = 0; i < input.size();) {
        if (!is_section(input[i], "vr")) { output.push_back(input[i++]); continue; }
        if (!replaced) {
            const auto section = vr_lines();
            output.insert(output.end(), section.begin(), section.end());
            replaced = true;
        }
        ++i;
        while (i < input.size() && !any_section(input[i])) ++i;
    }
    if (!replaced) {
        if (!output.empty() && !output.back().empty()) output.emplace_back();
        const auto section = vr_lines();
        output.insert(output.end(), section.begin(), section.end());
    }

    std::ofstream destination(filepath, std::ios::trunc);
    if (!destination) return;
    for (const auto &entry : output) destination << entry << '\n';
}


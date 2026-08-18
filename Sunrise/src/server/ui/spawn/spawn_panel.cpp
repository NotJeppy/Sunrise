#include "spawn_panel.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <imgui.h>
#include <span>
#include <string_view>
#include <vector>

#include "../../../client/content/entity_names/entity_name_cache.h"
#include "../../../client/content/items/packages/internal.h"
#include "../../../client/hooks/spawn/spawn_runtime.h"
#include "../../../client/content/placements/placement_extract.h"
#include "../../../client/player/player_position.h"
#include "../../../client/spawn/population_settings_store.h"
#include "../../../client/spawn/spawn_keybind_store.h"
#include "../../../state/activity/destination/activity_destination_snapshot.h"
#include "../../../state/activity/membership/activity_membership_query.h"
#include "../../../state/activity/definition.h"
#include "../../../state/build_data/runtime.h"
#include "../../../core/filesystem/path.h"
#include "../../../core/ui/components/picker/ui_picker_component.h"
#include "../../../middleware/content/packages/reader/reader.h"

namespace sunrise::server::ui::spawn {
namespace {

namespace native = client::hooks::spawn;
namespace spawn_keys = client::spawn;
namespace package_reader = middleware::content::packages::reader;
namespace picker = core::ui::components::picker;

constexpr std::uint32_t kEntityClass = 0x80809C0FU;
constexpr std::uint8_t kProjectileType = 18;
constexpr std::uint8_t kAmmoType = 20;
constexpr std::uint8_t kLootType = 21;
constexpr std::string_view kNameObject = "\"entities\"";
constexpr std::uint64_t kMaximumNameFile = 32ULL * 1024ULL * 1024ULL;

struct Candidate {
    std::uint32_t tag{};
    std::uint8_t type{};
    bool named{};
    std::array<char, 224> label{};
};

struct EntityName {
    std::uint32_t tag{};
    std::array<char, 144> text{};
    std::uint32_t order{};
};

struct Column {
    std::vector<Candidate> candidates{};
    std::vector<picker::Item> items{};
    std::size_t selected{};
    native::Settings settings{};
    int amount{1};
    int perRow{10};
    float spacing{1.0F};
};

Column g_main{};
Column g_projectile{};
Column g_loot{};
std::vector<EntityName> g_names{};
bool g_scanned{};
std::size_t g_capturingKey{spawn_keys::kActionCount};

void key_name(std::uint32_t virtualKey, std::array<char, 64>& output) noexcept {
    if (virtualKey == spawn_keys::kNoKey) {
        (void)std::snprintf(output.data(), output.size(), "None");
        return;
    }
    const UINT scanCode = MapVirtualKeyW(virtualKey, MAPVK_VK_TO_VSC);
    std::array<wchar_t, 64> wide{};
    const int written = scanCode != 0 ? GetKeyNameTextW(static_cast<LONG>(scanCode << 16),
                                                        wide.data(),
                                                        static_cast<int>(wide.size()))
                                      : 0;
    if (written <= 0
        || WideCharToMultiByte(CP_UTF8,
                               0,
                               wide.data(),
                               written,
                               output.data(),
                               static_cast<int>(output.size() - 1),
                               nullptr,
                               nullptr)
               <= 0) {
        (void)std::snprintf(
            output.data(), output.size(), "Key 0x%02X", static_cast<unsigned>(virtualKey));
    }
}

[[nodiscard]] bool capture_key(std::uint32_t& output) noexcept {
    if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0) {
        output = spawn_keys::kNoKey;
        return true;
    }
    for (int key = 7; key <= 254; ++key) {
        if ((GetAsyncKeyState(key) & 0x8000) != 0) {
            output = static_cast<std::uint32_t>(key);
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool key_picker(spawn_keys::Action action,
                              std::uint32_t& virtualKey,
                              float width) noexcept {
    const std::size_t index = static_cast<std::size_t>(action);
    ImGui::PushID(static_cast<int>(index));
    if (g_capturingKey == index) {
        if (ImGui::Button("...", ImVec2(width, 0.0F))) {
            g_capturingKey = spawn_keys::kActionCount;
        }
        ImGui::PopID();
        std::uint32_t picked = spawn_keys::kNoKey;
        if (capture_key(picked)) {
            virtualKey = picked;
            g_capturingKey = spawn_keys::kActionCount;
            return true;
        }
        return false;
    }
    std::array<char, 64> name{};
    key_name(virtualKey, name);
    const bool clicked = ImGui::Button(name.data(), ImVec2(width, 0.0F));
    ImGui::PopID();
    if (clicked) {
        g_capturingKey = index;
    }
    return false;
}

template <std::size_t Capacity>
[[nodiscard]] bool parse_string(std::string_view document,
                                std::size_t& cursor,
                                std::array<char, Capacity>& output) noexcept {
    output = {};
    if (cursor >= document.size() || document[cursor++] != '"') {
        return false;
    }
    std::size_t written = 0;
    while (cursor < document.size()) {
        char value = document[cursor++];
        if (value == '"') {
            return true;
        }
        if (value == '\\') {
            if (cursor >= document.size()) {
                return false;
            }
            value = document[cursor++];
            if (value == 'u') {
                if (cursor + 4 > document.size()) {
                    return false;
                }
                cursor += 4;
                value = '?';
            } else if (value == 'n') {
                value = '\n';
            } else if (value == 'r') {
                value = '\r';
            } else if (value == 't') {
                value = '\t';
            }
        }
        if (written + 1 < output.size()) {
            output[written++] = value;
        }
    }
    return false;
}

void skip_space(std::string_view document, std::size_t& cursor) noexcept {
    while (cursor < document.size()) {
        const char value = document[cursor];
        if (value != ' ' && value != '\t' && value != '\r' && value != '\n') {
            return;
        }
        ++cursor;
    }
}

[[nodiscard]] bool load_names() noexcept {
    g_names.clear();
    HMODULE module = nullptr;
    constexpr DWORD flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                            | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
    if (GetModuleHandleExW(flags, reinterpret_cast<LPCWSTR>(&load_names), &module) == FALSE) {
        return false;
    }

    std::vector<char> bytes{};
    core::path::Buffer path{};
    // A hand-supplied file is preferred: the managed cache is rebuilt by its own builder whenever
    // its generator marker does not match, which would overwrite a richer one dropped in place.
    core::path::Buffer preferred{};
    const bool hasPreferred = core::path::artifact_directory(module, preferred)
                              && core::path::append(preferred, L"\\EntityNamesExtra.json")
                              && GetFileAttributesW(preferred.chars.data())
                                     != INVALID_FILE_ATTRIBUTES;
    if (hasPreferred) {
        path = preferred;
    }
    if ((hasPreferred || core::path::artifact_directory(module, path))
        && (hasPreferred || core::path::append(path, L"\\EntityNames.json"))) {
        const HANDLE file = CreateFileW(path.chars.data(),
                                        GENERIC_READ,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                        nullptr,
                                        OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                                        nullptr);
        LARGE_INTEGER length{};
        if (file != INVALID_HANDLE_VALUE && GetFileSizeEx(file, &length) != FALSE
            && length.QuadPart > 0
            && static_cast<std::uint64_t>(length.QuadPart) <= kMaximumNameFile) {
            bytes.resize(static_cast<std::size_t>(length.QuadPart));
            DWORD read = 0;
            if (ReadFile(file,
                         bytes.data(),
                         static_cast<DWORD>(bytes.size()),
                         &read,
                         nullptr)
                    == FALSE
                || read != bytes.size()) {
                bytes.clear();
            }
        }
        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
        }
    }
    if (bytes.empty()) {
        return false;
    }

    const std::string_view document(bytes.data(), bytes.size());
    std::size_t cursor = document.find(kNameObject);
    cursor = cursor == std::string_view::npos ? cursor : document.find('{', cursor);
    if (cursor == std::string_view::npos) {
        return false;
    }
    ++cursor;
    while (cursor < document.size()) {
        skip_space(document, cursor);
        if (cursor < document.size() && document[cursor] == ',') {
            ++cursor;
            skip_space(document, cursor);
        }
        if (cursor >= document.size() || document[cursor] == '}') {
            break;
        }
        std::array<char, 16> tagText{};
        if (!parse_string(document, cursor, tagText)) {
            return false;
        }
        std::uint32_t tag = 0;
        const char* const end = tagText.data() + std::strlen(tagText.data());
        const auto parsed = std::from_chars(tagText.data(), end, tag, 16);
        skip_space(document, cursor);
        if (parsed.ec != std::errc{} || parsed.ptr != end || cursor >= document.size()
            || document[cursor++] != ':') {
            return false;
        }
        skip_space(document, cursor);
        if (cursor >= document.size() || document[cursor++] != '[') {
            return false;
        }
        std::uint32_t order = 0;
        for (;;) {
            skip_space(document, cursor);
            if (cursor < document.size() && document[cursor] == ',') {
                ++cursor;
                skip_space(document, cursor);
            }
            if (cursor >= document.size()) {
                return false;
            }
            if (document[cursor] == ']') {
                ++cursor;
                break;
            }
            EntityName name{};
            name.tag = tag;
            name.order = order++;
            if (!parse_string(document, cursor, name.text)) {
                return false;
            }
            if (name.text[0] != '\0') {
                g_names.push_back(name);
            }
        }
    }
    std::sort(g_names.begin(), g_names.end(), [](const EntityName& first, const EntityName& second) {
        return first.tag != second.tag ? first.tag < second.tag : first.order < second.order;
    });
    return !g_names.empty();
}

[[nodiscard]] const char* name_of(std::uint32_t tag) noexcept {
    const auto found = std::lower_bound(
        g_names.begin(), g_names.end(), tag, [](const EntityName& value, std::uint32_t wanted) {
            return value.tag < wanted;
        });
    return found != g_names.end() && found->tag == tag ? found->text.data() : nullptr;
}

[[nodiscard]] bool projectile_name(std::string_view name) noexcept {
    constexpr std::array<std::string_view, 12> markers{
        "projectile", "missile", "rocket", "grenade", "fireball", "mortar",
        "cannonball", "seeker", "tracer", "bullet", "plasma_bolt", "weapon_bolt",
    };
    return std::any_of(markers.begin(), markers.end(), [name](std::string_view marker) {
        return name.find(marker) != std::string_view::npos;
    });
}

[[nodiscard]] bool skipped_family(std::wstring_view family) noexcept {
    return family.starts_with(L"w64_audio_") || family.starts_with(L"w64_ui_");
}

void family_text(std::wstring_view family, std::array<char, 96>& output) noexcept {
    output = {};
    const std::size_t count = (std::min)(family.size(), output.size() - 1);
    for (std::size_t index = 0; index < count; ++index) {
        const wchar_t value = family[index];
        output[index] = value >= 32 && value <= 126 ? static_cast<char>(value) : '?';
    }
}

void add_candidate(Column& column,
                   std::uint32_t tag,
                   std::uint8_t type,
                   std::wstring_view family) {
    std::array<char, 96> package{};
    family_text(family, package);
    Candidate value{};
    value.tag = tag;
    value.type = type;
    const char* const name = name_of(tag);
    value.named = name != nullptr;
    if (name != nullptr) {
        (void)std::snprintf(value.label.data(),
                            value.label.size(),
                            "%s | 0x%08X | type %u | %s",
                            name,
                            tag,
                            static_cast<unsigned>(type),
                            package.data());
    } else {
        (void)std::snprintf(value.label.data(),
                            value.label.size(),
                            "0x%08X | type %u | %s",
                            tag,
                            static_cast<unsigned>(type),
                            package.data());
    }
    column.candidates.push_back(value);
}

bool collect_entity(void*, const package_reader::ClassEntry& entry) noexcept {
    if (skipped_family(entry.packageFamily) || !native::is_tag_resident(entry.tag)) {
        return true;
    }
    std::uint8_t type = 0;
    if (!native::object_type(entry.tag, type)) {
        return true;
    }
    const char* const name = name_of(entry.tag);
    if (type == kProjectileType || (name != nullptr && projectile_name(name))) {
        add_candidate(g_projectile, entry.tag, type, entry.packageFamily);
    } else if (type == kAmmoType || type == kLootType) {
        add_candidate(g_loot, entry.tag, type, entry.packageFamily);
    } else {
        add_candidate(g_main, entry.tag, type, entry.packageFamily);
    }
    return true;
}

void finish_column(Column& column) {
    std::sort(column.candidates.begin(),
              column.candidates.end(),
              [](const Candidate& first, const Candidate& second) {
                  if (first.named != second.named) {
                      return first.named;
                  }
                  return std::string_view(first.label.data())
                         < std::string_view(second.label.data());
              });
    column.candidates.erase(
        std::unique(column.candidates.begin(),
                    column.candidates.end(),
                    [](const Candidate& first, const Candidate& second) {
                        return first.tag == second.tag;
                    }),
        column.candidates.end());
    column.items.clear();
    column.items.reserve(column.candidates.size());
    for (const Candidate& candidate : column.candidates) {
        column.items.push_back({candidate.label.data()});
    }
    column.selected = 0;
}

void refresh() noexcept {
    g_main.candidates.clear();
    g_projectile.candidates.clear();
    g_loot.candidates.clear();
    core::path::Buffer directory{};
    const bool hasDirectory = client::content::items::packages::package_directory(directory);
    if (hasDirectory) {
        (void)client::content::entity_names::ensure(directory.chars.data());
    }
    (void)load_names();
    if (native::ready() && hasDirectory) {
        package_reader::ScanResult result{};
        (void)package_reader::scan_class_entries(
            directory.chars.data(), kEntityClass, &collect_entity, nullptr, result);
        package_reader::release_caches();
    }
    finish_column(g_main);
    finish_column(g_projectile);
    finish_column(g_loot);
    g_scanned = true;
}

[[nodiscard]] const char* preview(const Column& column) noexcept {
    return column.selected < column.candidates.size()
               ? column.candidates[column.selected].label.data()
               : "[None]";
}

void draw_settings(Column& column, const char* id, bool showSpawnAll) noexcept {
    ImGui::PushID(id);
    ImGui::TextUnformatted("Amount:");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::DragInt("##amount", &column.amount, 1.0F, 1, 4096, "%d");
    ImGui::TextUnformatted("Vertical lift:");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::DragFloat(
        "##vertical_lift", &column.settings.lift, 0.1F, -100.0F, 100.0F, "%.1f");
    ImGui::TextUnformatted("Ray distance:");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::DragFloat("##ray_distance",
                     &column.settings.rayDistance,
                     1.0F,
                     1.0F,
                     2000.0F,
                     "%.0f");
    ImGui::TextUnformatted("Scale:");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::DragFloat("##scale", &column.settings.scale, 0.01F, 0.01F, 100.0F, "%.2f");
    ImGui::Checkbox("Camera rotation", &column.settings.useCameraRotation);

    if (ImGui::TreeNodeEx("Transform", ImGuiTreeNodeFlags_SpanAvailWidth)) {
        ImGui::Checkbox("Override rotation", &column.settings.overrideRotation);
        ImGui::TextUnformatted("Position offset:");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputFloat3("##position_offset", column.settings.offset.data(), "%.2f");
        if (column.settings.overrideRotation) {
            ImGui::TextUnformatted("Rotation quaternion:");
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputFloat4("##rotation_quaternion", column.settings.rotation.data(), "%.3f");
        }
        ImGui::TreePop();
    }

    if (showSpawnAll
        && ImGui::TreeNodeEx("Spawn All [unstable]", ImGuiTreeNodeFlags_SpanAvailWidth)) {
        ImGui::TextUnformatted("Items per row:");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::DragInt("##items_per_row", &column.perRow, 1.0F, 1, 4096, "%d");
        ImGui::TextUnformatted("Spacing:");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::DragFloat("##spacing", &column.spacing, 0.1F, 0.1F, 100.0F, "%.1f");
        if (ImGui::Button("Spawn all at crosshair", ImVec2(-FLT_MIN, 0.0F))) {
            std::vector<std::uint32_t> tags{};
            tags.reserve(column.candidates.size());
            for (const Candidate& candidate : column.candidates) {
                tags.push_back(candidate.tag);
            }
            (void)native::request_line(tags,
                                       native::Origin::crosshair,
                                       static_cast<std::uint32_t>((std::max)(column.perRow, 1)),
                                       column.spacing,
                                       column.settings);
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void draw_keybinds(spawn_keys::Action playerAction,
                   spawn_keys::Action crosshairAction,
                   spawn_keys::Keybinds& keybinds,
                   bool& changed) noexcept {
    if (!ImGui::TreeNodeEx("Keybinds", ImGuiTreeNodeFlags_SpanAvailWidth)) {
        return;
    }
    const float labelWidth = ImGui::CalcTextSize("At crosshair").x
                             + ImGui::GetStyle().ItemSpacing.x * 2.0F;
    const float controlWidth = ImGui::GetContentRegionAvail().x - labelWidth;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("At player");
    ImGui::SameLine(labelWidth);
    changed = key_picker(playerAction,
                         keybinds.virtualKeys[static_cast<std::size_t>(playerAction)],
                         controlWidth)
              || changed;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("At crosshair");
    ImGui::SameLine(labelWidth);
    changed = key_picker(crosshairAction,
                         keybinds.virtualKeys[static_cast<std::size_t>(crosshairAction)],
                         controlWidth)
              || changed;
    ImGui::TreePop();
}

void draw_column(const char* title,
                 const char* id,
                 Column& column,
                 spawn_keys::Action playerAction,
                 spawn_keys::Action crosshairAction,
                 bool showSpawnAll,
                 spawn_keys::Keybinds& keybinds,
                 bool& keybindsChanged) noexcept {
    ImGui::PushID(id);
    ImGui::TextUnformatted(title);
    ImGui::Separator();
    const std::span<const picker::Item> rows(column.items.data(), column.items.size());
    (void)picker::control("picker", preview(column), rows, column.selected);

    ImGui::BeginDisabled(column.selected >= column.candidates.size() || native::busy());
    if (ImGui::Button("At player", ImVec2(ImGui::GetContentRegionAvail().x * 0.49F, 0.0F))) {
        (void)native::request(column.candidates[column.selected].tag,
                              native::Origin::player,
                              static_cast<std::uint32_t>((std::max)(column.amount, 1)),
                              column.settings);
    }
    ImGui::SameLine();
    if (ImGui::Button("At crosshair", ImVec2(-FLT_MIN, 0.0F))) {
        (void)native::request(column.candidates[column.selected].tag,
                              native::Origin::crosshair,
                              static_cast<std::uint32_t>((std::max)(column.amount, 1)),
                              column.settings);
    }
    ImGui::EndDisabled();
    const std::uint32_t selectedTag = column.selected < column.candidates.size()
                                          ? column.candidates[column.selected].tag
                                          : 0xFFFFFFFFU;
    const std::uint32_t amount = static_cast<std::uint32_t>((std::max)(column.amount, 1));
    native::configure_shortcut(playerAction, selectedTag, amount, column.settings);
    native::configure_shortcut(crosshairAction, selectedTag, amount, column.settings);
    draw_keybinds(playerAction, crosshairAction, keybinds, keybindsChanged);
    draw_settings(column, "settings", showSpawnAll);
    ImGui::PopID();
}

} // namespace


/**
 * Object type every combatant carries. The picker shows it beside each name, and ships, props
 * and simulation shells use other types, so this is the filter that keeps a population hostile.
 */
constexpr std::uint8_t kCombatantType = 12;

/** Unit names of one faction. Entity names are display names, so matching ignores case. */
struct Faction {
    const char* label{};
    std::array<std::string_view, 10> markers{};
};

/**
 * The factions the populator can draw from, each named by its own units.
 * A destination hosts only some of these, so the panel offers them separately rather than
 * populating every world with everything the packages happen to hold.
 */
constexpr std::array<Faction, 6> kFactions{
    Faction{"Fallen", {"dreg", "vandal", "marauder", "shank", "servitor", "captain", "walker"}},
    Faction{"Hive", {"thrall", "acolyte", "knight", "wizard", "ogre", "shrieker", "cursed"}},
    Faction{"Cabal",
            {"psion", "legionary", "phalanx", "centurion", "colossus", "incendior", "gladiator",
             "war beast", "scorpius"}},
    Faction{"Vex",
            {"goblin", "hobgoblin", "minotaur", "harpy", "cyclops", "hydra", "wyvern", "fanatic",
             "axis"}},
    Faction{"Taken", {"taken"}},
    Faction{"Scorn",
            {"screeb", "chieftain", "raider", "ravager", "abomination", "stalker", "lurker",
             "wretch"}},
};

/**
 * Names that mark a champion. The unit name stays in the display name, so a champion matches its
 * own faction list and has to be excluded by its prefix instead.
 */
constexpr std::array<std::string_view, 4> kChampionMarkers{
    "overload", "unstoppable", "barrier", "champion",
};

/** Names that mark a boss or a named major rather than an ordinary combatant. */
constexpr std::array<std::string_view, 4> kBossMarkers{
    "prime", ", the", " the ", "proxy",
};

/** Names that mark a ship or a vehicle. A Cabal Harvester is a dropship, not a combatant. */
constexpr std::array<std::string_view, 10> kVehicleMarkers{
    "ship", "skiff", "harvester", "pike", "interceptor",
    "sparrow", "drake", "shuttle", "transport", "carrier",
};

/** Population source: the faction filter, or the one entity picked in the main column. */
enum class PopulationSource : int {
    factions = 0,
    selected = 1,
    map = 2,
};

client::content::placements::ExtractResult g_extracted{};
bool g_extractedValid{};
/**
 * Off by default: the authored records name props and encounter definitions rather than
 * combatants, and only their positions are used, so filtering on the recorded entity's type just
 * discards usable ground.
 */
bool g_extractCombatantsOnly{};
/** On by default: free roam only opens the public bubbles of a shared map. */
bool g_extractPublicOnly{true};
native::PopulationSettings g_population{};
PopulationSource g_populationSource{PopulationSource::factions};
/** One flag per faction, in kFactions order. Every faction is offered until one is cleared. */
std::array<bool, kFactions.size()> g_factionEnabled{true, true, true, true, true, true};
bool g_excludeVehicles{true};
bool g_excludeChampions{true};
bool g_excludeBosses{true};
bool g_populationPrimed{};

/** @return True when the haystack holds the needle, comparing without case. */
[[nodiscard]] bool contains_insensitive(std::string_view haystack,
                                        std::string_view needle) noexcept {
    if (needle.empty() || needle.size() > haystack.size()) {
        return false;
    }
    const std::size_t last = haystack.size() - needle.size();
    for (std::size_t start = 0; start <= last; ++start) {
        std::size_t index = 0;
        while (index < needle.size()) {
            const char value = haystack[start + index];
            const char lowered =
                value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
            if (lowered != needle[index]) {
                break;
            }
            ++index;
        }
        if (index == needle.size()) {
            return true;
        }
    }
    return false;
}

/** @return True when any marker of an enabled faction names this entity. */
[[nodiscard]] bool wanted_faction(std::string_view name) noexcept {
    for (std::size_t index = 0; index < kFactions.size(); ++index) {
        if (!g_factionEnabled[index]) {
            continue;
        }
        for (const std::string_view marker : kFactions[index].markers) {
            if (!marker.empty() && contains_insensitive(name, marker)) {
                return true;
            }
        }
    }
    return false;
}

/** @return True when the name marks a champion. */
[[nodiscard]] bool champion_name(std::string_view name) noexcept {
    return std::any_of(kChampionMarkers.begin(),
                       kChampionMarkers.end(),
                       [name](std::string_view marker) {
                           return contains_insensitive(name, marker);
                       });
}

/** @return True when the name marks a boss or a named major. */
[[nodiscard]] bool boss_name(std::string_view name) noexcept {
    return std::any_of(kBossMarkers.begin(), kBossMarkers.end(), [name](std::string_view marker) {
        return contains_insensitive(name, marker);
    });
}

/** @return True when the name marks a ship or a vehicle rather than a combatant. */
[[nodiscard]] bool vehicle_name(std::string_view name) noexcept {
    return std::any_of(kVehicleMarkers.begin(),
                       kVehicleMarkers.end(),
                       [name](std::string_view marker) {
                           return contains_insensitive(name, marker);
                       });
}

/** One entity of the installed packages, kept whether or not it is streamed in. */
struct RosterEntity {
    std::uint32_t tag{};
    std::array<char, 96> name{};
    std::array<char, 64> family{};
};

std::vector<RosterEntity> g_roster{};
bool g_rosterScanned{};

/** Collects one entity of any package, because a batch fills destinations it is not standing in. */
bool collect_roster(void*, const package_reader::ClassEntry& entry) noexcept {
    if (skipped_family(entry.packageFamily)) {
        return true;
    }
    const char* const name = name_of(entry.tag);
    if (name == nullptr) {
        return true;
    }
    RosterEntity value{};
    value.tag = entry.tag;
    (void)std::snprintf(value.name.data(), value.name.size(), "%s", name);
    std::array<char, 96> family{};
    family_text(entry.packageFamily, family);
    (void)std::snprintf(value.family.data(), value.family.size(), "%s", family.data());
    g_roster.push_back(value);
    return true;
}

/** Builds the package-wide roster once. */
void ensure_roster() noexcept {
    if (g_rosterScanned) {
        return;
    }
    g_roster.clear();
    core::path::Buffer directory{};
    if (client::content::items::packages::package_directory(directory)) {
        (void)client::content::entity_names::ensure(directory.chars.data());
        (void)load_names();
        package_reader::ScanResult result{};
        (void)package_reader::scan_class_entries(
            directory.chars.data(), kEntityClass, &collect_roster, nullptr, result);
        package_reader::release_caches();
    }
    g_rosterScanned = true;
}

/**
 * @return The leading word of a name, which is the part its packages are named after.
 * `mercury_destination` yields `mercury`, and the entities that map streams sit in families like
 * `w64_mercury_...`, so the word is what ties an entity to the place it belongs.
 */
[[nodiscard]] std::string_view leading_word(std::string_view name) noexcept {
    const std::size_t split = name.find('_');
    const std::string_view word =
        split == std::string_view::npos ? name : name.substr(0, split);
    return word.size() >= 3 ? word : name;
}

/**
 * @return The key an activity's entities are found under.
 * The activity name is the wrong key: `adventure_brainwash` names no place, and every adventure,
 * mission and patrol is named for itself rather than the world it sits on. The map stem is the
 * place, so it is the key, and the activity name is only a fallback for a row that carries none.
 */
[[nodiscard]] std::string_view
destination_token(std::string_view destination,
                  const state::build_data::scenarios::Definition& layout) noexcept {
    const std::string_view stem(layout.spawnStem.data(), layout.spawnStemLength);
    return stem.empty() ? leading_word(destination) : leading_word(stem);
}

/**
 * @return True when this name is a combatant of any faction, after the exclusions.
 * The faction checkboxes are left out: they choose which world's roster the roaming mode draws
 * from, which is not a question a caller that already knows its world needs to ask.
 */
[[nodiscard]] bool is_combatant_name(std::string_view name) noexcept {
    bool named = false;
    for (const Faction& faction : kFactions) {
        for (const std::string_view marker : faction.markers) {
            named = named || (!marker.empty() && contains_insensitive(name, marker));
        }
    }
    return named && !(g_excludeVehicles && vehicle_name(name))
           && !(g_excludeChampions && champion_name(name))
           && !(g_excludeBosses && boss_name(name));
}

/** @return True when this name passes every roster filter that is switched on. */
[[nodiscard]] bool accepted_combatant(std::string_view name) noexcept {
    return wanted_faction(name) && !(g_excludeVehicles && vehicle_name(name))
           && !(g_excludeChampions && champion_name(name))
           && !(g_excludeBosses && boss_name(name));
}

/**
 * Hands the populator the tags its current source names.
 * The main column is the only source, because projectiles and loot are not combatants.
 */
void publish_population_source() noexcept {
    std::vector<std::uint32_t> tags{};
    if (g_populationSource == PopulationSource::selected) {
        if (g_main.selected < g_main.candidates.size()) {
            tags.push_back(g_main.candidates[g_main.selected].tag);
        }
        native::set_population_tags(tags);
        return;
    }
    for (const Candidate& candidate : g_main.candidates) {
        const char* const name = name_of(candidate.tag);
        if (candidate.type != kCombatantType || name == nullptr) {
            continue;
        }
        if (!accepted_combatant(std::string_view(name))) {
            continue;
        }
        tags.push_back(candidate.tag);
    }
    native::set_population_tags(tags);
}

/**
 * Names the destination the player is in, which is the key a recorded map is filed under.
 * @param output Receives the name.
 * @return True when the player is in a world with a named destination.
 */
[[nodiscard]] bool current_destination(std::string_view& output) noexcept {
    const std::uint64_t sessionId =
        state::activity::membership::live_region_session(state::activity::kAbsentSessionId);
    if (sessionId == state::activity::kAbsentSessionId) {
        return false;
    }
    state::activity::destination::DestinationSelection selection{};
    if (!state::activity::destination::snapshot(sessionId, selection)) {
        return false;
    }
    output = std::string_view(reinterpret_cast<const char*>(selection.packageName.data()),
                             selection.packageNameLength);
    return !output.empty();
}

/** Hands the runtime the points the held map carries. */
void publish_map_points() noexcept {
    std::vector<client::spawn::MapPoint> stored(client::spawn::map_size());
    const std::size_t count = client::spawn::copy_map(stored);
    std::vector<native::PopulationPoint> points{};
    points.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        native::PopulationPoint point{};
        point.tag = stored[index].tag;
        point.position = stored[index].position;
        points.push_back(point);
    }
    native::set_population_points(points);
}

/** One world's map stem and the factions that inhabit it, by their index in kFactions. */
struct WorldFactions {
    std::string_view stem;
    /** Faction indices, terminated by the capacity. Fallen 0, Hive 1, Cabal 2, Vex 3, Taken 4,
     * Scorn 5. */
    std::array<std::size_t, 4> factions;
};

/**
 * Which factions belong on which world.
 * Entity definitions sit in shared packages rather than in each destination's own, so a package
 * name cannot say where an entity belongs. The world does: this is the same faction split the
 * game itself uses per destination.
 */
constexpr std::size_t kNoFaction = 99;
constexpr std::array<WorldFactions, 12> kWorldFactions{
    WorldFactions{"edz", {0, 2, kNoFaction, kNoFaction}},
    WorldFactions{"eden", {3, 0, 2, kNoFaction}},
    WorldFactions{"planet", {1, 0, kNoFaction, kNoFaction}},
    WorldFactions{"polaris", {3, 4, kNoFaction, kNoFaction}},
    WorldFactions{"mercury", {3, kNoFaction, kNoFaction, kNoFaction}},
    WorldFactions{"luna", {1, 0, 3, kNoFaction}},
    WorldFactions{"moon", {1, 0, 3, kNoFaction}},
    WorldFactions{"tangled", {0, 5, kNoFaction, kNoFaction}},
    WorldFactions{"dreaming", {4, 5, 1, kNoFaction}},
    WorldFactions{"dreamy", {4, 5, 1, kNoFaction}},
    WorldFactions{"fleet", {2, kNoFaction, kNoFaction, kNoFaction}},
    WorldFactions{"nessus", {3, 0, 2, kNoFaction}},
};

/**
 * @return True when this name belongs to a faction the world hosts.
 * A world with no entry accepts every faction, which keeps an unknown map populated rather than
 * empty.
 */
[[nodiscard]] bool world_faction(std::string_view name, std::string_view stem) noexcept {
    const WorldFactions* found = nullptr;
    for (const WorldFactions& world : kWorldFactions) {
        if (contains_insensitive(stem, world.stem)) {
            found = &world;
            break;
        }
    }
    if (found == nullptr) {
        return true;
    }
    for (const std::size_t faction : found->factions) {
        if (faction == kNoFaction || faction >= kFactions.size()) {
            continue;
        }
        for (const std::string_view marker : kFactions[faction].markers) {
            if (!marker.empty() && contains_insensitive(name, marker)) {
                return true;
            }
        }
    }
    return false;
}

/** What one batch run produced. */
struct BatchResult {
    std::size_t destinations{};
    std::size_t written{};
    std::size_t points{};
    std::size_t skippedNoPlacements{};
    std::size_t skippedNoRoster{};
};

BatchResult g_batch{};
bool g_batchValid{};
/**
 * Batch state.
 * The walk is far too slow to run to completion inside one frame: doing so stalls the render loop
 * long enough for the game to drop its own session, which surfaces as a network error and a black
 * screen. One destination per frame keeps the game responsive throughout.
 */
bool g_batchActive{};
std::size_t g_batchCursor{};
std::vector<state::build_data::scenarios::Definition> g_batchLayouts{};
/** Last few destinations that produced nothing, which is what a failed run needs to explain. */
std::array<std::array<char, 72>, 6> g_batchNotes{};
std::size_t g_batchNoteCount{};

/** Records one short note about a destination the batch could not fill. */
void note_batch(std::string_view destination,
                std::string_view reason,
                std::size_t roster) noexcept {
    if (g_batchNoteCount == g_batchNotes.size()) {
        return;
    }
    (void)std::snprintf(g_batchNotes[g_batchNoteCount].data(),
                        g_batchNotes[g_batchNoteCount].size(),
                        "%.*s: no match for key '%.*s' (roster %zu)",
                        static_cast<int>(destination.size()),
                        destination.data(),
                        static_cast<int>(reason.size()),
                        reason.data(),
                        roster);
    ++g_batchNoteCount;
}

/** Begins a batch over every installed destination. */
void start_batch() noexcept {
    ensure_roster();
    g_batch = {};
    g_batchValid = true;
    g_batchNoteCount = 0;
    g_batchCursor = 0;
    g_batchLayouts.clear();
    const std::size_t count = state::build_data::scenario_layout_count();
    if (count == 0) {
        return;
    }
    g_batchLayouts.resize(count);
    std::size_t written = 0;
    if (!state::build_data::snapshot_scenario_layouts(g_batchLayouts, written)) {
        g_batchLayouts.clear();
        return;
    }
    g_batchLayouts.resize(written);
    g_batchActive = !g_batchLayouts.empty();
}

/** Advances the batch by one destination. */
void step_batch() noexcept {
    if (!g_batchActive) {
        return;
    }
    if (g_batchCursor >= g_batchLayouts.size()) {
        g_batchActive = false;
        // The batch left its last destination in the held map, which is not the one being played.
        client::spawn::clear_map();
        native::set_population_points({});
        return;
    }
    const state::build_data::scenarios::Definition& layout = g_batchLayouts[g_batchCursor++];
    const std::string_view destination(layout.name.data(), layout.nameLength);
    if (destination.empty()) {
        return;
    }
    ++g_batch.destinations;
    client::content::placements::ExtractResult extracted{};
    if (!client::content::placements::extract(destination, false, true, extracted)
        || extracted.kept == 0) {
        ++g_batch.skippedNoPlacements;
        return;
    }
    // The roster is narrowed to entities whose package names this destination, then to the ones
    // the filters accept, so a world gets its own combatants and nothing else.
    const std::string_view token = destination_token(destination, layout);
    std::vector<std::uint32_t> combatants{};
    for (const RosterEntity& entity : g_roster) {
        const std::string_view name(entity.name.data());
        // The faction filter is for the roaming mode, where the player picks a world by hand. A
        // batch knows which world each destination is, so the world decides instead.
        if (!world_faction(name, token)) {
            continue;
        }
        if (!is_combatant_name(name)) {
            continue;
        }
        combatants.push_back(entity.tag);
    }
    if (combatants.empty()) {
        ++g_batch.skippedNoRoster;
        note_batch(destination, token, g_roster.size());
        return;
    }
    std::vector<client::spawn::MapPoint> held(client::spawn::map_size());
    const std::size_t points = client::spawn::copy_map(held);
    client::spawn::clear_map();
    for (std::size_t index = 0; index < points; ++index) {
        client::spawn::MapPoint point = held[index];
        point.tag = combatants[(index * 7 + index / combatants.size()) % combatants.size()];
        if (!client::spawn::add_map_point(point)) {
            break;
        }
    }
    const std::size_t saved = client::spawn::map_size();
    if (client::spawn::save_map(destination)) {
        ++g_batch.written;
        g_batch.points += saved;
    } else {
        note_batch(destination, std::string_view("save failed"), combatants.size());
    }
}

/**
 * Replaces every held point's entity with one this world hosts.
 * @param destination Destination the points belong to.
 * @return True when the map was refilled.
 */
[[nodiscard]] bool fill_with_world_combatants(std::string_view destination) noexcept {
    // The world decides its own factions. Asking the player to tick them per destination is the
    // same lookup done by hand, and it is wrong the moment they forget.
    state::build_data::scenarios::Definition layout{};
    const bool known = state::build_data::find_scenario_layout(destination, layout);
    const std::string_view token =
        known ? destination_token(destination, layout) : std::string_view{};
    std::vector<std::uint32_t> combatants{};
    for (const Candidate& candidate : g_main.candidates) {
        const char* const name = name_of(candidate.tag);
        if (candidate.type != kCombatantType || name == nullptr) {
            continue;
        }
        const std::string_view text{name};
        if (is_combatant_name(text) && world_faction(text, token)) {
            combatants.push_back(candidate.tag);
        }
    }
    std::vector<client::spawn::MapPoint> held(client::spawn::map_size());
    const std::size_t count = client::spawn::copy_map(held);
    if (combatants.empty() || count == 0) {
        return false;
    }
    client::spawn::clear_map();
    for (std::size_t index = 0; index < count; ++index) {
        client::spawn::MapPoint point = held[index];
        // Spread the roster over the points without a generator: the index alone gives a stable,
        // even mix and keeps one destination reproducible run to run.
        point.tag = combatants[(index * 7 + index / combatants.size()) % combatants.size()];
        if (!client::spawn::add_map_point(point)) {
            break;
        }
    }
    publish_map_points();
    return true;
}

/**
 * Runs the whole preparation for one destination: read its placements, fill them with the
 * combatants this world hosts, save the map, and start placing.
 * @param destination Destination the player is in.
 */
void populate_this_destination(std::string_view destination) noexcept {
    ensure_roster();
    if (!g_scanned) {
        refresh();
    }
    client::content::placements::ExtractResult extracted{};
    // Combatants-only is off on purpose: the records name props and encounter definitions, and
    // only their positions are used.
    if (!client::content::placements::extract(destination, false, g_extractPublicOnly, extracted)) {
        return;
    }
    g_extracted = extracted;
    g_extractedValid = true;
    if (extracted.kept == 0 || !fill_with_world_combatants(destination)) {
        return;
    }
    (void)client::spawn::save_map(destination);
    g_population.useMap = true;
    g_population.enabled = true;
    g_populationSource = PopulationSource::map;
    (void)client::spawn::publish_population(g_population);
}

/** Draws the recorder, which is how a map is authored in the first place. */
void draw_recorder() noexcept {
    std::string_view destination{};
    const bool located = current_destination(destination);
    ImGui::TextDisabled("Destination: %.*s  |  %zu recorded  |  %zu published",
                        static_cast<int>(located ? destination.size() : 7),
                        located ? destination.data() : "unknown",
                        client::spawn::map_size(),
                        native::population_point_count());

    const bool hasEntity = g_main.selected < g_main.candidates.size();
    if (ImGui::Button("Record point here") && hasEntity) {
        const client::player::position::Snapshot player = client::player::position::snapshot();
        if (player.present) {
            client::spawn::MapPoint point{};
            point.tag = g_main.candidates[g_main.selected].tag;
            point.position = player.position;
            if (client::spawn::add_map_point(point)) {
                publish_map_points();
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Undo last")) {
        if (client::spawn::remove_last_map_point()) {
            publish_map_points();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear map")) {
        client::spawn::clear_map();
        publish_map_points();
    }

    if (ImGui::Button("Save map") && located) {
        (void)client::spawn::save_map(destination);
    }
    ImGui::SameLine();
    if (ImGui::Button("Load map") && located) {
        (void)client::spawn::load_map(destination);
        publish_map_points();
    }
    ImGui::TextDisabled(
        "Recording stamps the entity picked in the main spawner at your feet. Maps are saved per "
        "destination and reloaded with Load map. Filling picks the factions this world hosts, so "
        "the faction boxes below only affect the roaming modes.");
    if (ImGui::Button("Populate this destination") && located) {
        populate_this_destination(destination);
    }
    ImGui::TextDisabled(
        "One press: extract this destination's placements, fill them with the combatants this "
        "world hosts, save the map, and start placing. The steps below do it a piece at a time.");

    if (ImGui::Button("Extract authored placements") && located) {
        // Reads the destination's own placement records straight into the map.
        client::content::placements::ExtractResult extracted{};
        if (client::content::placements::extract(
                destination, g_extractCombatantsOnly, g_extractPublicOnly, extracted)) {
            g_extracted = extracted;
            g_extractedValid = true;
            publish_map_points();
        }
    }
    ImGui::SameLine();
    ImGui::Checkbox("Combatants only", &g_extractCombatantsOnly);
    ImGui::SameLine();
    ImGui::Checkbox("Public areas only", &g_extractPublicOnly);
    if (ImGui::Button("Fill positions with filtered combatants") && located) {
        (void)fill_with_world_combatants(destination);
    }
    if (g_batchActive) {
        // One destination per frame, so the game keeps drawing while the batch runs.
        step_batch();
        ImGui::TextDisabled("Batch running: %zu of %zu destinations. Keep this menu open.",
                            g_batchCursor,
                            g_batchLayouts.size());
        if (ImGui::Button("Stop batch")) {
            g_batchActive = false;
        }
    } else if (ImGui::Button("Extract every destination")) {
        start_batch();
    }
    if (g_batchValid) {
        ImGui::TextDisabled("Batch: %zu destinations, %zu maps written, %zu points, "
                            "%zu without placements, %zu without a roster (roster %zu entities)",
                            g_batch.destinations,
                            g_batch.written,
                            g_batch.points,
                            g_batch.skippedNoPlacements,
                            g_batch.skippedNoRoster,
                            g_roster.size());
        for (std::size_t index = 0; index < g_batchNoteCount; ++index) {
            ImGui::TextDisabled("  %s", g_batchNotes[index].data());
        }
        ImGui::TextDisabled(
            "Batch clears the held map. Use Load map to bring this destination's back.");
    }
}

/** Draws the world-population controls and publishes any change to the runtime. */
void draw_population() noexcept {
    if (!g_populationPrimed) {
        // The store published the saved settings at boot, so the panel opens showing them.
        g_population = native::population();
        g_populationSource = g_population.useMap ? PopulationSource::map : g_populationSource;
        g_populationPrimed = true;
    }
    if (!ImGui::CollapsingHeader("World population")) {
        return;
    }
    bool changed = false;
    changed = ImGui::Checkbox("Populate the world", &g_population.enabled) || changed;
    ImGui::SameLine();
    changed = ImGui::Checkbox("Populate on load", &g_population.autoOnLoad) || changed;
    ImGui::SameLine();
    ImGui::TextDisabled("%zu live  |  %zu source entities",
                        native::population_live(),
                        native::population_source_count());
    if (g_populationSource == PopulationSource::map) {
        constexpr std::array<const char*, 10> kOutcomes{"idle",
                                                       "placing",
                                                       "switched off",
                                                       "no player position",
                                                       "no points published",
                                                       "at live target",
                                                       "no point in range",
                                                       "entity not streamed in",
                                                       "no ground under point",
                                                       "spawn call failed"};
        const native::PopulationStatus status = native::population_status();
        const std::size_t outcome =
            static_cast<std::size_t>(status.last) < kOutcomes.size()
                ? static_cast<std::size_t>(status.last)
                : 0;
        if (status.nearest >= 0.0F) {
            ImGui::TextDisabled("Map: %zu points, nearest free %.0f units away, last step: %s",
                                status.points,
                                static_cast<double>(status.nearest),
                                kOutcomes[outcome]);
        } else {
            ImGui::TextDisabled("Map: %zu points, no free point measured, last step: %s",
                                status.points,
                                kOutcomes[outcome]);
        }
        // The two positions are printed together because the only way to tell an authored height
        // from a wrong one is to read it against where the player actually stands.
        ImGui::TextDisabled("You %.0f %.0f %.0f   last placement %.0f %.0f %.0f   ground moved it "
                            "%.1f",
                            static_cast<double>(status.player[0]),
                            static_cast<double>(status.player[1]),
                            static_cast<double>(status.player[2]),
                            static_cast<double>(status.placed[0]),
                            static_cast<double>(status.placed[1]),
                            static_cast<double>(status.placed[2]),
                            static_cast<double>(status.snapped));
    }

    int source = static_cast<int>(g_populationSource);
    bool sourceChanged = ImGui::RadioButton("Faction filter", &source, 0);
    ImGui::SameLine();
    sourceChanged = ImGui::RadioButton("Selected main entity", &source, 1) || sourceChanged;
    ImGui::SameLine();
    sourceChanged = ImGui::RadioButton("Recorded map", &source, 2) || sourceChanged;
    if (sourceChanged) {
        g_populationSource = static_cast<PopulationSource>(source);
        // Map mode fills recorded points; the other two roam a ring around the player.
        g_population.useMap = g_populationSource == PopulationSource::map;
        (void)client::spawn::publish_population(g_population);
    }
    if (g_populationSource == PopulationSource::map) {
        draw_recorder();
    }

    // Map mode fills its points from the same roster, so the filters belong on screen there too.
    if (g_populationSource != PopulationSource::selected) {
        for (std::size_t index = 0; index < kFactions.size(); ++index) {
            if (index != 0 && (index % 3) != 0) {
                ImGui::SameLine();
            }
            bool enabled = g_factionEnabled[index];
            if (ImGui::Checkbox(kFactions[index].label, &enabled)) {
                g_factionEnabled[index] = enabled;
                sourceChanged = true;
            }
        }
        sourceChanged = ImGui::Checkbox("Exclude ships and vehicles", &g_excludeVehicles)
                        || sourceChanged;
        ImGui::SameLine();
        sourceChanged = ImGui::Checkbox("Exclude champions", &g_excludeChampions) || sourceChanged;
        ImGui::SameLine();
        sourceChanged = ImGui::Checkbox("Exclude bosses", &g_excludeBosses) || sourceChanged;
    }
    if ((sourceChanged || ImGui::Button("Apply source"))
        && g_populationSource != PopulationSource::map) {
        publish_population_source();
    }
    ImGui::SameLine();
    if (ImGui::Button("Forget placed")) {
        native::clear_population_tracking();
    }

    int target = static_cast<int>(g_population.target);
    if (ImGui::SliderInt("Live count", &target, 1, 256)) {
        g_population.target = static_cast<std::uint32_t>(target);
        changed = true;
    }
    int interval = static_cast<int>(g_population.intervalMs);
    if (ImGui::SliderInt("Placement interval (ms)", &interval, 100, 5000)) {
        g_population.intervalMs = static_cast<std::uint32_t>(interval);
        changed = true;
    }
    int respawn = static_cast<int>(g_population.respawnDelayMs);
    if (ImGui::SliderInt("Respawn delay (ms)", &respawn, 0, 300000)) {
        g_population.respawnDelayMs = static_cast<std::uint32_t>(respawn);
        changed = true;
    }
    changed = ImGui::SliderFloat("Nearest distance", &g_population.minimumRadius, 0.0F, 120.0F)
              || changed;
    changed = ImGui::SliderFloat("Furthest distance", &g_population.maximumRadius, 0.0F, 400.0F)
              || changed;
    changed =
        ImGui::SliderFloat("Forget distance", &g_population.forgetRadius, 20.0F, 1200.0F) || changed;
    changed = ImGui::Checkbox("Snap recorded points to ground", &g_population.snapToGround)
              || changed;
    changed = ImGui::SliderFloat("Ground lift", &g_population.lift, 0.0F, 5.0F) || changed;
    changed = ImGui::SliderFloat("Entity scale", &g_population.scale, 0.1F, 5.0F) || changed;
    if (g_population.maximumRadius < g_population.minimumRadius) {
        g_population.maximumRadius = g_population.minimumRadius;
        changed = true;
    }
    // Forgetting closer than the placement reaches would free a point the moment it was filled,
    // so the same spot would be placed again and again while its entities piled up.
    const float forgetFloor = g_population.maximumRadius * 1.5F;
    if (g_population.forgetRadius < forgetFloor) {
        g_population.forgetRadius = forgetFloor;
        changed = true;
    }
    ImGui::TextDisabled(
        "Placed entities are never removed: the game offers no removal call. Leaving an area "
        "stops them being tracked, so they stay where they were left.");
    if (changed) {
        (void)client::spawn::publish_population(g_population);
    }
}

void draw() noexcept {
    if (!g_scanned) {
        refresh();
    }

    if (ImGui::Button("Refresh loaded entities")) {
        refresh();
        // The candidate lists just changed, so the populator's tags are re-taken from them.
        publish_population_source();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu main  |  %zu projectiles  |  %zu loot",
                        g_main.candidates.size(),
                        g_projectile.candidates.size(),
                        g_loot.candidates.size());
    if (native::busy()) {
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            native::cancel();
        }
    }

    constexpr ImGuiTableFlags flags = ImGuiTableFlags_SizingStretchSame
                                      | ImGuiTableFlags_BordersInnerV
                                      | ImGuiTableFlags_PadOuterX;
    spawn_keys::Keybinds keybinds = spawn_keys::get();
    bool keybindsChanged = false;
    if (ImGui::BeginTable("spawn_columns", 3, flags)) {
        ImGui::TableNextColumn();
        draw_column("Main spawner",
                    "main",
                    g_main,
                    spawn_keys::Action::mainPlayer,
                    spawn_keys::Action::mainCrosshair,
                    false,
                    keybinds,
                    keybindsChanged);
        ImGui::TableNextColumn();
        draw_column("Projectile spawner",
                    "projectile",
                    g_projectile,
                    spawn_keys::Action::projectilePlayer,
                    spawn_keys::Action::projectileCrosshair,
                    true,
                    keybinds,
                    keybindsChanged);
        ImGui::TableNextColumn();
        draw_column("Loot spawner",
                    "loot",
                    g_loot,
                    spawn_keys::Action::lootPlayer,
                    spawn_keys::Action::lootCrosshair,
                    true,
                    keybinds,
                    keybindsChanged);
        ImGui::EndTable();
    }
    if (keybindsChanged) {
        (void)spawn_keys::publish(keybinds);
    }
    ImGui::Separator();
    draw_population();
}

} // namespace sunrise::server::ui::spawn

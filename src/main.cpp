#include <Geode/Geode.hpp>
#include <Geode/modify/EditLevelLayer.hpp>
#include <Geode/modify/LevelInfoLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/utils/Keyboard.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numeric>
#include <string>
#include <string_view>
#include <vector>

using namespace geode::prelude;

namespace {
// The unique object ID for a Start Position object in Geometry Dash.
constexpr int START_POS_ID = 31;
// The size of the histogram. Records death rates from 0% to 99% in 1% increments, so it uses 100 slots.
constexpr int HISTOGRAM_SIZE = 100;
constexpr int START_POS_CHANGE_GRACE_FRAMES = 1;
// The number of suppression frames to prevent duplicate death recordings that occur simultaneously or at very short intervals. (Duplicate death prevention)
constexpr int DUPLICATE_DEATH_SUPPRESS_FRAMES = 1;
// The distance penalty value applied when a mismatch (deletion/insertion) occurs in the alignment algorithm
// when the previous start position list does not match the current list due to level edits.
// Simply put, this adjusts the death records accordingly when start positions change.
constexpr float START_POS_MATCH_SKIP_PENALTY = 300.f;
// The unique ID used to identify the death statistics popup UI node.
constexpr char const* POPUP_NODE_ID = "hyobeen.startpos-death-picker/death-stats-popup";
// The magic string recorded at the beginning of the saved statistics data file to identify it as this mod's data file.
constexpr char const* DATA_FILE_MAGIC = "SDP1";

// Type definition for an integer array (vector) that records the number of deaths at each position from 0% to 99%.
using Histogram = std::vector<int>;

// Clamps the death percent (float) to an integer between 0 and 99.
// If it's not a finite number or exceeds the valid range, it is adjusted to fit inside the range and returned safely.
int clampDeathPercent(float percent) {
    if (!std::isfinite(percent)) return 0;
    return std::clamp(static_cast<int>(std::floor(percent)), 0, HISTOGRAM_SIZE - 1);
}

// Extracts only the numeric value from the percentage text label displayed on screen (e.g., "45.7%"). Simply put, it removes the '%' sign.
float percentFromLabel(cocos2d::CCLabelBMFont* label) {
    if (!label || !label->getString()) return -1.f;

    std::string number;
    // Iterates through the label's string to extract only digits and the decimal point (.).
    for (auto ch : std::string_view(label->getString())) {
        if ((ch >= '0' && ch <= '9') || ch == '.') {
            number.push_back(ch);
        }
        else if (!number.empty()) {
            // Stop parsing if a non-numeric character (e.g., '%') is encountered after finding digits.
            break;
        }
    }

    if (number.empty()) return -1.f;
    try {
        // Converts the extracted numeric string to a float.
        return std::stof(number);
    }
    catch (...) {
        // Returns an error value (-1.f) if an exception occurs during conversion.
        return -1.f;
    }
}

// Structure to store death record data for each level.
struct TrackerData {
    std::vector<float> startXs;         // List of X coordinates for each start position (StartPos) in the level
    Histogram totalHist;                // Death statistics for the entire level (accumulated deaths regardless of start position)
    std::vector<Histogram> startHists;  // List of death statistics recorded separately for each start position
};

// Stable hash generation function (FNV-1a 64-bit algorithm).
// Generates a unique, fixed hash code based on the level string.
// This is used for mapping records to levels.
uint64_t stableHash(std::string_view text) {
    uint64_t hash = 14695981039346656037ull;
    for (auto ch : text) {
        hash ^= static_cast<uint8_t>(ch);
        hash *= 1099511628211ull;
    }
    return hash;
}

// Generates a unique save key name for the level.
// Creates a safe, unique key depending on the scenario, such as online server levels, imported editor levels, or local levels.
std::string stableLevelKey(GJGameLevel* level) {
    if (!level) return "unknown";

    // If it's an official level uploaded to the online server (Level ID is not 0)
    auto id = static_cast<int>(level->m_levelID.value());
    if (id != 0) return fmt::format("level_{}", id);

    // If it has a unique ID (M_ID) such as an editor copy or a multiplayer level
    if (level->m_M_ID != 0) return fmt::format("mid_{}", level->m_M_ID);

    // If it's a locally created custom level, combine the original ID and the level data hash
    auto original = static_cast<int>(level->m_originalLevel.value());
    if (original != 0) return fmt::format("local_original_{}_hash_{:016x}", original, stableHash(std::string_view(level->m_levelString)));

    // If level data exists, generate an identifier using only the data hash
    if (!level->m_levelString.empty()) {
        return fmt::format("hash_{:016x}", stableHash(std::string_view(level->m_levelString)));
    }

    return "unknown";
}

// Returns the absolute path of the text file where the mod data will be saved.
// It is set to the path "death-data/[unique level key].txt" under the save directory provided by Geode.
// As mentioned above, the hash is used for mapping.
std::filesystem::path trackerDataPath(GJGameLevel* level) {
    return Mod::get()->getSaveDir() / "death-data" / (stableLevelKey(level) + ".txt");
}

// Normalization function that adjusts the size of the histogram data to the standard (100 slots) and corrects negative values to 0.
Histogram normalizeHistogram(Histogram hist) {
    hist.resize(HISTOGRAM_SIZE, 0);
    for (auto& value : hist) {
        value = std::max(value, 0);
    }
    return hist;
}

// Collects and sorts only active StartPosObjects from the object array placed in the level.
std::vector<StartPosObject*> collectStartObjects(cocos2d::CCArray* objects) {
    std::vector<StartPosObject*> result;
    if (!objects) return result;

    // Use CCArrayExt to easily iterate over the Cocos2d CCArray with a C++ range-based loop.
    for (auto obj : CCArrayExt<GameObject*>(objects)) {
        // Determine if the object ID is 31 (StartPos).
        if (!obj || obj->m_objectID != START_POS_ID) continue;

        auto start = static_cast<StartPosObject*>(obj);
        // Exclude start positions that have the "Disable StartPos" option enabled from collection.
        if (start->m_startSettings && start->m_startSettings->m_disableStartPos) continue;
        result.push_back(start);
    }

    // Sort start positions in ascending order by X coordinate.
    // If the X coordinates are equal, sort by Y coordinate. If Y coordinates are also equal, sort by unique ID.
    std::sort(result.begin(), result.end(), [](StartPosObject* a, StartPosObject* b) {
        if (a->getPositionX() == b->getPositionX()) {
            if (a->getPositionY() == b->getPositionY()) {
                return a->m_uniqueID < b->m_uniqueID;
            }
            return a->getPositionY() < b->getPositionY();
        }
        return a->getPositionX() < b->getPositionX();
    });
    return result;
}

// Helper function that extracts only the X coordinate values (float) from the collected start position objects list.
std::vector<float> startPositionsFromObjects(std::vector<StartPosObject*> const& starts) {
    std::vector<float> result;
    result.reserve(starts.size());
    for (auto start : starts) {
        result.push_back(start->getPositionX());
    }
    return result;
}

// Lightweight utility function that splits a string by a specific delimiter. (Uses string_view to avoid copy overhead)
std::vector<std::string_view> split(std::string_view text, char delimiter) {
    std::vector<std::string_view> parts;
    size_t start = 0;
    while (start <= text.size()) {
        auto end = text.find(delimiter, start);
        if (end == std::string_view::npos) {
            parts.push_back(text.substr(start));
            break;
        }
        parts.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return parts;
}

// Converts a string to an integer (int), catching any exceptions if the format is invalid and returning a fallback value.
int parseInt(std::string_view text, int fallback = 0) {
    try {
        return std::stoi(std::string(text));
    }
    catch (...) {
        return fallback;
    }
}

// Converts a string to a float, catching any exceptions if the format is invalid and returning a fallback value.
float parseFloat(std::string_view text, float fallback = 0.f) {
    try {
        return std::stof(std::string(text));
    }
    catch (...) {
        return fallback;
    }
}

// Decompresses and parses the raw level string (levelString) directly to get the X coordinates of all start positions (StartPos) in the level.
// Used to quickly obtain the list of start positions in LevelInfoLayer without building the actual PlayLayer.
std::vector<float> collectStartPositionsFromLevelString(GJGameLevel* level) {
    std::vector<std::pair<float, float>> positions;
    if (!level || level->m_levelString.empty()) return {};

    // Since Geometry Dash level data is typically compressed, try decompressing it using ZipUtils.
    auto decompressed = cocos2d::ZipUtils::decompressString(level->m_levelString, false, 0);
    std::string_view levelStr = decompressed.empty() ? std::string_view(level->m_levelString) : std::string_view(decompressed);

    // Geometry Dash level data format: objects are separated by semicolons (;), and properties are specified as comma-separated (,) key-value pairs.
    // Example: "1,31,2,150.5,3,80.0;" -> Object with ID 31 (StartPos), X of 150.5, and Y of 80.0.
    for (auto object : split(levelStr, ';')) {
        auto fields = split(object, ',');
        if (fields.size() < 2) continue;

        int objectID = 0;
        float x = 0.f;
        float y = 0.f;
        bool hasID = false;
        bool hasX = false;

        // Even indices are property keys, odd indices are values.
        for (size_t i = 0; i + 1 < fields.size(); i += 2) {
            auto key = parseInt(fields[i]);
            if (key == 1) { // Key 1: Object ID
                objectID = parseInt(fields[i + 1]);
                hasID = true;
            }
            else if (key == 2) { // Key 2: X Coordinate
                x = parseFloat(fields[i + 1]);
                hasX = true;
            }
            else if (key == 3) { // Key 3: Y Coordinate
                y = parseFloat(fields[i + 1]);
            }
        }

        // Collect if the object ID is a start position (START_POS_ID = 31)
        if (hasID && hasX && objectID == START_POS_ID) {
            positions.emplace_back(x, y);
        }
    }

    // Sort the collected position information in ascending order.
    std::sort(positions.begin(), positions.end());

    std::vector<float> result;
    result.reserve(positions.size());
    for (auto const& [x, _] : positions) {
        result.push_back(x);
    }
    return result;
}

// Returns the bucket index representing where the active start position is located in the sorted start position list.
size_t findStartBucket(std::vector<StartPosObject*> const& starts, StartPosObject* activeStart) {
    if (!activeStart) return std::numeric_limits<size_t>::max();

    auto it = std::find(starts.begin(), starts.end(), activeStart);
    if (it == starts.end()) return std::numeric_limits<size_t>::max();

    return static_cast<size_t>(std::distance(starts.begin(), it));
}

// Crucial Algorithm: Alignment function that matches data when the previous start position list and current start position list differ.
// Uses Dynamic Programming (DP) to find the optimal matching path that minimizes the distance cost.
std::vector<Histogram> alignStartHistograms(
    std::vector<float> const& oldXs,           // List of old start position X coordinates recorded in the save file
    std::vector<Histogram> oldHists,           // List of old start position histograms recorded in the save file
    std::vector<float> const& currentStarts   // List of current start position X coordinates extracted from the current level
) {
    // Create an array to store the result, sized to the current start positions, and fill it with zeros.
    std::vector<Histogram> result(currentStarts.size(), Histogram(HISTOGRAM_SIZE, 0));
    for (auto& hist : oldHists) {
        hist = normalizeHistogram(std::move(hist));
    }

    // 1. No changes at all: Simply copy the statistics in order and return.
    if (oldXs == currentStarts) {
        for (size_t i = 0; i < currentStarts.size() && i < oldHists.size(); ++i) {
            result[i] = oldHists[i];
        }
        return result;
    }

    // 2. The coordinate array sizes differ, but the number of saved data entries matches the number of current start positions.
    if (oldXs.size() != oldHists.size() && oldHists.size() == currentStarts.size()) {
        for (size_t i = 0; i < currentStarts.size(); ++i) {
            result[i] = oldHists[i];
        }
        return result;
    }

    auto oldSize = std::min(oldXs.size(), oldHists.size());
    if (oldSize == 0 || currentStarts.empty()) return result;

    // 3. Start the Dynamic Programming-based alignment algorithm.
    // Similar to sequence alignment, it calculates the distance between the two coordinate lists as the cost.
    auto rows = oldSize + 1;
    auto cols = currentStarts.size() + 1;
    // DP table: stores the minimum accumulated distance cost (initialized to infinity)
    std::vector<float> dp(rows * cols, std::numeric_limits<float>::infinity());
    // Backtracking table: records which path (previous node) the path came from (1: Up, 2: Left, 3: Diagonal)
    std::vector<uint8_t> parent(rows * cols, 0);

    // Helper lambda function to convert a 2D array index to a 1D array index
    auto at = [cols](size_t i, size_t j) {
        return i * cols + j;
    };

    dp[at(0, 0)] = 0.f;
    for (size_t i = 0; i <= oldSize; ++i) {
        for (size_t j = 0; j <= currentStarts.size(); ++j) {
            auto base = dp[at(i, j)];
            if (!std::isfinite(base)) continue;

            // Path 1 (Skip Old): Ignore and skip the previous start position i. (Apply penalty)
            if (i < oldSize && base + START_POS_MATCH_SKIP_PENALTY < dp[at(i + 1, j)]) {
                dp[at(i + 1, j)] = base + START_POS_MATCH_SKIP_PENALTY;
                parent[at(i + 1, j)] = 1;
            }

            // Path 2 (Skip Current): Treat the current start position j as newly created and ignore it. (Apply penalty)
            if (j < currentStarts.size() && base + START_POS_MATCH_SKIP_PENALTY < dp[at(i, j + 1)]) {
                dp[at(i, j + 1)] = base + START_POS_MATCH_SKIP_PENALTY;
                parent[at(i, j + 1)] = 2;
            }

            // Path 3 (Match): Match the previous start position i with the current start position j. (The physical distance difference becomes the cost)
            if (i < oldSize && j < currentStarts.size()) {
                auto cost = std::abs(oldXs[i] - currentStarts[j]);
                if (base + cost < dp[at(i + 1, j + 1)]) {
                    dp[at(i + 1, j + 1)] = base + cost;
                    parent[at(i + 1, j + 1)] = 3;
                }
            }
        }
    }

    // 4. Backtrack from the bottom-right of the table (oldSize, currentStarts.size()) to retrieve the matching result
    auto i = oldSize;
    auto j = currentStarts.size();
    while (i > 0 || j > 0) {
        auto action = parent[at(i, j)];
        if (action == 3 && i > 0 && j > 0) {
            // Diagonal move: Previous start position i-1 matches current j-1! Copy statistics data.
            result[j - 1] = oldHists[i - 1];
            --i;
            --j;
        }
        else if (action == 2 && j > 0) {
            // Left move: Current start position j-1 is newly added, so leave the statistics empty.
            --j;
        }
        else if (action == 1 && i > 0) {
            // Up move: Previous start position i-1 was deleted, so discard it.
            --i;
        }
        else {
            break;
        }
    }

    return result;
}

// Reads the death record file (.txt) for a specific level from the disk.
// If the file is corrupted or does not exist, prepares a default structure.
TrackerData loadTrackerData(GJGameLevel* level, std::vector<float> currentStarts) {
    auto path = trackerDataPath(level);

    if (std::ifstream file(path); file) {
        std::string token;
        size_t count = 0;

        TrackerData fileData;
        // Check custom data file structure: [MAGIC] START_XS [Count] [Coordinates list...]
        if (file >> token && token == DATA_FILE_MAGIC &&
            file >> token && token == "START_XS" &&
            file >> count
        ) {
            fileData.startXs.resize(count);
            for (auto& value : fileData.startXs) file >> value;

            // [TOTAL] [List of 100 total death counts...]
            if (file >> token && token == "TOTAL") {
                fileData.totalHist.resize(HISTOGRAM_SIZE);
                for (auto& value : fileData.totalHist) file >> value;

                // [START_HISTS] [Start positions count] [HIST] [Each death list...]
                if (file >> token && token == "START_HISTS" && file >> count) {
                    fileData.startHists.assign(count, Histogram(HISTOGRAM_SIZE, 0));
                    bool valid = true;
                    for (auto& hist : fileData.startHists) {
                        file >> token;
                        if (token != "HIST") {
                            valid = false;
                            break;
                        }
                        for (auto& value : hist) file >> value;
                    }

                    // If the file parsing finishes successfully
                    if (valid && file) {
                        if (currentStarts.empty() && !fileData.startXs.empty()) {
                            currentStarts = fileData.startXs;
                        }
                        fileData.totalHist = normalizeHistogram(std::move(fileData.totalHist));
                        // Important: The level might have been edited, so align the loaded data with the current real-time start positions using dynamic programming.
                        fileData.startHists = alignStartHistograms(fileData.startXs, std::move(fileData.startHists), currentStarts);
                        fileData.startXs = currentStarts;
                        return fileData;
                    }
                }
            }
        }

        log::warn("Ignoring malformed Startpos Death Picker data file: {}", path.string());
    }

    // If the file does not exist or is invalid, returns a structure containing new, empty statistics data.
    TrackerData data;
    data.startXs = std::move(currentStarts);
    data.totalHist.assign(HISTOGRAM_SIZE, 0);
    data.startHists.assign(data.startXs.size(), Histogram(HISTOGRAM_SIZE, 0));
    return data;
}

// Helper function to write 100 integer values separated by spaces into a text file.
void writeHistogram(std::ofstream& file, Histogram const& hist) {
    for (auto i = 0; i < HISTOGRAM_SIZE; ++i) {
        auto value = i < hist.size() ? hist[i] : 0;
        file << ' ' << std::max(value, 0);
    }
}

// Safely saves (serializes) the currently accumulated death record data into a text file.
void saveTrackerData(GJGameLevel* level, TrackerData const& data) {
    auto path = trackerDataPath(level);
    std::error_code ec;
    // If the directory does not exist, create the nested folders.
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        log::warn("Failed to create Startpos Death Picker data directory: {}", ec.message());
        return;
    }

    std::ofstream file(path, std::ios::trunc);
    if (!file) {
        log::warn("Failed to open Startpos Death Picker data file: {}", path.string());
        return;
    }

    // Write text according to the specified serialization format
    file << DATA_FILE_MAGIC << '\n';
    file << "START_XS " << data.startXs.size();
    for (auto value : data.startXs) file << ' ' << value;
    file << "\nTOTAL";
    writeHistogram(file, data.totalHist);
    file << "\nSTART_HISTS " << data.startHists.size() << '\n';
    for (auto const& hist : data.startHists) {
        file << "HIST";
        writeHistogram(file, hist);
        file << '\n';
    }
}

// Calculates the sum of all death counts recorded in the histogram.
int histogramTotal(Histogram const& hist) {
    return std::accumulate(hist.begin(), hist.end(), 0);
}

// Returns the appropriate histogram for the requested category.
// Category 0 represents overall level deaths (totalHist), and anything above represents individual death statistics for each start position.
Histogram const& histogramForCategory(TrackerData const& data, size_t category) {
    if (category == 0 || category - 1 >= data.startHists.size()) {
        return data.totalHist;
    }
    return data.startHists[category - 1];
}

// Toggles and saves the graph rendering style (line graph toggle) in the settings, then returns the new state.
bool toggleGraphMode() {
    auto lineGraph = !Mod::get()->getSavedValue<bool>("line-graph", false);
    Mod::get()->setSavedValue("line-graph", lineGraph);
    return lineGraph;
}

// Cocos2d-x UI class definition that visualizes death statistics and displays them in a modal window.
class DeathStatsPopup : public Popup {
public:
    // Closes the popup when the D hotkey is pressed.
    void closeByHotkey() {
        this->onClose(nullptr);
    }

    // Switches to the previous start position statistics when the H hotkey is pressed.
    void moveLeft() {
        this->onPrev(nullptr);
    }

    // Switches to the next start position statistics when the L hotkey is pressed.
    void moveRight() {
        this->onNext(nullptr);
    }

    // Toggles between bar graph and line graph when the T hotkey is pressed, then rerenders.
    void toggleMode() {
        m_lineGraph = toggleGraphMode();
        this->render();
    }

    // Dispatch loop that handles key inputs inside the popup.
    bool handleHotkey(cocos2d::enumKeyCodes key) {
        if (key == cocos2d::KEY_D) {
            this->closeByHotkey();
            return true;
        }
        if (key == cocos2d::KEY_H) {
            this->moveLeft();
            return true;
        }
        if (key == cocos2d::KEY_L) {
            this->moveRight();
            return true;
        }
        if (key == cocos2d::KEY_T) {
            this->toggleMode();
            return true;
        }
        return false;
    }

protected:
    GJGameLevel* m_level = nullptr;       // Pointer to the target Geometry Dash level object
    TrackerData m_data;                   // Statistics data loaded from the save file
    size_t m_category = 0;                // Currently displayed start position index (0: entire level)
    bool m_lineGraph = false;             // Renders as a line graph if true, or a bar graph if false
    CCMenu* m_content = nullptr;          // Container node where UI content elements will be placed

    // Setup routine to initialize the popup in Geode SDK.
    bool setup(GJGameLevel* level) {
        if (!Popup::init(430.f, 265.f)) return false;

        this->setID(POPUP_NODE_ID);
        m_level = level;
        m_lineGraph = Mod::get()->getSavedValue<bool>("line-graph", false);

        // Dynamically add Geode's EventListener as a layer child to intercept keyboard inputs.
        this->addChild(geode::EventListenerNode::create(
            KeyboardInputEvent(),
            [this](KeyboardInputData& data) {
                if (data.action != KeyboardInputData::Action::Press) {
                    return ListenerResult::Propagate;
                }
                return this->handleHotkey(data.key)
                    ? ListenerResult::Stop
                    : ListenerResult::Propagate; // Stop key propagation if handled in the popup, otherwise propagate
            },
            Priority::VeryEarly
        ));

        // The default close button is replaced by the D hotkey, so remove it from the parent for layout cleanup.
        if (m_closeBtn) {
            m_closeBtn->removeFromParentAndCleanup(true);
            m_closeBtn = nullptr;
        }
        this->setTitle("Startpos Death Picker", "goldFont.fnt", .55f, 18.f);

        // Renders the screen after loading data.
        auto starts = collectStartPositionsFromLevelString(level);
        m_data = loadTrackerData(level, starts);
        this->render();
        return true;
    }

    // Handler to switch to the previous view
    void onPrev(cocos2d::CCObject*) {
        auto count = m_data.startHists.size() + 1;
        if (count == 0) return;
        m_category = (m_category + count - 1) % count;
        this->render();
    }

    // Handler to switch to the next view
    void onNext(cocos2d::CCObject*) {
        auto count = m_data.startHists.size() + 1;
        if (count == 0) return;
        m_category = (m_category + 1) % count;
        this->render();
    }

    // Renders the received statistics histogram graphically using a CCDrawNode canvas.
    void drawHistogram(Histogram const& hist, cocos2d::CCNode* parent) {
        auto graph = CCDrawNode::create();
        graph->setPosition({ 0.f, 0.f });
        parent->addChild(graph);

        // Define layout dimension parameters
        auto left = 28.f;
        auto bottom = 46.f;
        auto width = 374.f;
        auto height = 114.f;
        // Find the maximum value in the histogram and scale the vertical axis of the graph to match it.
        auto rawMax = *std::max_element(hist.begin(), hist.end());
        auto scaleMax = std::max(1, rawMax); // Prevent division by zero
        auto color = ccColor4F { .25f, .76f, 1.f, .9f }; // Main blue color tone

        // Draw the horizontal (X) and vertical (Y) guide lines.
        graph->drawSegment({ left, bottom }, { left + width, bottom }, .55f, { 1.f, 1.f, 1.f, .35f });
        graph->drawSegment({ left, bottom }, { left, bottom + height }, .55f, { 1.f, 1.f, 1.f, .25f });

        std::vector<CCPoint> points;
        points.reserve(HISTOGRAM_SIZE);
        auto step = width / static_cast<float>(HISTOGRAM_SIZE);
        auto barWidth = std::max(.45f, step * .24f);

        // Map data to drawing coordinates by iterating from 0% to 99%
        for (int i = 0; i < HISTOGRAM_SIZE; ++i) {
            auto x = left + step * (static_cast<float>(i) + .5f);
            auto y = bottom + height * (static_cast<float>(hist[i]) / static_cast<float>(scaleMax));
            points.push_back({ x, y });

            // In bar graph mode, draw rectangular polygons if the death count is 1 or more.
            if (!m_lineGraph && hist[i] > 0) {
                CCPoint verts[4] = {
                    { x - barWidth * .5f, bottom },
                    { x + barWidth * .5f, bottom },
                    { x + barWidth * .5f, y },
                    { x - barWidth * .5f, y },
                };
                graph->drawPolygon(verts, 4, color, .25f, { 0.f, 0.f, 0.f, .55f });
            }
        }

        // Connect points with lines and draw dots at death locations in line graph mode
        if (m_lineGraph) {
            for (size_t i = 1; i < points.size(); ++i) {
                graph->drawSegment(points[i - 1], points[i], 1.f, color);
            }
            for (size_t i = 0; i < points.size(); ++i) {
                if (hist[i] > 0) {
                    graph->drawDot(points[i], 1.7f, { 1.f, .92f, .25f, 1.f });
                }
            }
        }

        // Display X-axis scale labels (0%, 25%, 50%, 75%, 99%)
        for (auto [label, percent] : { std::pair { "0%", 0 }, { "25%", 25 }, { "50%", 50 }, { "75%", 75 }, { "99%", 99 } }) {
            auto x = left + step * (static_cast<float>(percent) + .5f);
            auto text = CCLabelBMFont::create(label, "goldFont.fnt");
            text->setScale(.22f);
            text->setPosition({ x, bottom - 12.f });
            parent->addChild(text);
        }

        // Display label for the maximum value
        auto maxLabel = CCLabelBMFont::create(fmt::format("max {}", rawMax).c_str(), "bigFont.fnt");
        maxLabel->setScale(.22f);
        maxLabel->setAnchorPoint({ 0.f, .5f });
        maxLabel->setPosition({ left + 4.f, bottom + height + 9.f });
        parent->addChild(maxLabel);
    }

    // Configures the layout of the popup's internal content and initiates rendering.
    void render() {
        if (m_content) {
            m_content->removeFromParentAndCleanup(true);
        }

        m_content = CCMenu::create();
        m_content->setPosition({ 0.f, 0.f });
        m_mainLayer->addChild(m_content);

        auto categoryCount = m_data.startHists.size() + 1;
        if (m_category >= categoryCount) m_category = 0;

        auto const& hist = histogramForCategory(m_data, m_category);
        auto title = m_category == 0 ? std::string("All Level") : fmt::format("Start Pos {}", m_category);
        auto summary = fmt::format("{}  |  deaths {}", title, histogramTotal(hist));

        // Render the title and death statistics summary text
        auto label = CCLabelBMFont::create(summary.c_str(), "bigFont.fnt");
        label->setScale(.34f);
        label->limitLabelWidth(270.f, .34f, .18f);
        label->setPosition({ 215.f, 212.f });
        m_content->addChild(label);

        // Call the function to draw the actual graph at the bottom
        this->drawHistogram(hist, m_content);
    }

    // Override keydown handler (forward to parent method if the hotkey isn't handled)
    void keyDown(cocos2d::enumKeyCodes key, double timestamp) override {
        if (this->handleHotkey(key)) {
            return;
        }

        Popup::keyDown(key, timestamp);
    }

public:
    // Factory creation method
    static DeathStatsPopup* create(GJGameLevel* level) {
        auto ret = new DeathStatsPopup();
        if (ret && ret->setup(level)) {
            ret->autorelease();
            return ret;
        }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }
};

// Checks if an instance of the statistics popup is currently open on the running scene, and returns its pointer if found.
DeathStatsPopup* activeStatsPopup() {
    auto scene = cocos2d::CCDirector::sharedDirector()->getRunningScene();
    if (!scene) return nullptr;

    auto node = scene->getChildByIDRecursive(POPUP_NODE_ID);
    if (!node) return nullptr;

    return static_cast<DeathStatsPopup*>(node);
}

// Global function that aggregates and handles hotkey inputs for opening/navigating the statistics popup from external layers (e.g., LevelInfoLayer).
bool handleStatsHotkey(cocos2d::enumKeyCodes key, GJGameLevel* level) {
    if (auto popup = activeStatsPopup()) {
        return popup->handleHotkey(key);
    }

    // If the D key is pressed when the popup is not open, instantiate and display the statistics popup.
    if (key == cocos2d::KEY_D) {
        if (auto popup = DeathStatsPopup::create(level)) {
            popup->show();
        }
        return true;
    }

    return false;
}

// Determines the progress percentage (0-99%) bucket at the moment of player death.
int deathPercentBucket(PlayLayer* layer, float deathX) {
    if (!layer) return 0;

    // Step 1: Attempt to parse the percentage text label displayed on screen first.
    auto labelPercent = percentFromLabel(layer->m_percentageLabel);
    if (labelPercent >= 0.f) {
        return clampDeathPercent(labelPercent);
    }

    // Step 2: Retrieve the precise in-game percentage supported by the API.
    auto apiPercent = layer->getCurrentPercent();
    if (std::isfinite(apiPercent) && apiPercent > 0.f) {
        return clampDeathPercent(apiPercent);
    }

    // Step 3: If neither is available, manually calculate the percentage based on the player's physical X position and the total level length.
    if (std::isfinite(deathX)) {
        auto levelLength = layer->m_levelLength > 0.f ? layer->m_levelLength : layer->m_maxObjectX;
        if (levelLength > 0.f && std::isfinite(levelLength)) {
            return clampDeathPercent((deathX / levelLength) * 100.f);
        }
    }

    return clampDeathPercent(apiPercent);
}
}

// Hook/extend the PlayLayer (gameplay scene) class using the Geode SDK modify macro.
class $modify(StartposDeathPickerPlayLayer, PlayLayer) {
    // Member variable fields unique to the hook class instance. (Automatic memory management by Geode)
    struct Fields {
        bool deathRecorded = false;              // Flag indicating if a death has already been recorded in the current attempt
        int lastRecordedAttempt = -1;            // The most recently recorded attempt number to prevent duplicates
        int duplicateDeathSuppressFrames = 0;    // Suppression counter to prevent duplicate processing from consecutive offset checks
        int startPosChangeFrames = 0;            // Grace period frame counter applied to death checks after switching start positions
        StartPosObject* lastStartPosObject = nullptr; // Pointer to the last active StartPosObject that was utilized
    };

    // Safely initialize members when entering the game layer.
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        m_fields->deathRecorded = false;
        m_fields->lastRecordedAttempt = -1;
        m_fields->duplicateDeathSuppressFrames = 0;
        m_fields->startPosChangeFrames = 0;
        m_fields->lastStartPosObject = m_startPosObject;
        return true;
    }

    // Decrements timer counters on every frame update loop.
    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);

        // If the user changed the start position in the middle (e.g., rapid inputs or search hotkeys)
        if (m_startPosObject != m_fields->lastStartPosObject) {
            m_fields->lastStartPosObject = m_startPosObject;
            m_fields->startPosChangeFrames = START_POS_CHANGE_GRACE_FRAMES; // Start grace counter
        }
        else if (m_fields->startPosChangeFrames > 0) {
            m_fields->startPosChangeFrames -= 1;
        }

        // Decrement the duplicate death suppression counter
        if (m_fields->duplicateDeathSuppressFrames > 0) {
            m_fields->duplicateDeathSuppressFrames -= 1;
        }
    }

    // Initialize flags when respawning normally on a new attempt
    void resetLevel() {
        m_fields->deathRecorded = false;
        m_fields->lastStartPosObject = m_startPosObject;
        m_fields->startPosChangeFrames = START_POS_CHANGE_GRACE_FRAMES;
        PlayLayer::resetLevel();
    }

    // Initialize flags when respawning from the very beginning (complete reset)
    void resetLevelFromStart() {
        m_fields->deathRecorded = false;
        m_fields->lastStartPosObject = m_startPosObject;
        m_fields->startPosChangeFrames = START_POS_CHANGE_GRACE_FRAMES;
        PlayLayer::resetLevelFromStart();
    }

    // Hook for the method triggered directly by the engine when the player dies by hitting an obstacle or wall.
    void destroyPlayer(PlayerObject* player, GameObject* object) {
        // Check if the death occurred while switching the start position (e.g., rapid hotkey presses).
        auto shouldIgnoreStartPosNavigation =
            m_startPosObject != m_fields->lastStartPosObject ||
            m_fields->startPosChangeFrames > 0;

        // Verify if this death event should be tracked normally.
        // (Must not be a duplicate recording, the player must not be already dead, and the level must not be completed)
        auto shouldTrack =
            !shouldIgnoreStartPosNavigation &&
            m_fields->duplicateDeathSuppressFrames == 0 &&
            !m_fields->deathRecorded &&
            m_fields->lastRecordedAttempt != m_attempts &&
            player &&
            !player->m_isDead &&
            m_player1 &&
            !m_player1->m_isDead &&
            !m_hasCompletedLevel;
        auto deathX = player ? player->getRealPosition().x : 0.f;
        auto percent = deathPercentBucket(this, deathX);
        auto activeStart = m_startPosObject;

        if (shouldTrack) {
            m_fields->deathRecorded = true;
            m_fields->lastRecordedAttempt = m_attempts;
            m_fields->duplicateDeathSuppressFrames = DUPLICATE_DEATH_SUPPRESS_FRAMES;
        }

        // First perform the parent (original game engine) destroyPlayer process to complete sound effects and state updates.
        PlayLayer::destroyPlayer(player, object);

        // If it's not a target for death statistics collection, stop here.
        if (!shouldTrack) return;

        // Load and analyze all start position information placed in the current level
        auto startObjects = collectStartObjects(m_objects);
        auto starts = startPositionsFromObjects(startObjects);
        auto data = loadTrackerData(m_level, starts);
        // Increment overall histogram death count
        data.totalHist[percent] += 1;

        // Determine the index bucket of the start position (StartPos) used at the moment of death.
        size_t bucket = data.startHists.size();
        auto activeStartBucket = findStartBucket(startObjects, activeStart);
        if (activeStartBucket < data.startHists.size()) {
            bucket = activeStartBucket;
        }
        else {
            // Special exception: If the active start position is missing or mismatched,
            // find the closest start position ahead of the player's physical death coordinate (deathX) using binary search (std::upper_bound).
            auto startIndex = std::distance(
                starts.begin(),
                std::upper_bound(starts.begin(), starts.end(), deathX)
            );
            if (startIndex > 0) {
                bucket = static_cast<size_t>(startIndex - 1);
            }
        }

        // Accumulate histogram data for the corresponding start position index.
        if (bucket < data.startHists.size()) {
            data.startHists[bucket][percent] += 1;
        }

        // Save the results directly to the text file.
        saveTrackerData(m_level, data);
    }
};

// Hook/extend the LevelInfoLayer (level info summary layout) class to control the statistics window via hotkeys.
class $modify(StartposDeathPickerLevelInfoLayer, LevelInfoLayer) {
    bool init(GJGameLevel* level, bool challenge) {
        if (!LevelInfoLayer::init(level, challenge)) return false;

        this->setKeyboardEnabled(true); // Enable keyboard detection for hotkeys
        return true;
    }

    void keyDown(cocos2d::enumKeyCodes key, double timestamp) {
        if (handleStatsHotkey(key, m_level)) {
            return;
        }

        LevelInfoLayer::keyDown(key, timestamp);
    }
};

// Similarly, attach the hotkey statistics window control hook to the EditLevelLayer (level editor layout) class.
class $modify(StartposDeathPickerEditLevelLayer, EditLevelLayer) {
    bool init(GJGameLevel* level) {
        if (!EditLevelLayer::init(level)) return false;

        this->setKeyboardEnabled(true); // Enable keyboard detection for hotkeys
        return true;
    }

    void keyDown(cocos2d::enumKeyCodes key, double timestamp) {
        if (handleStatsHotkey(key, m_level)) {
            return;
        }

        EditLevelLayer::keyDown(key, timestamp);
    }
};

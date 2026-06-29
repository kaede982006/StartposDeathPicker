#include <Geode/Geode.hpp>
#include <Geode/modify/EditLevelLayer.hpp>
#include <Geode/modify/LevelInfoLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/utils/Keyboard.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <string>
#include <string_view>
#include <vector>

using namespace geode::prelude;

namespace {
constexpr int START_POS_ID = 31;
constexpr int HISTOGRAM_SIZE = 100;
constexpr float START_POS_MATCH_SKIP_PENALTY = 300.f;
constexpr char const* POPUP_NODE_ID = "hyobeen.hdeathtracker/death-stats-popup";

using Histogram = std::vector<int>;

int clampDeathPercent(float percent) {
    if (!std::isfinite(percent)) return 0;
    return std::clamp(static_cast<int>(std::floor(percent)), 0, HISTOGRAM_SIZE - 1);
}

float percentFromLabel(cocos2d::CCLabelBMFont* label) {
    if (!label || !label->getString()) return -1.f;

    std::string number;
    for (auto ch : std::string_view(label->getString())) {
        if ((ch >= '0' && ch <= '9') || ch == '.') {
            number.push_back(ch);
        }
        else if (!number.empty()) {
            break;
        }
    }

    if (number.empty()) return -1.f;
    try {
        return std::stof(number);
    }
    catch (...) {
        return -1.f;
    }
}

struct TrackerData {
    std::vector<float> startXs;
    Histogram totalHist;
    std::vector<Histogram> startHists;
};

std::string levelKey(GJGameLevel* level) {
    if (!level) return "unknown";

    auto id = static_cast<int>(level->m_levelID.value());
    auto original = static_cast<int>(level->m_originalLevel.value());
    return fmt::format("{}:{}:{}:{}", id, original, level->m_M_ID, std::string(level->m_levelName));
}

std::string legacyLevelKey(GJGameLevel* level) {
    if (!level) return "unknown";

    auto id = static_cast<int>(level->m_levelID.value());
    auto original = static_cast<int>(level->m_originalLevel.value());
    return fmt::format("{}:{}:{}", id, original, std::string(level->m_levelName));
}

bool hasTrackerData(Mod* mod, std::string const& key) {
    return mod->hasSavedValue(key + ".total-hist") ||
        mod->hasSavedValue(key + ".start-hists") ||
        mod->hasSavedValue(key + ".start-xs") ||
        mod->hasSavedValue(key + ".total") ||
        mod->hasSavedValue(key + ".start-counts");
}

std::string readLevelKey(GJGameLevel* level) {
    auto mod = Mod::get();
    auto key = levelKey(level);
    if (hasTrackerData(mod, key)) return key;

    auto legacy = legacyLevelKey(level);
    if (hasTrackerData(mod, legacy)) return legacy;

    return key;
}

Histogram normalizeHistogram(Histogram hist) {
    hist.resize(HISTOGRAM_SIZE, 0);
    for (auto& value : hist) {
        value = std::max(value, 0);
    }
    return hist;
}

std::vector<StartPosObject*> collectStartObjects(cocos2d::CCArray* objects) {
    std::vector<StartPosObject*> result;
    if (!objects) return result;

    for (auto obj : CCArrayExt<GameObject*>(objects)) {
        if (!obj || obj->m_objectID != START_POS_ID) continue;

        auto start = static_cast<StartPosObject*>(obj);
        if (start->m_startSettings && start->m_startSettings->m_disableStartPos) continue;
        result.push_back(start);
    }

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

std::vector<float> startPositionsFromObjects(std::vector<StartPosObject*> const& starts) {
    std::vector<float> result;
    result.reserve(starts.size());
    for (auto start : starts) {
        result.push_back(start->getPositionX());
    }
    return result;
}

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

int parseInt(std::string_view text, int fallback = 0) {
    try {
        return std::stoi(std::string(text));
    }
    catch (...) {
        return fallback;
    }
}

float parseFloat(std::string_view text, float fallback = 0.f) {
    try {
        return std::stof(std::string(text));
    }
    catch (...) {
        return fallback;
    }
}

std::vector<float> collectStartPositionsFromLevelString(GJGameLevel* level) {
    std::vector<std::pair<float, float>> positions;
    if (!level || level->m_levelString.empty()) return {};

    auto decompressed = cocos2d::ZipUtils::decompressString(level->m_levelString, false, 0);
    std::string_view levelStr = decompressed.empty() ? std::string_view(level->m_levelString) : std::string_view(decompressed);

    for (auto object : split(levelStr, ';')) {
        auto fields = split(object, ',');
        if (fields.size() < 2) continue;

        int objectID = 0;
        float x = 0.f;
        float y = 0.f;
        bool hasID = false;
        bool hasX = false;

        for (size_t i = 0; i + 1 < fields.size(); i += 2) {
            auto key = parseInt(fields[i]);
            if (key == 1) {
                objectID = parseInt(fields[i + 1]);
                hasID = true;
            }
            else if (key == 2) {
                x = parseFloat(fields[i + 1]);
                hasX = true;
            }
            else if (key == 3) {
                y = parseFloat(fields[i + 1]);
            }
        }

        if (hasID && hasX && objectID == START_POS_ID) {
            positions.emplace_back(x, y);
        }
    }

    std::sort(positions.begin(), positions.end());

    std::vector<float> result;
    result.reserve(positions.size());
    for (auto const& [x, _] : positions) {
        result.push_back(x);
    }
    return result;
}

size_t findStartBucket(std::vector<StartPosObject*> const& starts, StartPosObject* activeStart) {
    if (!activeStart) return std::numeric_limits<size_t>::max();

    auto it = std::find(starts.begin(), starts.end(), activeStart);
    if (it == starts.end()) return std::numeric_limits<size_t>::max();

    return static_cast<size_t>(std::distance(starts.begin(), it));
}

std::vector<Histogram> alignStartHistograms(
    std::vector<float> const& oldXs,
    std::vector<Histogram> oldHists,
    std::vector<float> const& currentStarts
) {
    std::vector<Histogram> result(currentStarts.size(), Histogram(HISTOGRAM_SIZE, 0));
    for (auto& hist : oldHists) {
        hist = normalizeHistogram(std::move(hist));
    }

    if (oldXs == currentStarts) {
        for (size_t i = 0; i < currentStarts.size() && i < oldHists.size(); ++i) {
            result[i] = oldHists[i];
        }
        return result;
    }

    if (oldXs.size() != oldHists.size() && oldHists.size() == currentStarts.size()) {
        for (size_t i = 0; i < currentStarts.size(); ++i) {
            result[i] = oldHists[i];
        }
        return result;
    }

    auto oldSize = std::min(oldXs.size(), oldHists.size());
    if (oldSize == 0 || currentStarts.empty()) return result;

    auto rows = oldSize + 1;
    auto cols = currentStarts.size() + 1;
    std::vector<float> dp(rows * cols, std::numeric_limits<float>::infinity());
    std::vector<uint8_t> parent(rows * cols, 0);

    auto at = [cols](size_t i, size_t j) {
        return i * cols + j;
    };

    dp[at(0, 0)] = 0.f;
    for (size_t i = 0; i <= oldSize; ++i) {
        for (size_t j = 0; j <= currentStarts.size(); ++j) {
            auto base = dp[at(i, j)];
            if (!std::isfinite(base)) continue;

            if (i < oldSize && base + START_POS_MATCH_SKIP_PENALTY < dp[at(i + 1, j)]) {
                dp[at(i + 1, j)] = base + START_POS_MATCH_SKIP_PENALTY;
                parent[at(i + 1, j)] = 1;
            }

            if (j < currentStarts.size() && base + START_POS_MATCH_SKIP_PENALTY < dp[at(i, j + 1)]) {
                dp[at(i, j + 1)] = base + START_POS_MATCH_SKIP_PENALTY;
                parent[at(i, j + 1)] = 2;
            }

            if (i < oldSize && j < currentStarts.size()) {
                auto cost = std::abs(oldXs[i] - currentStarts[j]);
                if (base + cost < dp[at(i + 1, j + 1)]) {
                    dp[at(i + 1, j + 1)] = base + cost;
                    parent[at(i + 1, j + 1)] = 3;
                }
            }
        }
    }

    auto i = oldSize;
    auto j = currentStarts.size();
    while (i > 0 || j > 0) {
        auto action = parent[at(i, j)];
        if (action == 3 && i > 0 && j > 0) {
            result[j - 1] = oldHists[i - 1];
            --i;
            --j;
        }
        else if (action == 2 && j > 0) {
            --j;
        }
        else if (action == 1 && i > 0) {
            --i;
        }
        else {
            break;
        }
    }

    return result;
}

TrackerData loadTrackerData(GJGameLevel* level, std::vector<float> currentStarts) {
    auto mod = Mod::get();
    auto key = readLevelKey(level);

    auto oldXs = mod->getSavedValue<std::vector<float>>(key + ".start-xs");
    auto oldStartHists = mod->getSavedValue<std::vector<Histogram>>(key + ".start-hists");
    if (currentStarts.empty() && !oldXs.empty()) {
        currentStarts = oldXs;
    }

    TrackerData data;
    data.startXs = currentStarts;
    data.totalHist = normalizeHistogram(mod->getSavedValue<Histogram>(key + ".total-hist"));
    data.startHists = alignStartHistograms(oldXs, oldStartHists, currentStarts);
    return data;
}

void saveTrackerData(GJGameLevel* level, TrackerData const& data) {
    auto key = levelKey(level);
    auto mod = Mod::get();

    mod->setSavedValue(key + ".start-xs", data.startXs);
    mod->setSavedValue(key + ".total-hist", data.totalHist);
    mod->setSavedValue(key + ".start-hists", data.startHists);
}

int histogramTotal(Histogram const& hist) {
    return std::accumulate(hist.begin(), hist.end(), 0);
}

Histogram const& histogramForCategory(TrackerData const& data, size_t category) {
    if (category == 0 || category - 1 >= data.startHists.size()) {
        return data.totalHist;
    }
    return data.startHists[category - 1];
}

bool toggleGraphMode() {
    auto lineGraph = !Mod::get()->getSavedValue<bool>("line-graph", false);
    Mod::get()->setSavedValue("line-graph", lineGraph);
    return lineGraph;
}

class DeathStatsPopup : public Popup {
public:
    void closeByHotkey() {
        this->onClose(nullptr);
    }

    void moveLeft() {
        this->onPrev(nullptr);
    }

    void moveRight() {
        this->onNext(nullptr);
    }

    void toggleMode() {
        m_lineGraph = toggleGraphMode();
        this->render();
    }

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
    GJGameLevel* m_level = nullptr;
    TrackerData m_data;
    size_t m_category = 0;
    bool m_lineGraph = false;
    CCMenu* m_content = nullptr;

    bool setup(GJGameLevel* level) {
        if (!Popup::init(430.f, 265.f)) return false;

        this->setID(POPUP_NODE_ID);
        m_level = level;
        m_lineGraph = Mod::get()->getSavedValue<bool>("line-graph", false);
        this->addChild(geode::EventListenerNode::create(
            KeyboardInputEvent(),
            [this](KeyboardInputData& data) {
                if (data.action != KeyboardInputData::Action::Press) {
                    return ListenerResult::Propagate;
                }
                return this->handleHotkey(data.key)
                    ? ListenerResult::Stop
                    : ListenerResult::Propagate;
            },
            Priority::VeryEarly
        ));
        if (m_closeBtn) {
            m_closeBtn->removeFromParentAndCleanup(true);
            m_closeBtn = nullptr;
        }
        this->setTitle("Death Tracker", "goldFont.fnt", .55f, 18.f);

        auto starts = collectStartPositionsFromLevelString(level);
        m_data = loadTrackerData(level, starts);
        this->render();
        return true;
    }

    void onPrev(cocos2d::CCObject*) {
        auto count = m_data.startHists.size() + 1;
        if (count == 0) return;
        m_category = (m_category + count - 1) % count;
        this->render();
    }

    void onNext(cocos2d::CCObject*) {
        auto count = m_data.startHists.size() + 1;
        if (count == 0) return;
        m_category = (m_category + 1) % count;
        this->render();
    }

    void drawHistogram(Histogram const& hist, cocos2d::CCNode* parent) {
        auto graph = CCDrawNode::create();
        graph->setPosition({ 0.f, 0.f });
        parent->addChild(graph);

        auto left = 28.f;
        auto bottom = 46.f;
        auto width = 374.f;
        auto height = 114.f;
        auto rawMax = *std::max_element(hist.begin(), hist.end());
        auto scaleMax = std::max(1, rawMax);
        auto color = ccColor4F { .25f, .76f, 1.f, .9f };

        graph->drawSegment({ left, bottom }, { left + width, bottom }, .55f, { 1.f, 1.f, 1.f, .35f });
        graph->drawSegment({ left, bottom }, { left, bottom + height }, .55f, { 1.f, 1.f, 1.f, .25f });

        std::vector<CCPoint> points;
        points.reserve(HISTOGRAM_SIZE);
        auto step = width / static_cast<float>(HISTOGRAM_SIZE);
        auto barWidth = std::max(.45f, step * .24f);

        for (int i = 0; i < HISTOGRAM_SIZE; ++i) {
            auto x = left + step * (static_cast<float>(i) + .5f);
            auto y = bottom + height * (static_cast<float>(hist[i]) / static_cast<float>(scaleMax));
            points.push_back({ x, y });

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

        for (auto [label, percent] : { std::pair { "0%", 0 }, { "25%", 25 }, { "50%", 50 }, { "75%", 75 }, { "99%", 99 } }) {
            auto x = left + step * (static_cast<float>(percent) + .5f);
            auto text = CCLabelBMFont::create(label, "goldFont.fnt");
            text->setScale(.22f);
            text->setPosition({ x, bottom - 12.f });
            parent->addChild(text);
        }

        auto maxLabel = CCLabelBMFont::create(fmt::format("max {}", rawMax).c_str(), "bigFont.fnt");
        maxLabel->setScale(.22f);
        maxLabel->setAnchorPoint({ 0.f, .5f });
        maxLabel->setPosition({ left + 4.f, bottom + height + 9.f });
        parent->addChild(maxLabel);
    }

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

        auto label = CCLabelBMFont::create(summary.c_str(), "bigFont.fnt");
        label->setScale(.34f);
        label->limitLabelWidth(270.f, .34f, .18f);
        label->setPosition({ 215.f, 212.f });
        m_content->addChild(label);

        this->drawHistogram(hist, m_content);
    }

    void keyDown(cocos2d::enumKeyCodes key, double timestamp) override {
        if (this->handleHotkey(key)) {
            return;
        }

        Popup::keyDown(key, timestamp);
    }

public:
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

DeathStatsPopup* activeStatsPopup() {
    auto scene = cocos2d::CCDirector::sharedDirector()->getRunningScene();
    if (!scene) return nullptr;

    auto node = scene->getChildByIDRecursive(POPUP_NODE_ID);
    if (!node) return nullptr;

    return static_cast<DeathStatsPopup*>(node);
}

bool handleStatsHotkey(cocos2d::enumKeyCodes key, GJGameLevel* level) {
    if (auto popup = activeStatsPopup()) {
        return popup->handleHotkey(key);
    }

    if (key == cocos2d::KEY_D) {
        if (auto popup = DeathStatsPopup::create(level)) {
            popup->show();
        }
        return true;
    }

    return false;
}

int deathPercentBucket(PlayLayer* layer, float deathX) {
    if (!layer) return 0;

    auto labelPercent = percentFromLabel(layer->m_percentageLabel);
    if (labelPercent >= 0.f) {
        return clampDeathPercent(labelPercent);
    }

    auto apiPercent = layer->getCurrentPercent();
    if (std::isfinite(apiPercent) && apiPercent > 0.f) {
        return clampDeathPercent(apiPercent);
    }

    if (std::isfinite(deathX)) {
        auto levelLength = layer->m_levelLength > 0.f ? layer->m_levelLength : layer->m_maxObjectX;
        if (levelLength > 0.f && std::isfinite(levelLength)) {
            return clampDeathPercent((deathX / levelLength) * 100.f);
        }
    }

    return clampDeathPercent(apiPercent);
}
}

class $modify(HDeathTrackerPlayLayer, PlayLayer) {
    struct Fields {
        bool deathRecorded = false;
    };

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        m_fields->deathRecorded = false;
        return true;
    }

    void resetLevel() {
        m_fields->deathRecorded = false;
        PlayLayer::resetLevel();
    }

    void resetLevelFromStart() {
        m_fields->deathRecorded = false;
        PlayLayer::resetLevelFromStart();
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        auto shouldTrack =
            !m_fields->deathRecorded &&
            player &&
            !player->m_isDead &&
            m_player1 &&
            !m_player1->m_isDead &&
            !m_hasCompletedLevel;
        auto deathX = player ? player->getRealPosition().x : 0.f;
        auto percent = deathPercentBucket(this, deathX);
        auto activeStart = m_startPosObject;

        PlayLayer::destroyPlayer(player, object);

        if (!shouldTrack) return;
        m_fields->deathRecorded = true;

        auto startObjects = collectStartObjects(m_objects);
        auto starts = startPositionsFromObjects(startObjects);
        auto data = loadTrackerData(m_level, starts);
        data.totalHist[percent] += 1;

        size_t bucket = data.startHists.size();
        auto activeStartBucket = findStartBucket(startObjects, activeStart);
        if (activeStartBucket < data.startHists.size()) {
            bucket = activeStartBucket;
        }
        else {
            auto startIndex = std::distance(
                starts.begin(),
                std::upper_bound(starts.begin(), starts.end(), deathX)
            );
            if (startIndex > 0) {
                bucket = static_cast<size_t>(startIndex - 1);
            }
        }

        if (bucket < data.startHists.size()) {
            data.startHists[bucket][percent] += 1;
        }

        saveTrackerData(m_level, data);
    }
};

class $modify(HDeathTrackerLevelInfoLayer, LevelInfoLayer) {
    bool init(GJGameLevel* level, bool challenge) {
        if (!LevelInfoLayer::init(level, challenge)) return false;

        this->setKeyboardEnabled(true);
        return true;
    }

    void keyDown(cocos2d::enumKeyCodes key, double timestamp) {
        if (handleStatsHotkey(key, m_level)) {
            return;
        }

        LevelInfoLayer::keyDown(key, timestamp);
    }
};

class $modify(HDeathTrackerEditLevelLayer, EditLevelLayer) {
    bool init(GJGameLevel* level) {
        if (!EditLevelLayer::init(level)) return false;

        this->setKeyboardEnabled(true);
        return true;
    }

    void keyDown(cocos2d::enumKeyCodes key, double timestamp) {
        if (handleStatsHotkey(key, m_level)) {
            return;
        }

        EditLevelLayer::keyDown(key, timestamp);
    }
};

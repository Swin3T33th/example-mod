#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <vector>
#include <utility>

using namespace geode::prelude;

static int g_frame = 0;
static int g_lastClick = -1;
static int g_prevClick = -1;
static std::vector<std::pair<int, bool>> g_clicks;

class $modify(FrameBase, GJBaseGameLayer) {
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        if (PlayLayer::get() && !isHalfTick) g_frame++;
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
        if (PlayLayer::get() && isPlayer1 && button == 1) {
            g_clicks.push_back({g_frame, down});
            if (down) {
                g_prevClick = g_lastClick;
                g_lastClick = g_frame;
            }
        }
    }
};

class $modify(FrameLayer, PlayLayer) {
    struct Fields {
        CCLabelBMFont* label = nullptr;
    };

    void resetCounters() {
        g_frame = 0;
        g_lastClick = -1;
        g_prevClick = -1;
        g_clicks.clear();
    }

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        resetCounters();
        auto label = CCLabelBMFont::create("Frame: 0", "bigFont.fnt");
        label->setScale(0.4f);
        label->setAnchorPoint({0.f, 1.f});
        label->setPosition({5.f, CCDirector::get()->getWinSize().height - 5.f});
        label->setZOrder(100);
        this->addChild(label);
        m_fields->label = label;
        return true;
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        if (!m_fields->label) return;
        std::string text = fmt::format("Frame: {}", g_frame);
        if (g_lastClick >= 0) {
            text += fmt::format("\nClic: {}", g_lastClick);
            if (g_prevClick >= 0) {
                text += fmt::format(" (+{})", g_lastClick - g_prevClick);
            }
            text += fmt::format("\nTotal: {}", g_clicks.size());
        }
        m_fields->label->setString(text.data());
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        resetCounters();
    }
};

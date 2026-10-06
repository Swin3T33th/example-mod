#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <array>

using namespace geode::prelude;

static int g_frame = 0;
static int g_lastClick = -1;
static std::array<int, 7> g_counts = {0, 0, 0, 0, 0, 0, 0};

static const char* g_names[7] = {"9+", "7-8", "5-6", "4", "3", "2", "1"};
static const ccColor3B g_colors[7] = {
    {140, 120, 255}, {110, 190, 255}, {110, 230, 130}, {255, 255, 255},
    {240, 240, 90}, {255, 170, 80}, {240, 80, 90}
};

static int rowForWidth(int w) {
    if (w >= 9) return 0;
    if (w >= 7) return 1;
    if (w >= 5) return 2;
    if (w == 4) return 3;
    if (w == 3) return 4;
    if (w == 2) return 5;
    return 6;
}

class $modify(FrameBase, GJBaseGameLayer) {
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        if (PlayLayer::get() && !isHalfTick) g_frame++;
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
        if (PlayLayer::get() && isPlayer1 && button == 1 && down) {
            if (g_lastClick >= 0) {
                // TEMPORAL: solo para probar el diseño de la tabla.
                // Usa los frames entre clics, NO la ventana real.
                int gap = g_frame - g_lastClick;
                g_counts[rowForWidth(gap)]++;
            }
            g_lastClick = g_frame;
        }
    }
};

class $modify(FrameLayer, PlayLayer) {
    struct Fields {
        CCLabelBMFont* frameLabel = nullptr;
        std::array<CCLabelBMFont*, 7> rows = {};
    };

    void resetCounters() {
        g_frame = 0;
        g_lastClick = -1;
        g_counts.fill(0);
    }

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        resetCounters();

        CCNode* parent = this;
        if (m_uiLayer) parent = m_uiLayer;
        auto winSize = CCDirector::get()->getWinSize();

        auto fl = CCLabelBMFont::create("Frame: 0", "bigFont.fnt");
        fl->setScale(0.45f);
        fl->setAnchorPoint({0.f, 1.f});
        fl->setPosition({8.f, winSize.height - 6.f});
        fl->setZOrder(100);
        parent->addChild(fl);
        m_fields->frameLabel = fl;

        for (int i = 0; i < 7; i++) {
            std::string s = fmt::format("{}: 0", g_names[i]);
            auto row = CCLabelBMFont::create(s.data(), "bigFont.fnt");
            row->setScale(0.6f);
            row->setAnchorPoint({0.f, 1.f});
            row->setPosition({8.f, winSize.height - 30.f - i * 24.f});
            row->setColor(g_colors[i]);
            row->setZOrder(100);
            parent->addChild(row);
            m_fields->rows[i] = row;
        }
        return true;
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        if (m_fields->frameLabel) {
            std::string s = fmt::format("Frame: {}", g_frame);
            m_fields->frameLabel->setString(s.data());
        }
        for (int i = 0; i < 7; i++) {
            if (m_fields->rows[i]) {
                std::string s = fmt::format("{}: {}", g_names[i], g_counts[i]);
                m_fields->rows[i]->setString(s.data());
            }
        }
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        resetCounters();
    }
};
